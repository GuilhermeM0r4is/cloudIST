#define _XOPEN_SOURCE 700

#include "filesystem.h"

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

/**
 * Copies one file's bytes into a new file
 *
 * @param src Source File to copy bytes from
 * @param dst Destination to where we should copy the bytes
 *
 * @return 0 if the copying process worked
 * @return 1 if there were any errors whatsoever
 */
static int copy_file(const char *src, const char *dst) {

    // opens the source file as read only, if the file doesn't
    // exist, returns -1, so we shall consider it as an error
    if (open(src, O_RDONLY) == -1)
        return 1;
}

int copy_dir_recursive(const char *src, const char *dst) {

}

/**
 * Compares two paths of two given constants
 *
 * @param a char pointer given to compare
 * @param b char pointer given to compare
 *
 * @return 0 if chars are equal to each other
 * @return positive value if a should sort after b
 * @return negative value if a should sort before b
 */
static int compare_paths(const void *a, const void *b) {
    // all the function has to do, is to compare both chars and return
    // the respective value so qsort can work
    return strcmp((const char *)a, (const char *)b);
}

int list_conf_files(const char *path, char buffer[][MAX_PATH_SIZE]) {

    int counter = 0;    // counter that will assure ammount of files inside buffer
    errno = 0;          // clears errno from other possible problems in code

    // given type DIR into the pointer, opendir() shall open a
    // directory stream corresponding to the directory named by argument
    DIR *dir = opendir(path);

    // finds one entry inside a directory
    struct dirent *dir_entry;

    // returns NULL only if it couldn't open the directory
    if (dir == NULL) {
        perror("dir");
        return -1;
    }

    // will open each entry and while still having entries
    while((dir_entry = readdir(dir)) != NULL) {

        // a direct pointer into the filename inside the dir entry
        char *filename = dir_entry -> d_name;

        // finds the location of the last point in the filename
        char *dot_location = strrchr(filename, '.');

        // if the dot_location returns null, it means the given entry
        // has no file inside with .conf in it, so we skip it
        if (dot_location == NULL)
            continue;

        // checks the string comparison, if it returns 0, we
        // have that the pointer is indeed a .conf file
        if (strcmp(dot_location, ".conf") == 0) {

            if (counter >= MAX_FILES)    // if the counter is > 32, our limit
                break;                   // we shall stop the while loop

            // this formats a string (like printf, but writes into a buffer
            // instead of the screen) and is bounds-safe (won't overflow destination
            // past size bytes), initial path and addidng the filename
            snprintf(buffer[counter], MAX_PATH_SIZE, "%s/%s", path, filename);

            counter++;
        }
    }

    // if errno changed, then it means something failed during the readdir
    if (errno != 0) {
        perror("readdir");
        closedir(dir);
        return -1;
    }

    closedir(dir);

    // base > buffer, elem_in > counter, size > one buffer index, compar > compare_paths
    // this way, qsort can quickly sort our array so we can use it later
    qsort(buffer, counter, sizeof(buffer[0]), compare_paths);

    // returns the ammount of files inside the buffer
    return counter;
}
