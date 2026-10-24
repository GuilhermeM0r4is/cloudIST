#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>


#include "parser.h"
#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"

/* O_CLOEXEC prevents child processes (VMs) from inheriting the .conf descriptor.
   If the platform does not define it, it defaults to 0 (legacy behavior).*/
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define CLOUDIST_ROOT_DIR "/tmp/CloudIST"


// Necessary function to prevent orphaned VMs from continuing to run
static int has_running(DataCenter *dc) {
    for (size_t i = 0; i < dc->num_reservations; i++)
        if (dc->reservations[i].state == RES_STATE_RUNNING) {
            return 1;
        }
    return 0;
}

/**
 * Creates /tmp/CloudIST if it does not exist yet.
 *
 * Done once in main(), before any worker exists, so that two workers can
 * never race to create it. copy_dir_recursive() only creates the LAST
 * component of the destination path, so the parents must already exist.
 *
 * @return 0 on success (including "already exists"), 1 on error.
 */
static int ensure_root_dir(void) {
    if (mkdir(CLOUDIST_ROOT_DIR, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1
        && errno != EEXIST) {
        perror("mkdir " CLOUDIST_ROOT_DIR);
        return 1;
    }
    return 0;
}


/**
 * Processes ONE .conf file: reads and executes its commands, in order,
 * until the end of the file.
 *
 * Only talks to the Data Center through the datacenter_*() functions
 * (plus check_all_finished_vms), never touching its fields directly.
 * Every other variable is local, so it can be called by several threads
 * at the same time, each with its own file.
 *
 * @param filepath Path of the .conf file.
 * @param dc Pointer to the (shared) Data Center.
 *
 * @return 0 if the file was processed until EOC.
 * @return 1 if the file could not be opened.
 */
static int process_file(const char *filepath, DataCenter *dc) {
	// fd as file descriptor, or calling it "this open file"
	int fd = open(filepath, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Failed to open file: %s\n", filepath);
		return 1;
	}
 
	int done = 0;
	while (!done) {
		// Check the VMs BEFORE executing the commands
		check_all_finished_vms(dc);
		Command cmd = get_next_command(fd);
 
		switch (cmd) {
			case CMD_DEFINE: {
				VMType vmtype;
 
				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}
 
				if(datacenter_define_VM(dc, &vmtype) != 0){
					fprintf(stderr, "Failed to define VM.\n");
					continue;
				}
 
				printf("VM successfully defined!\n");
 
				break;
			}
 
			case CMD_RESERVE: {
				Reservation reservation = {0};
 
				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);
 
				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}
 
				if (datacenter_reserve(dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs.\n");
					continue;
				}
 
				printf("Reservation made successfully!\n");
 
				break;
			}
 
			case CMD_EXECUTE: {
				char id[MAX_STRING_SIZE];
 
				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}
 
				if (datacenter_execute(dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}
 
				printf("Finished reservation execution!\n");
 
				break;
			}
 
			case CMD_LIST: {
				if (datacenter_list(dc) != 0) {
					fprintf(stderr, "Failed to list VMs.\n");
					continue;
				}
 
				break;
			}
 
			case CMD_WAIT: {
				unsigned int delay;
 
				if(parse_wait(fd, &delay) != 0){
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}
 
				datacenter_wait(delay);
				break;
			}
 
			case CMD_INVALID: {
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;
			}
 
			case CMD_HELP: {
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				break;
			}
 
			case CMD_EMPTY: {
				break;
			}
 
			// if we get into the End of Commands lines we shall stop
			case EOC: {
				done = 1;
				break;
			}
		}
	}
 
	close(fd);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Worker pool                                                       */
/* ------------------------------------------------------------------ */

/**
 * Work shared by all workers: the sorted list of .conf files and the index
 * of the next file to hand out
 *
 * Only `next` is ever modified while workers run, and only under `lock`
 * Everything else is read-only after setup, so it needs no synchronization
 * The Data Center is NOT protected here: it is accessed exclusively through
 * the datacenter_*() functions, which own their synchronization
 */
typedef struct {
	DataCenter *dc;
	const char *input_dir;
	struct dirent **files;
	int n;                    // number of files
	int next;                 // next index to hand out (protected by lock)
	pthread_mutex_t lock;
} WorkQueue;

/**
 * Hands out the next file index. Each index is returned exactly once, to
 * exactly one worker, which is what guarantees "every file processed once"
 *
 * @return Index of the file to process, or -1 when there is no more work
 */
static int next_file(WorkQueue *q) {
	int i = -1;
 
	pthread_mutex_lock(&q->lock);
	if (q->next < q->n) {
		i = q->next++;
	}
	pthread_mutex_unlock(&q->lock);
 
	return i;
}

/**
 * Worker body: keeps taking files from the queue until it is empty
 * A worker that finishes early simply takes the next file, so a slow
 * .conf (e.g. one with a long E command) never idles the other workers
 */
static void *worker_main(void *arg) {
	WorkQueue *q = arg;
	int i;
 
	while ((i = next_file(q)) >= 0) {
		char filepath[MAX_PATH_SIZE + 256];
 
		snprintf(filepath, sizeof(filepath), "%s/%s", q->input_dir, q->files[i]->d_name);
 
		// A file that fails to open is reported inside; the others still run
		process_file(filepath, q->dc);
	}
 
	return NULL;
}

/**
 * Runs the queue with at most max_threads workers and returns only when
 * every worker has finished (all files processed)
 *
 * Never creates more workers than there are files. If a thread cannot be
 * created the remaining workers (or, as a last resort, the calling thread)
 * simply drain the queue, so no file is ever skipped
 */
static void run_workers(WorkQueue *q, size_t max_threads) {
	size_t num_threads = max_threads < (size_t)q->n ? max_threads : (size_t)q->n;
 
	if (num_threads == 0) {
		return; // no .conf files
	}
 
	pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
	size_t created = 0;
 
	if (threads != NULL) {
		while (created < num_threads) {
			if (pthread_create(&threads[created], NULL, worker_main, q) != 0) {
				fprintf(stderr, "Failed to create worker thread %zu.\n", created + 1);
				break;
			}
			created++;
		}
	} else {
		fprintf(stderr, "Failed to allocate worker threads.\n");
	}
 
	// Could not start a single worker: do the work in the calling thread
	if (created == 0) {
		worker_main(q);
	}
 
	for (size_t i = 0; i < created; i++) {
		pthread_join(threads[i], NULL);
	}
 
	free(threads);
}



int main(int argc, char **argv){
	if (argc != 7) {
		fprintf(stderr,
		        "Usage: %s <servers> <ram> <disk> <cpus> <input_dir> <max_threads>\n",
		        argv[0]);
		return 1;
	}

	// Every variable is declared up here so that all error paths can
	// jump to the single cleanup block at the end of main()
	DataCenter dc;
	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t max_threads;
	const char *input_dir = argv[5];
	struct dirent **files = NULL;
	int n = -1;                 // -1 means scandir did not succeed
	int exit_code = 1;

	datacenter_init(&dc);

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		goto cleanup;
	}

	if (parse_size_t_arg(argv[6], &max_threads) != 0 || max_threads == 0) {
		fprintf(stderr, "Invalid MAX_THREADS: must be an integer greater than 0.\n");
		goto cleanup;
	}
	 
	if (strlen(input_dir) >= MAX_PATH_SIZE) {
		fprintf(stderr, "Input directory path too long.\n");
		goto cleanup;
	}

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		goto cleanup;
	}

	if (!path_exists(input_dir)) {
		fprintf(stderr, "Input directory does not exist: %s\n", input_dir);
		goto cleanup;
	}

	if (ensure_root_dir() != 0) {
		goto cleanup;
	}

	// .conf files, already sorted alphabetically
	n = scandir(input_dir, &files, is_conf_file, alphasort);

	if (n < 0) {    // Check if scandir was successful, if -1 then error occurred
		fprintf(stderr, "Failed to scan input directory: %s\n", input_dir);
		goto cleanup;
	}

	// Process the .conf files with at most max_threads workers. Returns only
	// after EVERY worker has been joined, so from here on there is a single
	// thread again (has_running and datacenter_destroy below rely on that).
	{
		WorkQueue queue = {
			.dc = &dc,
			.input_dir = input_dir,
			.files = files,
			.n = n,
			.next = 0
		};
 
		if (pthread_mutex_init(&queue.lock, NULL) != 0) {
			fprintf(stderr, "Failed to initialize work queue.\n");
			goto cleanup;
		}
 
		run_workers(&queue, max_threads);
		pthread_mutex_destroy(&queue.lock);
	}
 
	// Wait for every VM to finish before destroying the Data Center
	while (has_running(&dc)) {
		check_all_finished_vms(&dc);
		datacenter_wait(50);
	}
 
	exit_code = 0;


cleanup:
 	if (n >= 0) {
		for (int i = 0; i < n; i++) {
			free(files[i]);
		}
		free(files);
	}
 
	datacenter_destroy(&dc);
	return exit_code;
}

