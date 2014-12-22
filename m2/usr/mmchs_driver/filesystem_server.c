#include "mmchs.h"

#include <limits.h>

#include <aos_support/fat32.h>
#include <aos_support/server.h>
#include <aos_support/shared_buffer.h>
#include <barrelfish/aos_dbg.h>

static const uint32_t MBR_SECTOR_IDX = 0U; // MBR is always first sector

static struct fat32_config my_config;


static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message, struct capref capability, uint32_t message_type)
{
    //uint32_t did   = message->words[1]; TODO: introduce inter-process isolation on files.
    errval_t error;
    uint32_t memory_descriptor = 0;
    uint32_t file_descriptor = 0;

    switch (message_type) {
        case AOS_RPC_OPEN_FILE:;
            {
                char* path = (char*)(&message->words[2]);

                error = fat32_open_file(&my_config, path, &file_descriptor);

                lmp_chan_send2(channel, 0, NULL_CAP, error, file_descriptor);
            }
            break;
        case AOS_RPC_READ_DIR_NEW:;
            memory_descriptor = message -> words [1];
            void* buffer = NULL;
            error = get_shared_buffer (memory_descriptor, &buffer, NULL);
            assert (err_is_ok (error)); //TODO
            debug_printf_quiet ("AOS_RPC_READ_DIR_NEW: Path %s, count %u\n", (char*) buffer, strlen(buffer));

            struct aos_dirent* entries;
            size_t count = 0;
            error = fat32_read_directory(&my_config, (char*) buffer, &entries, &count);
            if (err_is_ok (error)) {
                memcpy (buffer, entries, count * sizeof (struct aos_dirent));
                free (entries);
            }

            lmp_chan_send2 (channel, 0, NULL_CAP, error, count);
            break;
        case AOS_RPC_READ_FILE:;
            memory_descriptor = message -> words [1];
            file_descriptor = message -> words [2];
            uint32_t position = message -> words [3];
            uint32_t size = message -> words [4];

            void* result_buffer = NULL;
            uint32_t result_buffer_length = 0;
            error = get_shared_buffer (memory_descriptor, &result_buffer, &result_buffer_length);

            size_t characters_read = 0;

            // TODO: Send back an error.
            assert (size <= result_buffer_length);

             if (err_is_ok (error)) {
                // TODO: It would be nice if we could fill the shared buffer directly instead of using memcpy.
                void* buf = NULL;
                error = fat32_read_file (file_descriptor, position, size, &buf, &characters_read);

                if (err_is_ok (error)) {
                    memcpy (result_buffer, buf, characters_read);
                    free (buf);
                }
            }
            lmp_chan_send2 (channel, 0, NULL_CAP, error, characters_read);
            break;
        case AOS_RPC_CLOSE_FILE:;
            {
                file_descriptor = message->words[2];
                
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

    error = fat32_init (&my_config, mmchs_read_block, parse_master_boot_record (mmchs_read_block));

    debug_printf ("FAT initialized\n");

    if (err_is_ok (error)) {
//         test_fs (); // TODO remove when not needed any more.
        error = start_server (aos_service_filesystem, my_handler);
    }
    return error;
}
