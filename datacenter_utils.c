#include "datacenter_utils.h"
#include "datacenter.h"
#include "filesystem.h"

#include <errno.h>   // para ECHILD
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

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

// Deconstructiong reservation_destroy to create re-usable parts
static void vm_destroy(VM *vm) {

	// Free every VM belonging to the reservation.
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
			server->hosted_vms[server->num_hosted_vms] = NULL;
			break;
		}
	}
	free(vm);
}

static void reservation_remove_vm(Reservation *res, size_t k) {

	vm_destroy(res->vms[k]);
	memmove(&res->vms[k], &res->vms[k + 1],
	        (res->num_vms - k - 1) * sizeof(VM *));

	res->num_vms--;
	res->vms[res->num_vms] = NULL;
}

static void datacenter_remove_reservation(DataCenter *dc, size_t idx) {

    // Remove the reservation by shifting the remaining ones
	memmove(&dc->reservations[idx], &dc->reservations[idx + 1],
	        (dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}

void spawn_vm_child(DataCenter *dc, VM *vm) {

    /* uses the rlimit structure to define both limits for ram
       and disk, defining later on the current and max as the same limit
       so we can use the setrlimit() function */
	struct rlimit ram_lim;
	struct rlimit disk_lim;

	// starts by shitfing from GB into Bytes ^30
	ram_lim.rlim_cur = vm->type->required.ram << 30;
	ram_lim.rlim_max = ram_lim.rlim_cur;

	disk_lim.rlim_cur = vm->type->required.disk << 30;
	disk_lim.rlim_max = disk_lim.rlim_cur;

	setrlimit(RLIMIT_AS, &ram_lim);        // limit RAM using adress space
	setrlimit(RLIMIT_FSIZE, &disk_lim);    // maximum size in bytes any process can create


	int percent = vm->type->required.cpu / vm->server->total.cpu *
	                 dc->num_servers * 100;

	// transforms the percent int into a string
	char str_percent[MAX_STRING_SIZE];
	sprintf(str_percent, "%d", percent);

	/* execlp will search for the $PATH first and execute the command,
	   it also restrains the usage of NULL in the end */
	if (execlp("cpulimit", "cpulimit", "-q", "-f", "-l", str_percent, "--",
	            vm->type->exec_path, NULL) == -1)
	    exit(1);
}

int spawn_all_vms(DataCenter *dc, Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {

		VM *vm = res->vms[i];
		char dst_buffer[MAX_PATH_SIZE];

		// creates the path to be copied to as /tmp/CloudIST/<res id>/<vm id>
		snprintf(dst_buffer, sizeof(dst_buffer), "/tmp/CloudIST/%s/%s", res->id, vm->id);

		// calls our function to copy all the directories from the input_folder
		if (copy_dir_recursive(vm->type->input_folder, dst_buffer) == 1) {

		    fprintf(stderr, "%s went wrong while copying the input folder: %s\n",
					vm->id, vm->type->input_folder);
			return 1;
		}

		/* having a switch for the fork, so the parent can create
		   multiple VMs that will run as childs in background of the project */
		pid_t pid = fork();
		switch (pid) {

            case -1:    // error
                perror("fork");
                return 1;

            case 0:     // child
                spawn_vm_child(dc, vm);
                break;

            default:    //parent
                // save the child's PID in the VM, set its state to RUNNING
                vm->pid = pid;
                vm->state = VM_STATE_RUNNING;
                break;
        }
	}
	return 0;
}

/* TODO: IMPLEMENT WAITING FOR VM
   R: yes king */
void check_all_finished_vms(DataCenter *dc) {

    // variables to be incremented for each list respectivally
	size_t n_reservation = 0;
	size_t n_virtualmachine = 0;

	while (n_reservation < dc->num_reservations) {
	    // gets each reservation per cycle
		Reservation *res = &dc->reservations[n_reservation];

		if (res->state != RES_STATE_RUNNING) {
			n_reservation++;
			continue;
		}

		while (n_virtualmachine < res->num_vms) {
			VM *vm = res->vms[n_virtualmachine];

			if (vm->state == VM_STATE_RUNNING) {
				int status;
				pid_t r = waitpid(vm->pid, &status, WNOHANG);

				// r == pid: ended r == -1/ECHILD: has already been recieved
				if (r == vm->pid || (r == -1 && errno == ECHILD)) {

					reservation_remove_vm(res, n_virtualmachine);
					continue;  // Dont increase number of the vm
				}
				// r == 0: still runs
			}
			n_virtualmachine++;
		}

		if (res->num_vms == 0) {
			res->state = RES_STATE_FINISHED;
			datacenter_remove_reservation(dc, n_reservation);
			continue;   // Dont increase number of reservations
		}
		n_reservation++;
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
