#include "main_auxiliar.h"
#include "parser.h"
#include "constants.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int has_running(DataCenter *dc) {

   for (size_t i = 0; i < dc->num_reservations; i++)
       if (dc->reservations[i].state == RES_STATE_RUNNING)
           return 1;

   return 0;
}

int process_file(const char *filepath, DataCenter *dc) {
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

/**
 * Hands out the next file index. Each index is returned exactly once, to
 * exactly one worker, which is what guarantees "every file processed once"
 *
 * @return Index of the file to process, or -1 when there is no more work
 */
static int next_file(WorkQueue *queue) {
	int i = -1;

	pthread_mutex_lock(&queue->lock);
	if (queue->next < queue->n) {
		i = queue->next++;
	}
	pthread_mutex_unlock(&queue->lock);

	return i;
}

/**
 * Worker body: keeps taking files from the queue until it is empty
 * A worker that finishes early simply takes the next file, so a slow
 * .conf (e.g. one with a long E command) never idles the other workers
 */
static void *worker_main(void *arg) {
	WorkQueue *queue = arg;
	int i;

	while ((i = next_file(queue)) >= 0) {
		char filepath[MAX_PATH_SIZE + 256];

		snprintf(filepath, sizeof(filepath), "%s/%s", queue->input_dir, queue->files[i]->d_name);

		// A file that fails to open is reported inside; the others still run
		process_file(filepath, queue->dc);
	}
	return NULL;
}

void run_workers(WorkQueue *queue, size_t max_threads) {

	size_t num_threads = max_threads < (size_t)queue->n ? max_threads : (size_t)queue->n;

	if (num_threads == 0)
		return; // no .conf files

	pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
	size_t created = 0;

	if (threads != NULL) {
		while (created < num_threads) {
			if (pthread_create(&threads[created], NULL, worker_main, queue) != 0) {
				fprintf(stderr, "Failed to create worker thread %zu.\n", created + 1);
				break;
			}
			created++;
		}
	} else {
		fprintf(stderr, "Failed to allocate worker threads.\n");
	}

	// Could not start a single worker: do the work in the calling thread
	if (created == 0)
		worker_main(queue);

	for (size_t i = 0; i < created; i++)
		pthread_join(threads[i], NULL);

	free(threads);
}
