#include "mmchs.h"

#include <barrelfish/aos_rpc.h>
#include <string.h>
#include <ctype.h>

#define VERBOSE

#include <barrelfish/aos_dbg.h>

#define DIRECTORY_ENTRIES 16

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


static uint32_t fat32_volumeid_block = 0; // In the future this value may change when we support partitions.
static uint32_t fat32_fat_start;

static uint32_t fat32_cluster_start;
static uint32_t fat32_sectors_per_cluster;

static uint32_t root_directory_cluster;

static uint32_t cluster_to_sector_number (uint32_t cluster_index)
{
    return fat32_cluster_start + (cluster_index-2) * fat32_sectors_per_cluster;
}

errval_t fat32_find_node (char* path, uint32_t* ret_cluster, uint32_t* ret_size);
errval_t fat32_find_node (char* path, uint32_t* ret_cluster, uint32_t* ret_size)
{
    uint32_t path_index = 0;
    errval_t error = SYS_ERR_OK;
    bool end_of_path = false;

    while (!end_of_path) {
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

            //TODO: Follow paths here.

        }
    }
    return error;
}


errval_t fat32_read_directory (char* path, struct aos_dirent** entry_list, size_t* entry_count);
errval_t fat32_read_directory (char* path, struct aos_dirent** entry_list, size_t* entry_count)
{
    // TODO: Paths other than root.
    uint32_t cluster_index = root_directory_cluster;
    fat32_find_node ("/echo/asdf.txt/hello world how are you/blub//test.exe/", 0, 0);

    errval_t error = SYS_ERR_OK;

    uint32_t capacity = 1;
    uint32_t count = 0;
    struct aos_dirent** result;
    result = malloc (capacity * sizeof (struct aos_dirent*));

     while (cluster_index != 0xFFFFFFFF && err_is_ok (error)) {
        char sector [512];
        error = mmchs_read_block (cluster_to_sector_number (cluster_index), sector);

        if (err_is_ok (error)) {

            for (int entry_index = 0; entry_index < DIRECTORY_ENTRIES; entry_index++) {

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
                }
                // Long file name from VFAT extension.
                else if ( (attributes & 0xF) == 0xF) {
                    debug_printf_quiet ("Entry: %u, Long filename. Attributes: %u\n", entry_index, attributes);
                }
                // This is a file or directory. Finally, something to do.
                else if ((attributes & 0x10) || (attributes & 0x20) || (attributes == 0)) {

                    // Add more space if necessary.
                    if (count == capacity) {
                        capacity = capacity * 2;
                        struct aos_dirent** new_result = realloc (result, capacity * sizeof (struct aos_dirent*));
                        if (new_result) {
                            result = new_result;
                        } else {
                            error = LIB_ERR_MALLOC_FAIL;
                        }
                    }

                    // Create the new entry.
                    struct aos_dirent* new_entry = malloc (sizeof (struct aos_dirent));

                    if (new_entry) {

                        // FAT without extensions has 8.3 naming scheme.
                        // This means we only need to copy 11 characters.
                        strncpy (&(new_entry->name[0]), sector + base_offset, 11);
                        new_entry->name [11] = '\0';

                        if (attributes & 0x10) {
                            // Set size to zero for directories.
                            new_entry -> size = 0;
                            debug_printf_quiet ("Entry: %u, Directory. Attrib: %u. Name: %s. Cluster: %u\n", entry_index, attributes, &new_entry->name, cluster_index_entry);
                        } else {
                            // Get the file size.
                            uint32_t file_size = get_int (sector, base_offset + 0x1c);
                            new_entry -> size = file_size;
                            debug_printf_quiet ("Entry: %u, File. Attrib: %u. Name: %s. Cluster: %u. Size: %u\n", entry_index, attributes, &new_entry->name, cluster_index_entry, file_size);
                        }

                        result [count] = new_entry;
                        ++count;
                    } else {
                        error = LIB_ERR_MALLOC_FAIL;
                    }
                }
                else {
                    printf ("Entry: %u, Unknown type.\n");
                }

            }
        }

        // TODO: get next cluster index from File Allocation Table.
        cluster_index = 0xFFFFFFFF;

    }



    if (err_is_fail (error) || entry_list == NULL || entry_count == NULL) {
        for (int i=0; i<count; i++) {
            free (result[i]);
        }
        free (result);
    } else {
        *entry_list = *result;
        *entry_count = count;
    }

    return error;
}



void test_fs (void* buffer)
{

//     assert (get_short (buffer, bytes_per_sector_offset) == 512);

    // Check signature
    assert (get_short (buffer, signature_offset) == 0xaa55);


    fat32_sectors_per_cluster = get_char (buffer, sectors_per_cluster_offset);
    assert (fat32_sectors_per_cluster == 8);

    uint32_t reserved_sector_count = get_short (buffer, reserved_sectors_count_offset);
    assert (reserved_sector_count == 0x20);

    fat32_fat_start = fat32_volumeid_block + reserved_sector_count;

    uint8_t fat_count = get_char (buffer, fat_count_offset);
    assert (fat_count == 2);

    uint32_t sectors_per_fat = get_int (buffer, sectors_per_fat_offset); // depends on disk size.

    fat32_cluster_start = fat32_fat_start + (fat_count * sectors_per_fat);

    root_directory_cluster = get_int (buffer, root_dir_cluster_offset);
    debug_printf ("Root cluster: %u, sector %u\n", root_directory_cluster, cluster_to_sector_number (root_directory_cluster));


    fat32_read_directory (0,0,0);
    debug_printf ("Finished test\n");


    void *root_cluster = malloc(512);
    assert(root_cluster != NULL);

    errval_t err = mmchs_read_block(cluster_to_sector_number (root_directory_cluster), root_cluster);
    assert(err_is_ok(err));
    printf("Read block %d:\n", 0);
    for (int i = 1; i <= 512; ++i)
    {
        printf("%"PRIu8"\t", ((uint8_t*) root_cluster)[i-1]);
        if (i % 4 == 0) {
            printf("\n");
        }
    }


    // Read the directory entries of the root cluster.
    for (uint32_t base = 0; base < 512; base += 32) {


        uint8_t first_byte = get_char (root_cluster, base);

        uint8_t attrib = get_char (root_cluster, base + 0x0b);


        if (first_byte == 0xe5) { // Deleted file
            printf ("Entry: %u, Deleted.\n", base/32);
        }
        else if (first_byte == 0x0) {
           printf ("Entry: %u, Null entry.\n", base/32);
        }
        else if ( (attrib & 0xF) == 0xF) { // Long file name.
           printf ("Entry: %u, Long filename. Attrib: %u\n", base/32, attrib);
        }
        else {
            char filename[12];
            strncpy (&filename[0], root_cluster + base, 11);
            filename [11] = '\0';


            uint32_t cluster_high = get_short (root_cluster, base + 0x14);
            uint16_t cluster_low = get_short (root_cluster, base + 0x1a);

            uint32_t cluster_entry = (cluster_high << 16) | cluster_low;

            if (attrib & 0x10) { // Subdirectory
                printf ("Entry: %u, Directory. Attrib: %u. Name: %s. Cluster: %u\n", base/32, attrib, filename, cluster_entry);
            } else if ((attrib & 0x20) || (attrib == 0)) { // Normal file
                uint32_t file_size = get_int (root_cluster, base + 0x1c);
                printf ("Entry: %u, File. Attrib: %u. Name: %s. Cluster: %u. Size: %u\n", base/32, attrib, filename, cluster_entry, file_size);

                char filebuf [512];
                err = mmchs_read_block(cluster_to_sector_number (cluster_entry), filebuf);
                assert (err_is_ok (err));
                printf ("File contents:\n");
                printf ("%s\n",filebuf);
            } else {
                printf ("Entry: %u, Unknown type.\n");
            }
        }
    }
}
