#include <stddef.h>
#include <dirent.h>
#include <pthread.h>

#include "datacenter_utils.h"

/**
 * Work shared by all workers: the sorted list of .conf files and the index
 * of the next file to hand out
 *
 * Only `next` is ever modified while workers run, and only under `lock`
 * Everything else is read-only after setup, so it needs no synchronization
 * The Data Center is NOT protected here: it is accessed exclusively through
 * the datacenter_*() functions, which own their synchronization
 */
typedef struct WorkQueue {
	DataCenter *dc;
	const char *input_dir;
	struct dirent **files;
	int n;                    // number of files
	int next;                 // next index to hand out (protected by lock)
	pthread_mutex_t lock;
} WorkQueue;

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
int process_file(const char *filepath, DataCenter *dc);

/**
 * Necessary function to prevent orphaned VMs from continuing to run.
 *
 * @param dc Pointer to the datacenter we're looking for.
 *
 * @return 0 if there's a running reservation, 1 otherwise.
 */
int has_running(DataCenter *dc);

/**
 * Runs the queue with at most max_threads workers and returns only when
 * every worker has finished (all files processed)
 *
 * Never creates more workers than there are files. If a thread cannot be
 * created the remaining workers (or, as a last resort, the calling thread)
 * simply drain the queue, so no file is ever skipped
 */
void run_workers(WorkQueue *queue, size_t max_threads);
