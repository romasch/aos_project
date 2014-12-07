#include "mmchs.h"

#include <string.h>


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

static uint32_t cluster_to_sector_number (uint32_t cluster_number)
{
    return fat32_cluster_start + (cluster_number-2) * fat32_sectors_per_cluster;
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
