#ifndef FAT32_H
#define FAT32_H

#include <barrelfish/aos_rpc.h>

/**
 * Typedef for the function used to read a sector.
 */
typedef errval_t (*sector_read_function_t) (size_t sector_index, void* buffer);

struct fat32_config {
    // A sector read function.
    sector_read_function_t read_function;

    // The sector number for the volume ID block.
    uint32_t volume_id_sector;

    // The number of sectors per cluster.
    uint32_t sectors_per_cluster;

    // The first sector of the FAT table.
    uint32_t fat_sector_begin;

    // The first sector of the cluster area.
    uint32_t cluster_sector_begin;

    // The cluster index of the root directory.
    uint32_t root_directory_cluster;
};

/**
 * Initialize the FAT file system.
 * This function reads the volume ID block and stores all relevant
 * information in the config.
 *
 * \param config: The config struct to be filled.
 * \param read_function: The function to read a block.
 * \param volume_id_sector: The sector number of the volume ID.
 */
errval_t fat32_init (struct fat32_config* config, sector_read_function_t read_function, uint32_t volume_id_sector);

/**
 * Open the specified file.
 *
 * \param config: FAT filesystem data.
 * \param path: The path to the file.
 * \param file_descriptor: Storage for the file descriptor.
 *
 * NOTE: The path format is UNIX style, i.e. /a/b/c.txt
 */
errval_t fat32_open_file (struct fat32_config* config, char* path, uint32_t* file_descriptor);

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
 * \param config: FAT filesystem data.
 * \param path: The path to the directory.
 * \param entry_list: Result parameter for the array containing entry descriptors.
 * \param entry_count: Result parameter for the directory entry count.
 *
 * NOTE: The path format is UNIX style, i.e. /a/b/c or /a/b/c/
 *
 * NOTE: In case of success "entry_list" is allocated by this function.
 * It is the callers responsibility to free it afterwards.
 */
errval_t fat32_read_directory (struct fat32_config* config, char* path, struct aos_dirent** entry_list, size_t* entry_count);

#endif
