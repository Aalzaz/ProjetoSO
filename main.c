#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include "customstructs.h"
#include "parser.h"
#include "datacenter.h"
#include "constants.h"

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 7) {
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir> <real_cores>\n", argv[0]);
    return 1;
	}

	size_t servers;
	size_t ram;
	size_t disk;
	size_t real_cores;
	double cpu;
	char *input_dir = argv[5];
	DIR *dir = opendir(input_dir);

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[6], &real_cores) != 0 ||
			dir == NULL){
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}
	//Dynamic array MAX_STRING_SIZE used for the initial capacity of the configuration file list.
	ConfFileList *conf_files = conf_list_create(MAX_STRING_SIZE);
	if (!conf_files) {
		fprintf(stderr, "Failed to create configuration file list.\n");
		return 1;
	}

	while(1){
		struct dirent *entry = readdir(dir);
		if (entry == NULL) {
			break;
		}
		size_t len = strlen(entry->d_name);
		if (len > 4 && strcmp(entry->d_name + len - 4, ".conf") == 0) {
			if (conf_list_add(conf_files, input_dir, entry->d_name) != 0) {
				fprintf(stderr, "Failed to add configuration file: %s\n", entry->d_name);
				return 1;
			}
		}
	}
	qsort(conf_files->files, conf_files->count, sizeof(ConfFile), conf_file_compare);

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	size_t count = 0;
	while(count < conf_files->count){
		int file_fd = open(conf_files->files[count].filepath, O_RDONLY);
		if (file_fd < 0) {
			fprintf(stderr, "Failed to open configuration file: %s\n", conf_files->files[count].filepath);
			return 1;
		}
		int cycle = 1;
		while (cycle){
			switch (get_next_command(file_fd)){
				case CMD_DEFINE: {
					VMType vmtype;

					if (parse_define(file_fd, &vmtype) != 0) {
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

					size_t num_items = parse_reserve(file_fd, &reservation, MAX_RESERVATIONS_ITEMS);

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

				case CMD_EXECUTE:
					char id[MAX_STRING_SIZE];

					if(parse_execute(file_fd, id) != 0){
						fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
						continue;
					}

					if (datacenter_execute(&dc, id) != 0) {
						fprintf(stderr, "Failed to execute reservation.\n");
						continue;
					}

					printf("Finished reservation execution!\n");

					break;

				case CMD_LIST:
					if (datacenter_list(&dc) != 0) {
						fprintf(stderr, "Failed to list VMs.\n");
						continue;
					}

					break;

				case CMD_WAIT:
					unsigned int delay;

					if(parse_wait(file_fd, &delay) != 0){
						fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
						continue;
					}

					datacenter_wait(delay);
					break;

				case CMD_INVALID:
					fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
					break;

				case CMD_HELP:
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

				case CMD_EMPTY:
					break;

				case EOC:
					cycle = 0;
					if (close(file_fd) < 0) {
						fprintf(stderr, "Failed to close configuration file: %s\n", conf_files->files[count].filepath);
						return 1;
					}
					count++;
					break;
				}
		}
	}
	datacenter_destroy(&dc);
	conf_list_destroy(conf_files);
	closedir(dir);
	return 0;
}