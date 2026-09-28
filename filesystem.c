#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "constants.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>     // included for handling directory operations
#include <errno.h>      // included to handle possible errors with code


int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

int is_conf_file(const struct dirent *entry) {

    // searches for the size of the entry filename
	size_t len = strlen(entry -> d_name);

	// returns the logical response to when a filename
	// ends with the .conf termination
	return (len > 5 && strcmp(entry -> d_name + len - 5, ".conf") == 0);
}

/**
 * Auxiliar function that helps not overwrite code
 *
 * @param fd1 File descriptor of one file
 * @param fd2 File descriptor of another file.
 *
 * @return 1 all cases.
 */
static int fail_copy(int fd1, int fd2) {
    close(fd1);
    close(fd2);
    return 1;
}

/**
 * Copies one file's bytes into a new file
 *
 * @param src Source File to copy bytes from
 * @param dst Destination to where we should copy the bytes
 *
 * @return 0 if the copying process worked, 1 otherwise.
 */
static int copy_file(const char *src, const char *dst) {

    // opens the source file as read only
    int src_fd = open(src, O_RDONLY);

    // if -1, didn't open, we should stop
    if (src_fd == -1)
        return 1;

    // opens dst as write only, creates if file doesn't exist
    // and empties the file content if it exists already
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, PERMISSION_MODE);

    if (dst_fd == -1) {
        close(src_fd);
        return 1;
    }

    char buffer[BUFFER_COPYING_SIZE];

    while(1) {
        // reads up the file content until the bytes given
        int bytes = read(src_fd, buffer, sizeof(buffer));

        if (bytes == 0) // there's nothing else to read
            break;

        if (bytes < 0) // something went wrong
            return fail_copy(src_fd, dst_fd);

        // if we have read bytes up until here
        if (write(dst_fd, buffer, bytes) == -1)
            return fail_copy(src_fd, dst_fd);
    }
    close(src_fd);
    close(dst_fd);
    return 0;
}

int copy_dir_recursive(const char *src, const char *dst) {

    // makes an new directory with read, write and search permissions
    // for owner and read and search permissions for others
    int new_dir = mkdir(dst, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    // assures there will be an error
    if (new_dir == -1 && errno != EEXIST)
        // not considered an error if the folder already exists
        return 1;

    errno = 0;

    DIR *dir = opendir(src);
    struct dirent *dir_entry;

    // returns NULL only if it couldn't open the directory
    if (dir == NULL) {
        perror("dir");
        return 1;
    }

    // will open each entry and while still having entries
    while((dir_entry = readdir(dir)) != NULL) {

        if (strcmp(dir_entry -> d_name, ".") == 0 || strcmp(dir_entry -> d_name, "..") == 0)
            continue;   // skips any logic for other directories

        // stores both paths into the buffers
        char src_buffer[MAX_PATH_SIZE];
        snprintf(src_buffer, sizeof(src_buffer), "%s/%s", src, dir_entry -> d_name);

        char dst_buffer[MAX_PATH_SIZE];
        snprintf(dst_buffer, sizeof(dst_buffer), "%s/%s", dst, dir_entry -> d_name);

        struct stat st;
        // assures that if -1, then stat failed
        if (stat(src_buffer, &st) == -1) {
            closedir(dir);
            return 1;
        }

        if (S_ISDIR(st.st_mode)) {
            // is a directory
            if (copy_dir_recursive(src_buffer, dst_buffer) == 1) {
                closedir(dir);
                return 1;
            }
        }

        else if (S_ISREG(st.st_mode)) {
            // is a regular file
            if(copy_file(src_buffer, dst_buffer) == 1) {
                closedir(dir);
                return 1;
            }
        }
    }

    // if errno changed, then it means something failed during the readdir
    if (errno != 0) {
        perror("readdir");
        closedir(dir);
        return 1;
    }

    closedir(dir);
    return 0;
}
