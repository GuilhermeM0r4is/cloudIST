#ifndef FILESYSTEM__H
#define FILESYSTEM__H

#include <stddef.h>
#include "constants.h"

/**
 * Checks whether a path exists and is a directory.
 *
 * @param path Directory path.
 *
 * @return 1 if it exists and is a directory, 0 otherwise.
 */
int path_exists(const char *path);

/**
 * Checks whether a path exists and is a regular file.
 *
 * @param path File path.
 *
 * @return 1 if it exists and is a regular file, 0 otherwise.
 */
int file_exists(const char *path);

/**
 * Converts the given path into an absolute path, resolving symbolic links,
 * relative components ('.' and '..'), and redundant separators. The resolved
 * path is copied into the provided buffer.
 *
 * @param path Path to resolve.
 * @param buffer Destination buffer where the absolute path will be stored.
 * @param size Size of the destination buffer, in bytes.
 *
 * @return 0 if the path was successfully resolved and copied to the buffer
 * @return 1 if the path could not be resolved or the buffer is too small.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/**
 * Walks a directory tree, recreating its folder structure at the destination
 * while copying any file recursively and preserving subfolders.
 *
 * @param src Path to the source file.
 * @param dst Path to the destination file to copy all info.
 *
 * @return 0 if the contents of source file were copied into dst.
 * @return 1 in case of errors with the copy logic
 */
int copy_dir_recursive(const char *src, const char *dst);

/**
 * Takes a directory path and checks for .conf files inside of it.
 *
 * @param path Directory to search for files.
 * @param buffer Array with room for up to MAX_FILES paths where all
 * .conf files will be stored.
 *
 * @return -1 if the directory could not be opened.
 * @return the number of .conf files found (0 or more) otherwise.
 */
int list_conf_files(const char *path, char buffer[][MAX_PATH_SIZE]);

#endif // FILESYSTEM__H
