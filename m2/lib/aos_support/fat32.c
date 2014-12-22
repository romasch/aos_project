#include <aos_support/fat32.h>

#include <barrelfish/aos_rpc.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// #define VERBOSE
#include <barrelfish/aos_dbg.h>

#define DIRECTORY_ENTRIES 16


static inline uint8_t get_char (void* buffer, uint32_t offset)
{
    return ((uint8_t*) buffer) [offset];
}

static inline uint16_t get_short (void* buffer, uint32_t offset)
{
    // We can't handle offsets spanning two words yet.
    assert ((offset & 1) == 0);
    return ((uint16_t*) buffer) [offset >> 1];
}

static inline uint32_t get_int (void* buffer, uint32_t offset)
{
    // We can't handle offsets spanning two words yet.
    assert ((offset & 3) == 0);
    return ((uint32_t*) buffer) [offset >> 2];
}

struct sector_stream {
    struct fat32_config* config;
    uint32_t cluster_start;
    uint32_t current_cluster;
    uint32_t sector_index;
};

static void stream_init (struct sector_stream* stream, struct fat32_config* conf, uint32_t a_cluster_start)
{
    stream -> config = conf;
    stream -> cluster_start = a_cluster_start;
    stream -> current_cluster= a_cluster_start;
    stream -> sector_index = 0;
}


// Forward declaration.
static errval_t fat_lookup (struct fat32_config* config, uint32_t cluster_index, uint32_t* result_sector);

static bool stream_is_finished (struct sector_stream* stream)
{
    return (stream->current_cluster) >= 0x0FFFFFF8; //TODO: Define constant.
}

/// Advance the sector stream.
static errval_t stream_next (struct sector_stream* stream)
{
    errval_t error = SYS_ERR_OK;
    stream -> sector_index++;

    if (stream -> sector_index == stream -> config -> sectors_per_cluster) {
        stream -> sector_index = 0;
        uint32_t next_cluster = stream->current_cluster;
        error = fat_lookup (stream -> config, stream -> current_cluster, &next_cluster);
        stream -> current_cluster = next_cluster;
    }
    return SYS_ERR_OK; // TODO
}

/// Load the sector pointed to by 'stream' into 'buffer'.
static errval_t stream_load (struct sector_stream* stream, void* buffer)
{
    struct fat32_config* conf = stream -> config;

    // Calculate sector number from cluster index.
    uint32_t sector_to_read = conf -> cluster_sector_begin;
    sector_to_read  += ((stream -> current_cluster) - 2) * (conf -> sectors_per_cluster);

    // Add the sector index within the cluster.
    sector_to_read = sector_to_read + stream -> sector_index;

    // Perform the actual read.
    errval_t error = stream->config->read_function (sector_to_read, buffer);
    return error;
}

/// Do a FAT table lookup.
static errval_t fat_lookup (struct fat32_config* config, uint32_t cluster_index, uint32_t* result_sector)
{
    char sector [512];
    errval_t error = SYS_ERR_OK;
    uint32_t result = 0;
    // Read the correct sector in the FAT table (consider upper 25 bits).
    uint32_t fat_sector_offset = cluster_index >> 7;
    debug_printf_quiet ("FAT sector offset: %u\n", fat_sector_offset);
    error = config->read_function (config -> fat_sector_begin + fat_sector_offset, sector);

    if (err_is_ok (error)) {
        // Read the next cluster number in this FAT sector (lower 7 bits).
        debug_printf_quiet ("FAT index: %u\n", cluster_index & 127);
        result = ((uint32_t*) sector) [cluster_index & 127];

        debug_printf_quiet ("Raw result: %x\n", result);

        if (result < 0x0FFFFFF8) {
            // According to https://www.pjrc.com/tech/8051/ide/fat32.html the top bits should be cleared
            result = ((result << 4) >> 4);
        } else {
            // TODO In all other code we assumed (-1) is the end of mark,
            // but it's actually anything >=0x0FFFFFF8.
            // Maybe we should fix the other code.
            result = 0xFFFFFFFF;
        }

        if (result_sector) {
            *result_sector = result;
        }

    }
    debug_printf_quiet ("Result: %x\n", result);
    return error;
}


static errval_t fat32_find_node (struct fat32_config* config, char* path, uint32_t* ret_cluster, uint32_t* ret_size)
{
    debug_printf_quiet ("Find node %s\n", path);

    uint32_t path_index = 0;
    errval_t error = SYS_ERR_OK;
    bool end_of_path = false;
    bool found_entry = false;

    uint32_t cluster_index = config -> root_directory_cluster;
    uint32_t file_size = 0;

    //TODO: Hack for root directory.
    if (strcmp (path, "/") == 0) {
        *ret_cluster = cluster_index;
        *ret_size = 0;
        return SYS_ERR_OK;
    }


    while (!end_of_path && err_is_ok (error)) {
        uint32_t fat_name_index = 0;
        char fat_name [12];

        bool end_of_name = false;
        while (!end_of_name) {
            switch (path [path_index]) {
                case '\0':
                    end_of_path = true;
                    // Fallthrough intended!
                case '/':
                    while (fat_name_index < 11) {
                        fat_name [fat_name_index] = ' ';
                        fat_name_index++;
                    }
                    end_of_name = true;
                    break;
                case '.':
                    while (fat_name_index < 8) {
                        fat_name [fat_name_index] = ' ';
                        fat_name_index++;
                    }
                    break;
                default:
                    if (fat_name_index < 11) {
                        fat_name [fat_name_index] = (char) toupper ((int) path [path_index]);
                        fat_name_index++;
                    }
            }
            path_index++;
        }
        fat_name [11] = '\0';

        if (fat_name [0] != ' ') {

            debug_printf_quiet ("FAT name: ---%s---\n", fat_name);
            found_entry = false;

            struct sector_stream stack_stream;
            struct sector_stream* stream = &stack_stream;
            stream_init (stream, config, cluster_index);
            char sector [512];
            bool early_stop = false;

            while ( err_is_ok (error) && !stream_is_finished (stream) && !found_entry && !early_stop) {

                error = stream_load (stream, &sector);

                for (int entry_index = 0; err_is_ok (error) && entry_index < DIRECTORY_ENTRIES && !found_entry && !early_stop; entry_index++) {
                    uint32_t base_offset = entry_index * 32;

                    uint8_t first_byte = get_char (sector, base_offset);
                    uint8_t attributes = get_char (sector, base_offset + 0x0b);

                    // The cluster index is split among two 16 bit fields.
                    uint32_t cluster_high = get_short (sector, base_offset + 0x14); // Higher 2 bytes.
                    uint32_t cluster_low = get_short (sector, base_offset + 0x1a); // Lower 2 bytes.
                    uint32_t cluster_index_entry = (cluster_high << 16) | cluster_low;

                    // Check what kind of directory entry it is.
                    if (first_byte == 0xe5) {
                        debug_printf_quiet ("Entry: %u, Deleted.\n", entry_index);
                    }
                    else if (first_byte == 0x0) {
                        debug_printf_quiet ("Entry: %u, Null entry.\n", entry_index);
//                         early_stop = true;
                    }
                    // Long file name from VFAT extension.
                    else if ( (attributes & 0xF) == 0xF) {
                        debug_printf_quiet ("Entry: %u, Long filename. Attributes: %u\n", entry_index, attributes);
                    }
                    else if (attributes & 0x8) {
                        debug_printf_quiet ("Entry: %u, Volume ID. Attributes: %u\n", entry_index, attributes);
                    }
                    // This is a file or directory. Finally, something to do.
                    else if ((attributes & 0x10) || (attributes & 0x20) || (attributes == 0)) {

                        // FAT without extensions has 8.3 naming scheme.
                        // This means we only need to copy 11 characters.
                        if (strncmp (fat_name, sector+base_offset, 11) == 0) {
                            cluster_index = cluster_index_entry;
                            found_entry = true;
                        }
                        if ((attributes & 0x20) || (attributes == 0)) {
                            file_size = get_int (sector, base_offset + 0x1c);
                            debug_printf_quiet ("Entry: %u, File. Attrib: %u. Name: %s. Cluster: %u. Size: %u\n", entry_index, attributes, sector+base_offset, cluster_index_entry, file_size);
                        } else {
                            file_size = 0;
                            debug_printf_quiet ("Entry: %u, Directory. Attrib: %u. Name: %s. Cluster: %u.\n", entry_index, attributes, sector+base_offset, cluster_index_entry);
                        }
                    } else {
                        debug_printf_quiet ("Unknown entry. Attributes %u\n", attributes);
                    }
                }

                error = stream_next (stream);
            }
        }
    }

    if (!found_entry) {
        debug_printf_quiet ("File not found!\n");
        error = AOS_ERR_FAT_NOT_FOUND;
    }

    if (err_is_ok (error)) {
        *ret_cluster = cluster_index;
        *ret_size = file_size;
    } else {
        *ret_cluster = 0;
        *ret_size = 0;
    }
    return error;
}

struct fat32_config* descriptor_to_config [1024];
static uint32_t descriptor_to_size [1024];
static uint32_t descriptor_to_cluster [1024];
static uint32_t descriptor_count = 0;

errval_t fat32_open_file (struct fat32_config* config, char* path, uint32_t* file_descriptor)
{
    assert (path != NULL && file_descriptor != NULL);
    assert (descriptor_count < 1024); // TODO: A dynamic structure.

    uint32_t ret_cluster = 0;
    uint32_t ret_size = 0;
    errval_t error = SYS_ERR_OK;

    error = fat32_find_node (config, path, &ret_cluster, &ret_size);

    if (err_is_ok (error)) {
        descriptor_to_config [descriptor_count] = config;
        descriptor_to_cluster [descriptor_count] = ret_cluster;
        descriptor_to_size [descriptor_count] = ret_size;
        *file_descriptor = descriptor_count;
        descriptor_count++;
    }
    return error;
}

errval_t fat32_close_file (uint32_t file_descriptor)
{
    // TODO: Recycle file descriptors.
    descriptor_to_cluster [file_descriptor] = 0;
    descriptor_to_size [file_descriptor] = 0;
    return SYS_ERR_OK;
}

static inline int min (int first, int second)
{
    if (first < second) {
        return first;
    } else {
        return second;
    }
}

errval_t fat32_read_file (uint32_t file_descriptor, size_t position, size_t size, void** buf, size_t *buflen)
{
    uint32_t cluster_index = descriptor_to_cluster [file_descriptor];
    uint32_t file_size = descriptor_to_size [file_descriptor];
    debug_printf_quiet ("FD: %u, size: %u, Cluster index: %u, file size: %u, position %u\n", file_descriptor, size, cluster_index, file_size, position);

    errval_t error = SYS_ERR_OK;
    struct sector_stream stack_stream;
    struct sector_stream* stream = &stack_stream;
    stream_init (stream, descriptor_to_config [file_descriptor], cluster_index);
    uint32_t early_stop = false;
    char sector [512];

    uint32_t file_index = 0;
    uint32_t chars_read = 0;
    uint32_t buffer_size = min (file_size - position, size);
    int8_t* buffer = malloc (buffer_size+1);
    if (!buffer) {
        error = LIB_ERR_MALLOC_FAIL;
    }

    // Skip sectors which are not interesting.
    while (!stream_is_finished (stream) && file_index + 512 < position && err_is_ok (error)) {
        error = stream_next (stream);
        file_index += 512;
    }

    while (!stream_is_finished (stream) && chars_read != size && err_is_ok (error)) {
        error = stream_load (stream, sector);

        // TODO: Use memcpy
        for (int i=0; i<512; i++) {
            if ( (file_index + i) >= (position + size) || (position + i) > file_size) {
                early_stop = true;
            } else if (position <= (file_index + i) && (file_index + i) < (position + size)) {
                buffer [chars_read] = sector [i];
                chars_read++;
            }
        }
        file_index += 512;
        error = stream_next (stream);
    }
     if (err_is_ok (error)) {
        *buf = buffer;
        *buflen = buffer_size;
    }
    return error;
}


errval_t fat32_read_directory (struct fat32_config* config, char* path, struct aos_dirent** entry_list, size_t* entry_count)
{
    uint32_t cluster_index = 0;
    uint32_t ret_size;
    struct aos_dirent new_entry;

    errval_t error = fat32_find_node (config, path, &cluster_index, &ret_size);

    // Check that it's a directory.
    if (ret_size != 0) {
        return AOS_ERR_FAT_NOT_FOUND;
    }

    uint32_t capacity = 1;
    uint32_t count = 0;
    struct aos_dirent* result;
    result = malloc (capacity * sizeof (struct aos_dirent));

    struct sector_stream stack_stream;
    struct sector_stream* stream = &stack_stream;
    stream_init (stream, config, cluster_index);
    uint32_t early_stop = false;
    char sector [512];

    while ( !stream_is_finished (stream) && err_is_ok (error) && !early_stop) {

        error = stream_load (stream, &sector);

        for (int entry_index = 0; entry_index < DIRECTORY_ENTRIES && err_is_ok (error) && !early_stop; entry_index++) {

            uint32_t base_offset = entry_index * 32;

            uint8_t first_byte = get_char (sector, base_offset);
            uint8_t attributes = get_char (sector, base_offset + 0x0b);

            // The cluster index is split among two 16 bit fields.
            uint32_t cluster_high = get_short (sector, base_offset + 0x14); // Higher 2 bytes.
            uint32_t cluster_low = get_short (sector, base_offset + 0x1a); // Lower 2 bytes.
            uint32_t cluster_index_entry = (cluster_high << 16) | cluster_low;
            cluster_index_entry = cluster_index_entry; // Avoid compiler warning.

            // Check what kind of directory entry it is.
            if (first_byte == 0xe5) {
                debug_printf_quiet ("Entry: %u, Deleted.\n", entry_index);
            }
            else if (first_byte == 0x0) {
                debug_printf_quiet ("Entry: %u, Null entry.\n", entry_index);
                // TODO: We can skip the remaining entries.
                early_stop = true;
            }
            // Long file name from VFAT extension.
            else if ( (attributes & 0xF) == 0xF) {
                debug_printf_quiet ("Entry: %u, Long filename. Attributes: %u\n", entry_index, attributes);
            }
            else if (attributes & 0x8) {
                debug_printf_quiet ("Entry: %u, Volume ID. Attributes: %u\n", entry_index, attributes);
            }
            // This is a file or directory. Finally, something to do.
            else if ((attributes & 0x10) || (attributes & 0x20) || (attributes == 0)) {

                // Add more space if necessary.
                if (count == capacity) {
                    capacity = capacity * 2;
                    struct aos_dirent* new_result = realloc (result, capacity * sizeof (struct aos_dirent));
                    if (new_result) {
                        result = new_result;
                    } else {
                        error = LIB_ERR_MALLOC_FAIL;
                    }
                }

                // Create the new entry.

                // FAT without extensions has 8.3 naming scheme.
                // This means we only need to copy 11 characters.
                //strncpy (&(new_entry.name[0]), sector + base_offset, 11);
                //new_entry.name [11] = '\0';
                int i;
                int end = -1;
                for (i = 7; i >= 0; i--) {
                    if (sector [base_offset + i] == ' ') {
                        if (end != -1) {
                            new_entry.name[i] = sector [base_offset + i];
                        }
                    } else {
                        if (end == -1) {
                            end = i + 1;
                        }

                        new_entry.name[i] = sector [base_offset + i];
                    }
                }

                if (sector [base_offset + 8] != ' ') {
                    new_entry.name[end] = '.';
                    end++;

                    for (i = 8; i < 11; i++) {
                        if (sector [base_offset + i] == ' ') {
                            i = 11;
                        } else {
                            new_entry.name[end] = sector [base_offset + i];
                            end++;
                        }
                    }
                }
                new_entry.name[end] = '\0';

                if (attributes & 0x10) {
                    // Set size to zero for directories.
                    new_entry.size = 0;
                    debug_printf_quiet ("Entry: %u, Directory. Attrib: %u. Name: %s. Cluster: %u\n", entry_index, attributes, new_entry.name, cluster_index_entry);
                } else {
                    // Get the file size.
                    uint32_t file_size = get_int (sector, base_offset + 0x1c);
                    new_entry.size = file_size;
                    debug_printf_quiet ("Entry: %u, File. Attrib: %u. Name: %s. Cluster: %u. Size: %u\n", entry_index, attributes, new_entry.name, cluster_index_entry, file_size);

                    // Uncomment to print file content:
//                             char filebuf [512];
//                             errval_t inner_error = fat32_driver_read_sector(cluster_to_sector_number (cluster_index_entry), filebuf);
//                             assert (err_is_ok (inner_error));
//                             debug_printf ("File contents:\n%s\n", filebuf);

                }

                result [count] = new_entry;
                ++count;
            }
            else {
                debug_printf_quiet ("Entry: %u, Unknown type. Attributes %u\n", attributes);
            }

        }

        error = stream_next (stream);
    }


    if (err_is_fail (error) || entry_list == NULL || entry_count == NULL) {
        free (result);
    } else {
        *entry_list  = result;
        *entry_count = count ;
    }

    return error;
}


// Offsets within the Volume ID block, as defined in the FAT specification.
// NOTE: When accessing these values, make sure to only dereference 4-byte aligned addresses.
#define BYTES_PER_SECTOR_OFFSET 0x0b
#define SECTORS_PER_CLUSTER_OFFSET 0x0d
#define RESERVED_SECTOR_COUNT_OFFSET 0x0e
#define FAT_COUNT_OFFSET 0x10
#define SECTORS_PER_FAT_OFFSET 0x24
#define ROOT_DIRECTORY_CLUSTER_OFFSET 0x2c
#define SIGNATURE_OFFSET 0x1fe

/// See header file.
errval_t fat32_init (struct fat32_config* config, sector_read_function_t read_function, uint32_t fat32_pbb)
{
    assert (read_function);
    assert (config);

    errval_t error = SYS_ERR_OK;
    config -> read_function = read_function;
    config -> volume_id_sector = fat32_pbb;

    // Read the volume id sector.
    uint8_t* sector [512];
    error = config->read_function (config -> volume_id_sector, sector);

    // Check the signature.
    if (err_is_ok (error) && get_short (sector, SIGNATURE_OFFSET) != 0xAA55) {
        error = FAT_ERR_BAD_FS;
    }

    if (err_is_ok (error)) {

        // Read the amount of sectors per cluster.
        // Should be 8 according to the formatting command.
        config->sectors_per_cluster = get_char (sector, SECTORS_PER_CLUSTER_OFFSET);
        if (config->sectors_per_cluster != 8) {
            // A different number of sectors per cluster has never been tested.
            debug_printf ("Warning! Sectors per cluster is %u\n", config->sectors_per_cluster);
        }

        // Read the amound of reserved sectors.
        uint32_t reserved_sector_count = get_short (sector, RESERVED_SECTOR_COUNT_OFFSET);
        if (reserved_sector_count != 0x20) {
            // A different number of reserved sectors has never been tested.
            debug_printf ("Warning! Reserved sector count in FAT filesystem is 0x%X\n", reserved_sector_count);
        }

        // Now we can get the sector index of the first File Allocation Table.
        config->fat_sector_begin = config->volume_id_sector + reserved_sector_count;

        // Read out the size and number of FAT tables.
        // NOTE: The size of a FAT depends on the disk size.
        uint32_t sectors_per_fat = get_int (sector, SECTORS_PER_FAT_OFFSET);
        uint8_t fat_count = get_char (sector, FAT_COUNT_OFFSET);
        if (fat_count != 2) {
            // A different number of FAT tables has never been tested.
            debug_printf ("Warning! Number of FAT tables is %u\n", fat_count);
        }

        // Now we can get the sector index of the first cluster in the file system.
        config->cluster_sector_begin = config->fat_sector_begin + (fat_count * sectors_per_fat);

        // Read the first cluster number of the root directory.
        config->root_directory_cluster = get_int (sector, ROOT_DIRECTORY_CLUSTER_OFFSET);
        if (config->root_directory_cluster != 2) {
            // The code should work for a root directory cluster other than two, but we never tested this.
            debug_printf ("Warning! Root directory cluster number in FAT filesystem is %u\n", config->root_directory_cluster);
        }
    }
    return error;
}
