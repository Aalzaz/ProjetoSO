#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#include "customstructs.h"
#include "parser.h"
#include "datacenter.h"
#include "constants.h"


void *process_commands(void *args) {
		ThreadArgs *thread_args = (ThreadArgs *)args;
		char *filepath = thread_args->filepath;
		DataCenter *dc = thread_args->dc;
		sem_t *thread_sem = thread_args->thread_sem;

		int file_fd = open(filepath, O_RDONLY);
		if (file_fd < 0) {
			fprintf(stderr, "Failed to open configuration file: %s\n", filepath);
			return (void*)1;
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
					//Has a critical section
					if(datacenter_define_VM(dc, &vmtype) != 0){
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
					//Has a critical section
					if (datacenter_reserve(dc, &reservation) != 0) {
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
					//Has a critical section
					if (datacenter_execute(dc, id) != 0) {
						fprintf(stderr, "Failed to execute reservation.\n");
						continue;
					}

					printf("Finished reservation execution!\n");

					break;

				case CMD_LIST:
					//Has a critical cesction
					if (datacenter_list(dc) != 0) {
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
						fprintf(stderr, "Failed to close configuration file: %s\n",filepath);
						free(thread_args);
						sem_post(thread_sem); // Increment semaphore on failure
						return (void*)1;
					}
					break;
				}
		}
	sem_post(thread_sem); // Increment semaphore after thread completion
	free(thread_args);
	return (void*)0;
}


int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 8) {
    fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <real_cores> <input_dir> <max_threads>\n", argv[0]);
    return 1;
	}

	size_t servers;
	size_t ram;
	size_t disk;
	size_t real_cores;
	size_t max_threads;
	double cpu;
	char *input_dir = argv[6];
	DIR *dir = opendir(input_dir);

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[5], &real_cores) != 0 ||
			parse_size_t_arg(argv[7], &max_threads) != 0 ||
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
		
	struct dirent *entry;
	while((entry = readdir(dir))){
		if (entry == NULL) {
			break;
		}
		size_t len = strlen(entry->d_name);
		if (len > 5 && strcmp(entry->d_name + len - 5, ".conf") == 0) {
			if (conf_list_add(conf_files, input_dir, entry->d_name) != 0) {
				fprintf(stderr, "Failed to add configuration file: %s\n", entry->d_name);
				return 1;
			}
		}
	}
	closedir(dir);

	//Alphabetically sorted, but with multi threading, the order of execution is not guaranteed.
	qsort(conf_files->files, conf_files->count, sizeof(ConfFile), conf_file_compare);

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu,
	.real_core = real_cores
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}
	
	sem_t thread_sem;
	if (sem_init(&thread_sem, 0, (unsigned int)max_threads) != 0) {
		fprintf(stderr, "Failed to initialize semaphore for thread limit.\n");
		return 1;
	}

	size_t count = 0;
	pthread_t thread[max_threads];

	while(count < conf_files->count){
		// Decrement semaphore to limit concurrent threads
		sem_wait(&thread_sem); 
		
		ThreadArgs *args = malloc(sizeof(ThreadArgs));
		if(args == NULL) {
			fprintf(stderr, "Failed to allocate memory for thread arguments.\n");
			return 1;
		}
		args->filepath = conf_files->files[count].filepath;
    	args->dc = &dc;
		args->thread_sem = &thread_sem;

		//Avoid the loss of a thread.
		if (count >= max_threads) 
        	pthread_join(thread[count % max_threads], NULL);

		if(pthread_create(&thread[count % max_threads], NULL, process_commands, (void*)args) != 0) {
			fprintf(stderr, "Failed to create thread for configuration file: %s\n", conf_files->files[count].filepath);
			free(args);
			sem_post(&thread_sem); // Increment semaphore on failure
			return 1;
		}
		count++;
	}
	size_t active_threads = (count < max_threads) ? count : max_threads;
	for (size_t i = 0; i < active_threads; i++) {
		pthread_join(thread[i], NULL);
	}
	datacenter_destroy(&dc);
	conf_list_destroy(conf_files);
	return 0;
}