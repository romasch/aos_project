#ifndef FAT32_H
#define FAT32_H

#include <barrelfish/aos_rpc.h>

/**
 * Typedef for the function used to read a sector.
 */
typedef errval_t (*sector_read_function_t) (size_t sector_index, void* buffer);

/**
 * Initialize the FAT file system.
 * This reads the first block and extracts all information
 * needed for further processing.
 *
 * \param read_function: The function to read a block.
 */
errval_t fat32_init (sector_read_function_t read_function, uint32_t fat32_pbb);

/**
 * Open the specified file.
 *
 * \param path: The path to the file.
 * \param file_descriptor: Storage for the file descriptor.
 *
 * NOTE: The path format is UNIX style, i.e. /a/b/c.txt
 */
errval_t fat32_open_file (char* path, uint32_t* file_descriptor);

/**
 * Close the specified file.
 * NOTE: File must be open.
 */
errval_t fat32_close_file (uint32_t file_descriptor);

/**
 * Read a chuck of the file.
 *
 * \param file_descriptor: The file identified by its descriptor.
 * \param position: Position where to read from.
 * \param size: Maximum number of bytes to read.
 * \param buf: Result parameter for the buffer.
 * \param buflen: Result parameter for the buffer length.
 *
 * NOTE: In case of success, "buf" is allocated by this function.
 * It is the callers responsibility to free it afterwards.
 */
errval_t fat32_read_file (uint32_t file_descriptor, size_t position, size_t size, void** buf, size_t *buflen);

/**
 * Read the contents of a directory.
 *
 * \param path: The path to the directory.
 * \param entry_list: Result parameter for the array containing entry descriptors.
 * \param entry_count: Result parameter for the directory entry count.
 *
 * NOTE: The path format is UNIX style, i.e. /a/b/c or /a/b/c/
 *
 * NOTE: In case of success "entry_list" is allocated by this function.
 * It is the callers responsibility to free it afterwards.
 */
errval_t fat32_read_directory (char* path, struct aos_dirent** entry_list, size_t* entry_count);

/// Some test features.
void test_fs (void);

inline uint32_t get_int (void* buffer, uint32_t offset);

inline uint32_t get_int (void* buffer, uint32_t offset)
{
    return (uint32_t)(((char*)buffer) + offset);
}

#endif
