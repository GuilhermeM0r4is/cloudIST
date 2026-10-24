#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>

#include "datacenter.h"
#include "datacenter_utils.h"
#include "constants.h"
#include "main_auxiliar.h"
#include "parser.h"

int main(int argc, char **argv){

	if (argc != 8) {
		fprintf(stderr,
		        "Usage: %s <servers> <ram> <disk> <cpus> <local_cores> <input_dir> <max_threads>\n",
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
	size_t core_count;
	size_t max_threads;
	const char *input_dir = argv[6];
	struct dirent **files = NULL;
	int n = -1;                 // -1 means scandir did not succeed
	int exit_code = 1;

	datacenter_init(&dc);

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &core_count) != 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		goto cleanup;
	}

	if (parse_size_t_arg(argv[7], &max_threads) != 0 || max_threads == 0) {
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

	if(datacenter_configure(&dc, servers, &resources, core_count) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		goto cleanup;
	}

	if (!path_exists(input_dir)) {
		fprintf(stderr, "Input directory does not exist: %s\n", input_dir);
		goto cleanup;
	}

	if (mkdir(CLOUDIST_ROOT_DIR, DIRECTORY_PERMISSION_MODE) == -1 &&
        errno != EEXIST) {

        perror("Failed to create CloudIST root directory");
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
