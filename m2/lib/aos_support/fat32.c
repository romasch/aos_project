#include <aos_support/fat32.h>

#include <barrelfish/aos_rpc.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// #define VERBOSE
#include <barrelfish/aos_dbg.h>

#define DIRECTORY_ENTRIES 16

//The function to read a sector.
static sector_read_function_t fat32_driver_read_sector;

// Offsets to values in root cluster.
// NOTE: ARM requires 4-byte aligned addresses.
static const uint32_t bytes_per_sector_offset = 0x0b; // 2 bytes
static const uint32_t sectors_per_cluster_offset = 0x0d; // 1 bytes
static const uint32_t reserved_sectors_count_offset = 0x0e; // 2 bytes
static const uint32_t fat_count_offset = 0x10; // 1 bytes
static const uint32_t sectors_per_fat_offset = 0x24; // 4 bytes
static const uint32_t root_dir_cluster_offset = 0x2c; // 4 bytes
static const uint32_t signature_offset = 0x1fe; // 2 bytes


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

static uint32_t fat32_volumeid_block; // In the future this value may change when we support partitions.
static uint32_t fat32_fat_start;

static uint32_t fat32_cluster_start;
static uint32_t fat32_sectors_per_cluster;

static uint32_t root_directory_cluster;

static uint32_t cluster_to_sector_number (uint32_t cluster_index)
{
    return fat32_cluster_start + (cluster_index-2) * fat32_sectors_per_cluster;
}

struct sector_stream {
    uint32_t cluster_start;
    uint32_t current_cluster;
    uint32_t sector_index;
};

static void stream_init (struct sector_stream* stream, uint32_t a_cluster_start)
{
    stream -> cluster_start = a_cluster_start;
    stream -> current_cluster= a_cluster_start;
    stream -> sector_index = 0;
}


// Forward declaration.
static uint32_t fat_lookup (uint32_t cluster_index);

static bool stream_is_finished (struct sector_stream* stream)
{
    return (stream->current_cluster) >= 0x0FFFFFF8; //TODO: Define constant.
}

static errval_t stream_next (struct sector_stream* stream)
{
    stream -> sector_index++;

    if (stream -> sector_index == fat32_sectors_per_cluster) {
        stream -> sector_index = 0;
        stream -> current_cluster = fat_lookup (stream -> current_cluster);
    }
    return SYS_ERR_OK; // TODO
}

static errval_t stream_load (struct sector_stream* stream, void* buffer)
{
    uint32_t sector_to_read = cluster_to_sector_number (stream -> current_cluster);
    sector_to_read = sector_to_read + stream -> sector_index;
    errval_t error = fat32_driver_read_sector (sector_to_read, buffer);
    return error;
}


static uint32_t fat_lookup (uint32_t cluster_index)
{
    char sector [512];
    errval_t error;
    uint32_t result = 0;
    // Read the correct sector in the FAT table (consider upper 25 bits).
    uint32_t fat_sector_offset = cluster_index >> 7;
    debug_printf_quiet ("FAT sector offset: %u\n", fat_sector_offset);
    error = fat32_driver_read_sector (fat32_fat_start + fat_sector_offset, sector);

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

    }
    // TODO: Better error handling
    debug_printf_quiet ("Result: %x\n", result);
    assert (err_is_ok (error));
    return result;
}


// errval_t fat32_find_node (char* path, uint32_t* ret_cluster, uint32_t* ret_size);
static errval_t fat32_find_node (char* path, uint32_t* ret_cluster, uint32_t* ret_size)
{
    debug_printf_quiet ("Find node %s\n", path);

    uint32_t path_index = 0;
    errval_t error = SYS_ERR_OK;
    bool end_of_path = false;
    bool found_entry = false;

    uint32_t cluster_index = root_directory_cluster;
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
            stream_init (stream, cluster_index);
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

static uint32_t descriptor_to_size [1024];
static uint32_t descriptor_to_cluster [1024];
static uint32_t descriptor_count = 0;

errval_t fat32_open_file (char* path, uint32_t* file_descriptor)
{
    assert (path != NULL && file_descriptor != NULL);
    assert (descriptor_count < 1024); // TODO: A dynamic structure.

    uint32_t ret_cluster = 0;
    uint32_t ret_size = 0;
    errval_t error = SYS_ERR_OK;

    error = fat32_find_node (path, &ret_cluster, &ret_size);

    if (err_is_ok (error)) {
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
    stream_init (stream, cluster_index);
    uint32_t early_stop = false;
    char sector [512];

    uint32_t file_index = 0;
    uint32_t chars_read = 0;
    uint32_t buffer_size = min (file_size - position, size);
    int8_t* buffer = malloc (buffer_size+1);
    if (!buffer) {
        error = LIB_ERR_MALLOC_FAIL;
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


//     // TODO: More graceful way of handling closed files.
//     assert (cluster_index != 0 && file_size != 0);
//
//     errval_t error = SYS_ERR_OK;
//
//     char sector [512];
//     uint32_t sector_index = cluster_to_sector_number (cluster_index);
//     error = fat32_driver_read_sector (sector_index, sector);
//
     if (err_is_ok (error)) {
//         uint32_t buffer_size = min (file_size - position, size);
//         int8_t* buffer = malloc (buffer_size+1); // TODO: errors
//         assert (buffer != NULL);
//
//         for (int i=0; i<buffer_size; i++) {
//             buffer [i] = sector [i + position];
//         }
//         // TODO: Do we actually need to null-terminate the buffer?
//         buffer [buffer_size] = '\0';
//         debug_printf_quiet ("File contents: %s \n", (char*) buffer);
//
        *buf = buffer;
        *buflen = buffer_size;
    }
    return error;
}


errval_t fat32_read_directory (char* path, struct aos_dirent** entry_list, size_t* entry_count)
{
    uint32_t cluster_index = 0;
    uint32_t ret_size;
    struct aos_dirent new_entry;

    errval_t error = fat32_find_node (path, &cluster_index, &ret_size);

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
    stream_init (stream, cluster_index);
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


errval_t fat32_init (sector_read_function_t read_function, uint32_t fat32_pbb)
{
    assert (read_function != NULL);
    errval_t error = SYS_ERR_OK;
    fat32_driver_read_sector = read_function;

    // Read the first block from sector.
    // NOTE: When using partition tables we'll have to change this constant.
    fat32_volumeid_block = fat32_pbb;
    uint8_t* volume_id_sector [512];
    error = fat32_driver_read_sector (fat32_volumeid_block, volume_id_sector);

    if (err_is_ok (error)) {

        // Check signature
        assert (get_short (volume_id_sector, signature_offset) == 0xaa55);

        // Read the amount of sectors per cluster.
        // Should be 8 according to the formatting command.
        fat32_sectors_per_cluster = get_char (volume_id_sector, sectors_per_cluster_offset);
        assert (fat32_sectors_per_cluster == 8);

        // Read the amound of reserved sectors.
        uint32_t reserved_sector_count = get_short (volume_id_sector, reserved_sectors_count_offset);
        if (reserved_sector_count != 0x20) {
            // The code should work for reserved sectors other than 0x20, but we never tested this.
            debug_printf ("Warning! Reserved sector count in FAT filesystem is 0x%X\n", reserved_sector_count);
        }

        // Now we can get the sector index of the first File Allocation Table.
        fat32_fat_start = fat32_volumeid_block + reserved_sector_count;

        // Read out the size and number of FAT tables.
        // NOTE: The size of a FAT depends on the disk size.
        uint32_t sectors_per_fat = get_int (volume_id_sector, sectors_per_fat_offset);
        uint8_t fat_count = get_char (volume_id_sector, fat_count_offset);
        assert (fat_count == 2);

        // Now we can get the sector index of the first cluster in the file system.
        fat32_cluster_start = fat32_fat_start + (fat_count * sectors_per_fat);

        // Read the first cluster number of the root directory.
        root_directory_cluster = get_int (volume_id_sector, root_dir_cluster_offset);
        if (root_directory_cluster != 2) {
            // The code should work for a root directory cluster other than two, but we never tested this.
            debug_printf ("Warning! Root directory cluster number in FAT filesystem is %u\n", root_directory_cluster);
        }
    }
    return error;
}


void test_fs (void)
{
    /////// Read the first block

    void *root_cluster = malloc(512);
    assert(root_cluster != NULL);
    errval_t err = fat32_driver_read_sector(cluster_to_sector_number (root_directory_cluster), root_cluster);
    assert(err_is_ok(err));
#ifdef VERBOSE
    debug_printf_quiet ("Read block %d:\n", cluster_to_sector_number (root_directory_cluster));
    for (int i = 1; i <= 512; ++i)
    {
        printf ("%"PRIu8"\t", ((uint8_t*) root_cluster)[i-1]);
        if (i % 4 == 0) {
            printf("\n");
        }
    }
#endif

    /////// Read the file at path /asdf/a.txt

    uint32_t fd = 0;
    fat32_open_file ("/testdir/hello.txt", &fd);
    void* buf;
    size_t buflen;
    fat32_read_file (fd, 0, 100, &buf, &buflen);
    debug_printf_quiet ("Size of read file: %u\n", buflen);
    debug_printf_quiet ("File: %s\n", buf);

    /////// Read some directories.

    debug_printf_quiet ("Reading directory: / \n\n");
    fat32_read_directory ("/",0,0);

    debug_printf_quiet ("Reading directory: /testdir \n\n");
    fat32_read_directory ("/testdir/", 0, 0);

    debug_printf_quiet ("Reading directory: /asdf \n\n");
    fat32_read_directory ("/asdf", 0, 0);

    for (int i=0; i<10; i++) {
        debug_printf_quiet ("Reading directory: / \n\n");
        fat32_read_directory ("/",0,0);
    }

    debug_printf_quiet ("Finished tests\n");
}
