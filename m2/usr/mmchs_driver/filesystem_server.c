#include "mmchs.h"

#include <limits.h>

#include <aos_support/fat32.h>
#include <aos_support/server.h>
#include <aos_support/shared_buffer.h>
#include <barrelfish/aos_dbg.h>

#define MAX_DIRS_SUPPORTED 64

static char dir_table[MAXNAMELEN * MAX_DIRS_SUPPORTED];
    
static const uint32_t MBR_SECTOR_IDX = 0U; // MBR is always first sector
    
static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message, struct capref capability, uint32_t message_type)
{
    //uint32_t did   = message->words[1]; TODO: introduce inter-process isolation on files.
    errval_t error;

    switch (message_type) {
        case AOS_RPC_OPEN_FILE:;
            {
                uint32_t  file_descriptor;
                char    * path            = (char*)(&message->words[2]);

                error = fat32_open_file(path, &file_descriptor);

                lmp_chan_send2(channel, 0, NULL_CAP, error, file_descriptor);
            }
            break;
        case AOS_RPC_READ_DIR_NEW:;
            uint32_t mem_descriptor = message -> words [1];
            void* buffer = NULL;
            error = get_shared_buffer (mem_descriptor, &buffer, NULL);
            assert (err_is_ok (error)); //TODO
            debug_printf ("AOS_RPC_READ_DIR_NEW: Path %s, count %u\n", (char*) buffer, strlen(buffer));

            struct aos_dirent* entries;
            size_t count = 0;
            error = fat32_read_directory((char*) buffer, &entries, &count);
            if (err_is_ok (error)) {
                memcpy (buffer, entries, count * sizeof (struct aos_dirent));
                free (entries);
            }

            lmp_chan_send2 (channel, 0, NULL_CAP, error, count);
            break;
        case AOS_RPC_READ_FILE:;
            {
                void    * buf            ;
                size_t    buflen         ;
                uint32_t  file_descriptor = message->words[2];
                uint32_t  position        = message->words[3];
                size_t    size            = message->words[4];

                error = fat32_read_file(file_descriptor, position, size, &buf, &buflen);
                if (err_is_ok (error)) {
                    uint32_t words[7];

                    memcpy(words, buf, buflen);
                    free  (       buf        );

                    lmp_chan_send9(channel, 0, NULL_CAP, error, buflen, words[0], words[1], words[2], words[3], words[4], words[5], words[6]);
                } else {
                    lmp_chan_send1(channel, 0, NULL_CAP, error                                                                              );
                }
            }
            break;
        case AOS_RPC_CLOSE_FILE:;
            {
                // TODO: handle close of directory handles
                uint32_t file_descriptor = message->words[2];
                
                error = fat32_close_file(file_descriptor);

                lmp_chan_send1(channel, 0, NULL_CAP, error);
            }
            break;

        default:;
            handle_unknown_message(channel, capability);
    }
}

static inline uint16_t get_short (void* buffer, uint32_t offset)
{
    // We can't handle offsets spanning two words yet.
    assert ((offset & 1) == 0);
    return ((uint16_t*) buffer) [offset >> 1];
}

static uint32_t parse_master_boot_record (sector_read_function_t read_function)
{
    errval_t error = SYS_ERR_OK;
    uint32_t partition_start_sector = 0;
    uint8_t * master_boot_record [512];

    error = read_function(MBR_SECTOR_IDX, master_boot_record);
    if (err_is_ok(error)) {
        // TODO: We assume a partition table with only the first partition occupied by a FAT filesystem.

        // Offset to first partition table entry is 0x1be.
        // NOTE: The reading has to be split in two, because the integer is not byte aligned.
        // NOTE: Partition table is little-endian, so the LSB are at lower positions.
        uint32_t partition_start_sector_low = get_short (master_boot_record, 0x1be + 0x8);
        uint32_t partition_start_sector_high = get_short (master_boot_record, 0x1be + 0xa);

        partition_start_sector = (partition_start_sector_high << 16) | partition_start_sector_low;
    } else {
        debug_printf ("Warning! NULL-sector can't be read\n");
    }
    debug_printf_quiet ("partition_start_sector: %u\n", partition_start_sector);

    return partition_start_sector;
}

errval_t start_filesystem_server (void)
{
    errval_t error = SYS_ERR_OK;

    memset(dir_table, 0, sizeof(dir_table));

    error = fat32_init (mmchs_read_block, parse_master_boot_record (mmchs_read_block));

    debug_printf ("FAT initialized\n");

    if (err_is_ok (error)) {
//         test_fs (); // TODO remove when not needed any more.
        error = start_server (aos_service_filesystem, my_handler);
    }
    return error;
}
