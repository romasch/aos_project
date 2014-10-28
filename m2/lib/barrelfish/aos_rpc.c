/**
 * \file
 * \brief Implementation of AOS rpc-like messaging
 */

/*
 * Copyright (c) 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/aos_rpc.h>
#include <barrelfish/lmp_chan_arch.h>

errval_t aos_rpc_send_string(struct aos_rpc *chan, const char *string)
{
    debug_printf ("aos_rpc_send_string(%u, %s)\n", chan, string);
    
    errval_t error = SYS_ERR_INVARGS_SYSCALL;

    if ((chan != NULL) && (chan->data_type == UNDEFINED)) {
        bool finished = false;
        int  indx = 0;

        chan->data_type = NT_STRING;
	
        for (; finished == false ;) {
            uint32_t buf[LMP_MSG_LENGTH] = {0,0,0,0,0,0,0,0,0};
	
            for (int i = 0; (i < (sizeof(buf)/sizeof(buf[0]))) && (finished == false); i++) {
	        for (int j = 0; j < 4; j ++) {
                    buf[i] |= (uint32_t)(string[indx]) << (8 * j);
                    
                    if (string[indx] == '\0') {
                        finished = true;
                    }                

                    indx++;
                }            
            }

            error = lmp_ep_send9(chan->target, LMP_FLAG_SYNC | LMP_FLAG_YIELD, NULL_CAP, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
            if (error != SYS_ERR_OK) {
                finished = true;

                debug_printf("aos_rpc_send_string(0x%x, %s) experienced error %u\n", chan, string, error);
            }
        }
        
        debug_printf("aos_rpc_send_string(0x%x, %s) trying to send string\n", chan, string);
    }

    return error;
}

errval_t aos_rpc_get_ram_cap(struct aos_rpc *chan, size_t request_bits,
                             struct capref *retcap, size_t *ret_bits)
{
    // TODO: implement functionality to request a RAM capability over the
    // given channel and wait until it is delivered.
    // TODO:YK
    return SYS_ERR_OK;
}

errval_t aos_rpc_get_dev_cap(struct aos_rpc *chan, lpaddr_t paddr,
                             size_t length, struct capref *retcap,
                             size_t *retlen)
{
    // TODO (milestone 4): implement functionality to request device memory
    // capability.
    return SYS_ERR_OK;
}

errval_t aos_rpc_serial_getchar(struct aos_rpc *chan, char *retc)
{
    // TODO (milestone 4): implement functionality to request a character from
    // the serial driver.
    return SYS_ERR_OK;
}


errval_t aos_rpc_serial_putchar(struct aos_rpc *chan, char c)
{
    // TODO (milestone 4): implement functionality to send a character to the
    // serial port.
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_spawn(struct aos_rpc *chan, char *name,
                               domainid_t *newpid)
{
    // TODO (milestone 5): implement spawn new process rpc
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_name(struct aos_rpc *chan, domainid_t pid,
                                  char **name)
{
    // TODO (milestone 5): implement name lookup for process given a process
    // id
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_all_pids(struct aos_rpc *chan,
                                      domainid_t **pids, size_t *pid_count)
{
    // TODO (milestone 5): implement process id discovery
    return SYS_ERR_OK;
}

errval_t aos_rpc_open(struct aos_rpc *chan, char *path, int *fd)
{
    // TODO (milestone 7): implement file open
    return SYS_ERR_OK;
}

errval_t aos_rpc_readdir(struct aos_rpc *chan, char* path,
                         struct aos_dirent **dir, size_t *elem_count)
{
    // TODO (milestone 7): implement readdir
    return SYS_ERR_OK;
}

errval_t aos_rpc_read(struct aos_rpc *chan, int fd, size_t position, size_t size,
                      void** buf, size_t *buflen)
{
    // TODO (milestone 7): implement file read
    return SYS_ERR_OK;
}

errval_t aos_rpc_close(struct aos_rpc *chan, int fd)
{
    // TODO (milestone 7): implement file close
    return SYS_ERR_OK;
}

errval_t aos_rpc_write(struct aos_rpc *chan, int fd, size_t position, size_t *size,
                       void *buf, size_t buflen)
{
    // TODO (milestone 7): implement file write
    return SYS_ERR_OK;
}

errval_t aos_rpc_create(struct aos_rpc *chan, char *path, int *fd)
{
    // TODO (milestone 7): implement file create
    return SYS_ERR_OK;
}

errval_t aos_rpc_delete(struct aos_rpc *chan, char *path)
{
    // TODO (milestone 7): implement file delete
    return SYS_ERR_OK;
}

errval_t aos_rpc_init(struct aos_rpc *rpc, struct capref receiver)
{
    debug_printf ("aos_rpc_init(0x%x)\n", rpc);

    errval_t error = SYS_ERR_INVARGS_SYSCALL;

    if (rpc != NULL) {
        debug_printf ("aos_rpc_init(0x%x) succed\n", rpc);

        rpc->target    = receiver ;
        rpc->data_type = UNDEFINED;

	error = SYS_ERR_OK;
    }

    return error;
}
