#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <string.h>


#include "parser.h"
#include "datacenter.h"
#include "constants.h"


int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 6) {
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir>\n", argv[0]);
    return 1;
  }

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	char input_dir[MAX_PATH_SIZE]; // Buffer for input directory path

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	if (strlen(argv[5]) >= sizeof(input_dir)) {
    fprintf(stderr, "Input directory path too long.\n");
    return 1;
	}
	strcpy(input_dir, argv[5]);

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	struct dirent **files;

	if (!path_exists(input_dir)) {
		fprintf(stderr, "Input directory does not exist: %s\n", input_dir);
		return 1;
	};

	int n = scandir(input_dir, &files, is_conf_file, alphasort);
	// Check if scandir was successful, if -1 then error occurred
	if (n < 0) {
		fprintf(stderr, "Failed to scan input directory: %s\n", input_dir);
		return 1;
	}

	for (int i = 0; i < n; i++) {
		char filepath[MAX_PATH_SIZE + 256];

		snprintf(filepath, sizeof(filepath), "%s/%s", input_dir, files[i]->d_name);
		int fd = open(filepath, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Failed to open file: %s\n", filepath);
			continue;
		}
		int done = 0;
		while (!done) {
			Command cmd = get_next_command(fd);

			if (cmd == EOC) {
				done = 1;
				continue;
			}
			switch (cmd) {
				case CMD_DEFINE: {
					VMType vmtype;

					if (parse_define(fd, &vmtype) != 0) {
						fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
						continue;
					}

					if(datacenter_define_VM(&dc, &vmtype) != 0){
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

					if (datacenter_reserve(&dc, &reservation) != 0) {
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

					if (datacenter_execute(&dc, id) != 0) {
						fprintf(stderr, "Failed to execute reservation.\n");
						continue;
					}

					printf("Finished reservation execution!\n");

					break;
				}

				case CMD_LIST: {
					if (datacenter_list(&dc) != 0) {
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

				case EOC: {
					done = 1;
					break;
				}
				
			}
		}
		close(fd);
	}

for (int i = 0; i < n; i++) {
    free(files[i]);
}
	free(files);

datacenter_destroy(&dc);
return 0;
}
