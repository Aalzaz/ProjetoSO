#include "datacenter_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <constants.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}
int cpu_limit(double vmCPU, DataCenter *dc) {
    double limit = (100.0 * vmCPU * dc->real_cores) / (dc->servers[0].total.cpu * dc->num_servers);
    return (int)limit;
}

void spawn_vm_child(VM *vm, DataCenter *dc) {
	// TODO: Limit RAM, DISK and use exec with cpulimit.
	struct rlimit rlRam; 
	rlRam.rlim_cur = vm->type->required.ram * GIB_TO_BYTE;
	rlRam.rlim_max = vm->type->required.ram * GIB_TO_BYTE;
	setrlimit(RLIMIT_AS, &rlRam);

	struct rlimit rlDisk;
	rlDisk.rlim_cur = vm->type->required.disk * GIB_TO_BYTE;
	rlDisk.rlim_max = vm->type->required.disk * GIB_TO_BYTE;
	setrlimit(RLIMIT_FSIZE, &rlDisk);
	
	int limit_val = cpu_limit(vm->type->required.cpu,dc);
	char limit_str[16];
    snprintf(limit_str, sizeof(limit_str), "%d", limit_val);
	char *args[] = {
        "cpulimit",           
        "-q",              
        "-f",                
        "-l",                 
        limit_str,            
        "--",                
        vm->type->exec_path,  
        NULL                  
    };
	execvp("cpulimit", args);
	printf("Failed to execute cpulimit for VM %s.\n", vm->id);
    exit(EXIT_FAILURE);
}

void clone_directory(char *srcPath, char *destPath) {
	DIR *dir = opendir(srcPath);
	if (!dir) {
		printf("Failed to open source directory: %s\n", srcPath);
		return;
	}
	// Create destination directory with full permissions
	if(mkdir(destPath, 0777) != 0) {
		printf("Failed to create destination directory: %s\n", destPath);
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
    }

		char srcFilePath[MAX_PATH_SIZE];
		char destFilePath[MAX_PATH_SIZE];
		
		//Pull the full path of the source and destination files/directories
		snprintf(srcFilePath, sizeof(srcFilePath), "%s/%s", srcPath, entry->d_name);
		snprintf(destFilePath, sizeof(destFilePath), "%s/%s", destPath, entry->d_name);

		if (entry->d_type == DT_DIR) {
			clone_directory(srcFilePath, destFilePath);
		} else {
			int srcFile = open(srcFilePath, O_RDONLY);
			// Create destination file with read/write permissions for owner, and read for group and others
			int destFile = open(destFilePath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		

			if (srcFile < 0 || destFile < 0) {
				printf("Failed to open files for cloning: %s -> %s\n", srcFilePath, destFilePath);
				if (srcFile >= 0) close(srcFile);
				if (destFile >= 0) close(destFile);
				continue;
			}

			char buffer[BUFFER_SIZE];
			size_t bytesRead;
			while ((bytesRead = read(srcFile, buffer, sizeof(buffer))) > 0) {
				write(destFile, buffer, bytesRead);
			}

			close(srcFile);
			close(destFile);
		}
	}

	closedir(dir);
	}
	

int spawn_all_vms(Reservation *res, DataCenter *dc) {
	//clone -> fork  - > chdir -> execl
	// TODO: Implement fork code. Set VM PID and update VM state to running.
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];
		
		//Clone directory to a temporary location for the VM to run in isolation.
		mkdir(TEMP_DIR, 0777);

		char res_dir[MAX_PATH_SIZE];
        snprintf(res_dir, sizeof(res_dir), "%s/%s", TEMP_DIR, res->id);
        mkdir(res_dir, 0777);
		char temp_dir[MAX_PATH_SIZE];
        snprintf(temp_dir, sizeof(temp_dir), "%s/%s/%s", TEMP_DIR, res->id, vm->id);
		clone_directory(vm->type->input_folder, temp_dir);

		pid_t pid = fork();
		if (pid < 0) {
			fprintf(stderr, "Failed to fork for VM %s.\n", vm->id);
			return 1;
		} else if (pid == 0) {
			// Child process
			chdir(temp_dir);
			spawn_vm_child(vm, dc);	
		}else{
			vm->pid = pid;
			vm->state = VM_STATE_RUNNING;
		}
	}

	return 0;
}

void wait_for_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		int status;
		waitpid(res->vms[i]->pid, &status, 0);
		if(WIFEXITED(status) && WEXITSTATUS(status) != 0){
			fprintf(stderr, "VM %s exited with an error.\n", res->vms[i]->id);
		}

		res->vms[i]->state = VM_STATE_TERMINATED;
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}