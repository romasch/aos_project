#include "mmchs.h"

#include <limits.h>

#include <aos_support/fat32.h>
#include <aos_support/server.h>
#include <barrelfish/aos_dbg.h>

#define MAX_DIRS_SUPPORTED 32

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
        case AOS_RPC_READ_DIR:;
            {
                char* path = (char*)(&message->words[2]);

                struct aos_dirent* entry_list;
                
                size_t entry_count;

                error = fat32_read_directory(path, &entry_list, &entry_count);
                if (err_is_ok (error)) {
                    uint32_t fd = MAX_DIRS_SUPPORTED;
                        
                    for (int i = 0; (i < MAX_DIRS_SUPPORTED) && (fd == MAX_DIRS_SUPPORTED); i++) {
                        if (dir_table[MAXNAMELEN * i] == '\0') {
                            fd = i;
                        }
                    }

                    if (fd != MAX_DIRS_SUPPORTED) {
                        strcpy(&dir_table[MAXNAMELEN * fd], path);

                        lmp_chan_send3(channel, 0, NULL_CAP, error, ~fd, entry_count);
                    } else {
                        lmp_chan_send1(channel, 0, NULL_CAP,    -1                  );
                    }

                    free(entry_list);
                } else {
                    lmp_chan_send1(channel, 0, NULL_CAP, error);
                }
            }
            break;
        case AOS_RPC_READ_DIRX:;
            {
                uint32_t fd  = ~(message->words[2]);
                
                if (fd < MAX_DIRS_SUPPORTED) {
                    uint32_t idx = message->words[3];

                    struct aos_dirent* entry_list;
                
                    size_t entry_count;

                    error = fat32_read_directory(&dir_table[fd * MAXNAMELEN], &entry_list, &entry_count);
                    if (err_is_ok (error)) {
                        if (idx < entry_count) {
                            uint32_t words[7];

                            strncpy((char*)words, entry_list[idx].name, MAXNAMELEN);
                        
                            lmp_chan_send8(channel, 0, NULL_CAP, error, words[0], words[1], words[2], words[3], words[4], words[5], words[6]);
                        } else {
                            lmp_chan_send1(channel, 0, NULL_CAP,    -1                                                                      );
                        }

                        free(entry_list);
                    } else {
                        lmp_chan_send1(channel, 0, NULL_CAP, error);
                    }
                } else {
                    lmp_chan_send1(channel, 0, NULL_CAP, -1);
                }
            }
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

                    lmp_chan_send8(channel, 0, NULL_CAP, error, words[0], words[1], words[2], words[3], words[4], words[5], words[6]);
                } else {
                    lmp_chan_send1(channel, 0, NULL_CAP, error                                                                      );
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

static uint32_t parse_mbr(sector_read_function_t read_function)
{
    errval_t  error                ;
    uint32_t  starting_lba          = UINT_MAX;
    uint8_t * volume_id_sector[512];

    error = read_function(MBR_SECTOR_IDX, volume_id_sector);
    if (err_is_ok(error)) {
        starting_lba = get_int (volume_id_sector, 0x1C6U); // TODO: Now we assume the classical removable devices partitioning where we have 
                                                           // only one partition covering entire storage space.
    } else {
        debug_printf ("Warning! NULL-sector can't be read\n");
    }

    return starting_lba;
}

errval_t start_filesystem_server (void)
{
    errval_t error = SYS_ERR_OK;

    memset(dir_table, 0, sizeof(dir_table));

    error = fat32_init (mmchs_read_block, parse_mbr(mmchs_read_block));

    if (err_is_ok (error)) {
        test_fs (); // TODO remove when not needed any more.
        error = start_server (aos_service_filesystem, my_handler);
    }
    return error;
}
