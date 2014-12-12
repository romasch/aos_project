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

#include <stdio.h>

#include <barrelfish/aos_rpc.h>
#include <barrelfish/lmp_chan.h>

#include <barrelfish/aos_dbg.h>

#define SHARED_BUFFER_DEFAULT_SIZE_BITS 20

static inline void print_error (errval_t error, char* fmt, ...)
{
    if (err_is_fail (error)) {
        debug_printf (""); // print domain id
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
}

/// Predefined channels available in every domain.
// NOTE: We can't use malloc here, because the initial ram allocator
// only allows frames of one page size, and our paging code doesn't allow this.
static struct aos_rpc init_channel;

static errval_t aos_rpc_setup_shared_buffer (struct aos_rpc* rpc, uint8_t size_bits);

struct aos_rpc* aos_rpc_get_init_channel (void)
{
    return &init_channel;
}

static struct aos_rpc serial_driver_channel;

struct aos_rpc* aos_rpc_get_serial_driver_channel (void)
{
    return &serial_driver_channel;
}

/**
 * Support structure to store arguments in receive handler.
 */
struct lmp_message_args {
    struct lmp_chan* channel;
    struct capref cap;
    struct lmp_recv_msg message;
    errval_t error;
};

static void init_lmp_message_args (struct lmp_message_args* args, struct lmp_chan* channel)
{
    args -> channel = channel;
    args -> cap = NULL_CAP;
    args -> error = SYS_ERR_OK;

    // somehow the LMP_RECV_MSG_INIT only works this way...
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    args -> message = msg;

    for (int i=0; i < LMP_MSG_LENGTH; i++ ) {
        args -> message.words [i] = 0;
    }
}

/**
 * A generic response handler that just fills in a struct lmp_message_args.
 *
 * \param arg: must be a valid struct lmp_message_args*
 */
static void aos_generic_response_handler (void* arg)
{
    struct lmp_message_args* args = arg;
    debug_printf_quiet ("aos_generic_response_handler, arg %p, channel %p...\n", arg, args->channel);

    // Make sure we don't return stale values...
    // NOTE: Once we're sure that everything works we can also uncomment this.
    args -> cap = NULL_CAP;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    args -> message = msg;

    // Read values from channel.
    args -> error = lmp_chan_recv(args -> channel, &(args->message), &(args->cap));
    print_error (args -> error, "aos_generic_response_handler: error code: %s\n", err_getstring (args -> error));
}

/**
 * Generic function to send a request and wait for a response.
 * Arguments can be given in the storage struct.
 * Any return value will be stored in the same struct.
 */
static errval_t aos_send_receive (struct lmp_message_args* storage, bool needs_receive_cap)
{
    debug_printf_quiet ("aos_send_receive, storage %p, channel %p... \n", storage, storage -> channel);

    errval_t error = SYS_ERR_OK;

    // Set up the receive handler.
    error = lmp_chan_register_recv (storage->channel, get_default_waitset(), MKCLOSURE (aos_generic_response_handler, storage));
    print_error (error, "aos_send_receive: message sent. Error code: %s, channel %p\n", err_getstring (error), storage -> channel);

    if (err_is_ok (error)) {

        // Send the request.
        error = lmp_chan_send9 (
            storage -> channel, // Channel to send on.
            0, // Don't give up processor (yet).
            storage -> cap, // Capability to send.
            storage -> message.words [0],
            storage -> message.words [1],
            storage -> message.words [2],
            storage -> message.words [3],
            storage -> message.words [4],
            storage -> message.words [5],
            storage -> message.words [6],
            storage -> message.words [7],
            storage -> message.words [8]
        );
        print_error (error, "aos_send_receive: message sent. Error code: %s, channel %p\n", err_getstring (error), storage -> channel);

        if (err_is_ok (error)) {

            // Yield processor and wait for response.
            error = event_dispatch (get_default_waitset());
            print_error (error, "aos_send_receive: message received. Error code: %s, channel %p\n", err_getstring (error), storage -> channel);

            // Re-allocate a new slot for the next incoming message.
            // TODO: It might be better to reserve them lazily.
            // TODO: In case of error we may sometimes be able to reuse the existing slot.
            if (needs_receive_cap) {
                error = lmp_chan_alloc_recv_slot (storage -> channel);
                print_error (error, "aos_send_receive: reallocated. Error code: %s\n", err_getstring (error));
            }
        }

        if (err_is_fail (error)) {
            // Send or receive failed. Deregister event handler and return.
            error = lmp_chan_deregister_recv (storage -> channel);
        }
    }
    return error;
}

static bool str_to_args(const char* string, uint32_t* args, size_t args_length, int* indx, bool finished)
{
    finished = false;

    for (int i = 0; (i < args_length) && (finished == false); i++) {
        for (int j = 0; (j < 4) && (finished == false); j++, (*indx)++) {
            args[i] |= (uint32_t)(string[*indx]) << (8 * j);

            if ((string[*indx] == '\0') && (finished == false)) {
                finished = true;
            }
        }
    }

    return finished;
}

errval_t aos_rpc_send_string(struct aos_rpc *chan, const char *string)
{
    debug_printf_quiet ("aos_rpc_send_string. String to send (may be truncated): %s\n", string);

    errval_t error = SYS_ERR_OK;

    if (chan && chan -> shared_buffer == NULL) {
        error = aos_rpc_setup_shared_buffer (chan, SHARED_BUFFER_DEFAULT_SIZE_BITS);
        debug_printf_quiet ("Initialized buffer: %s\n", err_getstring (error));
    }

    assert (chan -> shared_buffer != NULL);
    assert (chan -> shared_buffer_length != 0);

    if (chan && err_is_ok (error)) {
        // TODO: Gracefully handle this assertion.
        assert (strlen (string) <= chan->shared_buffer_length);

        // Put the string into the shared buffer.
        char* as_string_buffer = chan -> shared_buffer;
        strncpy (as_string_buffer, string, chan -> shared_buffer_length);

        struct lmp_message_args args;
        init_lmp_message_args (&args, &(chan->channel));

        // Set up the send arguments.
        args.message.words [0] = AOS_RPC_SEND_STRING;
        args.message.words [1] = chan -> memory_descriptor;

        // Do the IPC call.
        error = aos_send_receive (&args, false);
        print_error (error, "aos_rpc_send_string: communication failed. %s\n", err_getstring (error));

        // Get the result.
        if (err_is_ok (error)) {
            error = args.message.words [0];
        }
    } else {
        error = SYS_ERR_INVARGS_SYSCALL;
    }
    return error;
}

errval_t aos_rpc_get_ram_cap(struct aos_rpc *chan, size_t request_bits,
                             struct capref *retcap, size_t *ret_bits)
{
    // Request a RAM capability over the given channel
    // and wait until it is delivered.

    struct lmp_chan* channel = &chan->channel;
    errval_t error = SYS_ERR_OK;

    // Initialize storage for message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the send arguments.
    args.message.words [0] = AOS_RPC_GET_RAM_CAP;
    args.message.words [1] = request_bits;

    // Do the IPC call.
    error = aos_send_receive (&args, true);
    print_error (error, "aos_rpc_get_ram_cap: communication failed. %s\n", err_getstring (error));

    // Get the result.
    if (err_is_ok (error)) {

        error = args.message.words [0];
        print_error (error, "aos_rpc_get_ram_cap: RAM allocation failed. %s\n", err_getstring (error));

        if (err_is_ok (error)) {
            *ret_bits = args.message.words [1];
            *retcap = args.cap;
        }
    }
    return error;
}

errval_t aos_rpc_get_dev_cap(struct aos_rpc *chan, lpaddr_t paddr,
                             size_t length, struct capref *retcap,
                             size_t *retlen)
{
    assert (retcap);
    // Request device memory capability.
    struct lmp_chan* channel = &chan->channel;
    errval_t error = SYS_ERR_OK;

    // Initialize storage for message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the send arguments.
    uint8_t length_bits = log2ceil (length);
    if (length_bits < BASE_PAGE_BITS) {
        length_bits = BASE_PAGE_BITS;
    }

    args.message.words [0] = AOS_RPC_GET_DEVICE_FRAME;
    args.message.words [1] = paddr;
    args.message.words [2] = length_bits;


    // Do the IPC call.
    error = aos_send_receive (&args, true);
    print_error (error, "aos_rpc_get_dev_cap: communication failed. %s\n", err_getstring (error));

    // Get the result.
    if (err_is_ok (error)) {

        error = args.message.words [0];
        print_error (error, "aos_rpc_get_dev_cap: operation failed. %s\n", err_getstring (error));

        if (err_is_ok (error)) {
            *retcap = args.cap;
            if (retlen) {
                *retlen = 1u << length_bits;
            }
        }
    }
    return error;

    return SYS_ERR_OK;
}

errval_t aos_rpc_serial_getchar(struct aos_rpc *chan, char *retc)
{
    // Request a character from the serial driver.

    struct lmp_chan* channel = &chan->channel;
    errval_t error = SYS_ERR_OK;

    // Initialize storage for message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the send arguments.
    args.message.words [0] = AOS_RPC_SERIAL_GETCHAR;
    args.message.words [1] = disp_get_domain_id();

    // Do the IPC call.
    error = aos_send_receive (&args, false);
    print_error (error, "aos_rpc_serial_getchar: communication failed. %s\n", err_getstring (error));

    // Get the result.
    if (err_is_ok (error)) {

        error = args.message.words [0];
        print_error (error, "aos_rpc_serial_getchar: operation failed. %s\n", err_getstring (error));

        if (err_is_ok (error)) {
            *retc = args.message.words [1];
        }
    }
    return error;
}


errval_t aos_rpc_serial_putchar(struct aos_rpc *chan, char c)
{
    // Send a character to the serial port.
    // TODO: Shall we change the protocol to wait for acknowledgement?
    struct lmp_chan* channel = &chan->channel;

    uint32_t flags = LMP_FLAG_SYNC | LMP_FLAG_YIELD;
    lmp_chan_send2 (channel, flags, NULL_CAP, AOS_RPC_SERIAL_PUTCHAR, c);
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_spawn(struct aos_rpc *chan, char *name, domainid_t *newpid)
{
    // Request a creation of new process from the binary packed into boot image.
    errval_t error = -1; // Consider -1 as a sign of general error

    if ((chan != NULL) && (name != NULL) && (newpid != NULL)) {
        if (strlen(name) <= MAX_PROCESS_NAME_LENGTH) {
            struct lmp_message_args  args   ;
            struct lmp_chan        * channel = &chan->channel;

            int indx = 0;
    
            init_lmp_message_args (&args, channel);

            args.message.words [0] = AOS_RPC_SPAWN_PROCESS;

            str_to_args(&name[indx], &args.message.words[1], 8, &indx, false);
            
            // Do the IPC call.
            error = aos_send_receive(&args, true);
            print_error (error, "aos_rpc_process_spawn: communication failed. %s\n", err_getstring (error));

            // Get the result.
            if (err_is_ok (error)) {

                error = args.message.words [0];
                print_error (error, "aos_rpc_process_spawn: operation failed. %s\n", err_getstring (error));

                if (err_is_ok (error)) {
                    *newpid = args.message.words [1];
                }
            }
        }
    }

    return error;
}

errval_t aos_rpc_process_spawn_remotely(struct aos_rpc *chan, char *name, coreid_t core_id, domainid_t *newpid)
{
    // Lets assume that we have only two cores. 
    // We ignore third parameter "core id" in pass request to the opposite core.
    core_id = 0;

    // Request a creation of new process from the binary packed into boot image.
    errval_t error = -1; // Consider -1 as a sign of general error

    if ((chan != NULL) && (name != NULL) && (newpid != NULL)) {
        if (strlen(name) <= MAX_PROCESS_NAME_LENGTH) {
            struct lmp_message_args  args   ;
            struct lmp_chan        * channel = &chan->channel;

            int indx = 0;
    
            init_lmp_message_args (&args, channel);

            args.message.words [0] = AOS_RPC_SPAWN_PROCESS_REMOTELY;

            str_to_args(&name[indx], &args.message.words[1], 8, &indx, false);
            
            // Do the IPC call.
            error = aos_send_receive(&args, true);
            print_error (error, "aos_rpc_process_spawn_remotely: communication failed. %s\n", err_getstring (error));

            // Get the result.
            if (err_is_ok (error)) {

                error = args.message.words [0];
                print_error (error, "aos_rpc_process_spawn_remotely: operation failed. %s\n", err_getstring (error));

                if (err_is_ok (error)) {
                    *newpid = args.message.words [1];
                }
            }
        }
    }

    return error;
}

errval_t aos_rpc_process_get_name(struct aos_rpc *chan, domainid_t pid,
                                  char **name)
{
    // Name lookup for process given a process id
    errval_t error = -1; // Consider -1 as a sign of general error
    
    if ((chan != NULL) && (name != NULL)) {
        struct lmp_message_args  args   ;
        struct lmp_chan        * channel = &chan->channel;
    
        init_lmp_message_args (&args, channel);

        args.message.words [0] = AOS_RPC_GET_PROCESS_NAME;
        args.message.words [1] = pid;

        // Do the IPC call.
        error = aos_send_receive(&args, true);
        print_error (error, "aos_rpc_process_get_name: communication failed. %s\n", err_getstring (error));

        // Get the result.
        if (err_is_ok (error)) {

            error = args.message.words [0];
            print_error (error, "aos_rpc_process_get_name: operation failed. %s\n", err_getstring (error));

            if (err_is_ok (error)) {
                *name = malloc(MAX_PROCESS_NAME_LENGTH);
                strcpy(*name, (char*)&args.message.words [1]);
                // TODO
            }
        }
    }

    return error;
}

errval_t aos_rpc_process_get_all_pids(struct aos_rpc *chan,
                                      domainid_t **pids, size_t *pid_count)
{
    // Process id discovery
    errval_t error = 0;
    
    if ((chan != NULL) && (pids != NULL) && (pid_count != NULL)) {
        struct lmp_message_args args;
        struct lmp_chan* channel = &chan->channel;

        int idx = 0;
        *pids = NULL;
        *pid_count = 0;
        for (; (idx != 0xffffffff) && err_is_ok (error);) {

            init_lmp_message_args (&args, channel);
            args.message.words [0] = AOS_RPC_GET_PROCESS_LIST;
            args.message.words [1] = idx                     ;
            // Do the IPC call.
            error = aos_send_receive(&args, false);
            print_error (error, "aos_rpc_process_get_all_pids: communication failed. %s\n", err_getstring (error));
          
            if (err_is_ok (args.message.words[0])) {
                idx = args.message.words [1];
                
                for (int i = 0; i < 7; i++) {
                    if (args.message.words[2 + i] != 0xffffffff) {
                        if ((*pid_count & (*pid_count - 1)) == 0) {
                            *pids = realloc(*pids, sizeof(domainid_t) * ((*pid_count == 0) ? (1) : (*pid_count * 2)));
                        }

                        (*pids)[*pid_count] = args.message.words[2 + i];
                        (*pid_count)++;
                    } else {
                        i = 7;
                    }
                }
            }
        }
    }

    return error;
}

#define MAX_PATH 7 * 4 - 1

errval_t aos_rpc_open(struct aos_rpc *chan, char *path, int *fd)
{
    // Open file from the removable storage

    errval_t error = -1;

    if ((chan != NULL) && (path != NULL) && (fd != NULL)) {
        if (strlen(path) <= MAX_PATH) {
            struct lmp_message_args  args   ;
            struct lmp_chan        * channel = &chan->channel;
    
            init_lmp_message_args(&args, channel);

            args.message.words[0] = AOS_RPC_OPEN_FILE   ;
            args.message.words[1] = disp_get_domain_id();

            strcpy((char*)(&args.message.words[2]), path);

            error = aos_send_receive (&args, false);
            print_error (error, "aos_rpc_open: communication failed. %s\n", err_getstring (error));

            if (err_is_ok (error)) {
                error = args.message.words[0];
                print_error (error, "aos_rpc_open: operation failed. %s\n", err_getstring (error));

                if (err_is_ok (error)) {
                    *fd = args.message.words[1];
                }
            }
        }
    }

    return error;
}

errval_t aos_rpc_readdir(struct aos_rpc *chan, char* path, struct aos_dirent **dir, size_t *elem_count)
{
    // Read list of files from the directory
    errval_t error = SYS_ERR_OK;

    if (chan && chan -> shared_buffer == NULL) {
        error = aos_rpc_setup_shared_buffer (chan, SHARED_BUFFER_DEFAULT_SIZE_BITS);
        debug_printf_quiet ("Initialized buffer: %s\n", err_getstring (error));
    }

    assert (chan -> shared_buffer != NULL);
    assert (chan -> shared_buffer_length != 0);

    if (chan && err_is_ok (error)) {
        // TODO: Gracefully handle this assertion.
        assert (strlen (path) <= chan->shared_buffer_length);

        // Put the string into the shared buffer.
        char* as_string_buffer = chan -> shared_buffer;
        strncpy (as_string_buffer, path, chan -> shared_buffer_length);

        struct lmp_message_args args;
        init_lmp_message_args (&args, &(chan->channel));

        // Set up the send arguments.
        args.message.words [0] = AOS_RPC_READ_DIR_NEW;
        args.message.words [1] = chan -> memory_descriptor;

        // Do the IPC call.
        error = aos_send_receive (&args, false);
        print_error (error, "aos_rpc_send_string: communication failed. %s\n", err_getstring (error));

        // Get the result.
        if (err_is_ok (error)) {
            error = args.message.words [0];
            if (err_is_ok (error)) {
                uint32_t count = args.message.words [1];
                void* buf = malloc (count * sizeof (struct aos_dirent));
                // TODO check buf.
                assert (buf != NULL);
                memcpy (buf, chan->shared_buffer, count * sizeof (struct aos_dirent));
                *dir = buf;
                *elem_count = count;
            }
        }
    } else {
        error = SYS_ERR_INVARGS_SYSCALL;
    }
    return error;
}

errval_t aos_rpc_read(struct aos_rpc *chan, int fd, size_t position, size_t size, void** buf, size_t *buflen)
{
    // Read previously opened file

    errval_t error = SYS_ERR_OK;

    if ((chan != NULL) && (buf != NULL) && (buflen != NULL)) {
        if (size <= MAX_PATH) {
            struct lmp_message_args  args   ;
            struct lmp_chan        * channel = &chan->channel;
    
            init_lmp_message_args(&args, channel);

            args.message.words [0] = AOS_RPC_READ_FILE   ;
            args.message.words [1] = disp_get_domain_id();
    
            args.message.words [2] = fd      ;
            args.message.words [3] = position;
            args.message.words [4] = size    ;

            error = aos_send_receive (&args, false);
            print_error (error, "aos_rpc_read: communication failed. %s\n", err_getstring (error));

            if (err_is_ok (error)) {
                error = args.message.words [0];
                print_error (error, "aos_rpc_read: operation failed. %s\n", err_getstring (error));

                if (err_is_ok (error)) {
                    *buflen = args.message.words[1];
                    *buf    = malloc(*buflen)      ;

                    memcpy(*buf, &(args.message.words[2]), *buflen);
                }
            }
        }
    }

    return error;
}

errval_t aos_rpc_close(struct aos_rpc *chan, int fd)
{
    // Close previously opened file

    errval_t error = -1;

    if (chan != NULL) {
        struct lmp_message_args  args   ;
        struct lmp_chan        * channel = &chan->channel;

        init_lmp_message_args(&args, channel);

        // Set up the send arguments.
        args.message.words [0] = AOS_RPC_CLOSE_FILE  ;
        args.message.words [1] = disp_get_domain_id();

        args.message.words [2] = fd;

        error = aos_send_receive (&args, false);
        print_error (error, "aos_rpc_close: communication failed. %s\n", err_getstring (error));

        if (err_is_ok (error)) {
            error = args.message.words [0];
            print_error (error, "aos_rpc_close: operation failed. %s\n", err_getstring (error));
        }
    }

    return error;
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



errval_t aos_find_service (uint32_t service, struct capref* endpoint)
{
    debug_printf_quiet ("aos_find_service...\n");
    errval_t error = SYS_ERR_OK;
    *endpoint = NULL_CAP;

    // Check arguments.
    if (service < 0 || service >= aos_service_guard) {
        error = SYS_ERR_INVARGS_SYSCALL; // TODO: is there a better error type?

    } else {
        // Provide a new set of message arguments.
        struct lmp_message_args my_args;
        init_lmp_message_args (&my_args, &(aos_rpc_get_init_channel()->channel));

        // Set up the arguments according to IPC convention.
        my_args.message.words [0] = AOS_ROUTE_FIND_SERVICE;
        my_args.message.words [1] = service;

        // Do the actual IPC call.
        error = aos_send_receive (&my_args, true);
        print_error (error, "aos_find_service:%s\n", err_getstring (error));

        // Set the result parameter.
        if (err_is_ok (error)) {
            error = my_args.message.words [0];
            if (err_is_ok (error)) {
                *endpoint = my_args.cap;
                error = lmp_chan_alloc_recv_slot (my_args.channel);
            }
        }
    }
    print_error (error, "aos_find_service:%s\n", err_getstring (error));
    return error;
}


errval_t aos_register_service (struct aos_rpc* rpc, uint32_t service)
{
    debug_printf_quiet ("aos_register_service...\n");
    errval_t error = SYS_ERR_OK;

    // Check arguments.
    if (service < 0 || service >= aos_service_guard) {
        error = SYS_ERR_INVARGS_SYSCALL; // TODO: is there a better error type?

    } else {
        // Provide a new set of message arguments.
        struct lmp_message_args my_args;
        init_lmp_message_args (&my_args, &(rpc->channel));

        // Set up the arguments according to IPC convention.
        my_args.message.words [0] = AOS_ROUTE_REGISTER_SERVICE;
        my_args.message.words [1] = service;

        // Do the actual IPC call.
        error = aos_send_receive (&my_args, false);
        print_error (error, "aos_find_service:%s\n", err_getstring (error));

        // Set the result parameter.
        if (err_is_ok (error)) {
            error = my_args.message.words [0];
        }
    }
    print_error (error, "aos_find_service:%s\n", err_getstring (error));
    return error;
}

errval_t aos_ping (struct aos_rpc* chan, uint32_t value)
{
    debug_printf_quiet ("aos_ping, channel %p...\n", chan);

    struct lmp_chan* channel = &chan->channel;
    // Provide a set of message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the arguments according to the convention.
    args.cap = channel -> local_cap;
    args.message.words [0] = AOS_PING;
    args.message.words [1] = value;

    // Do the actual IPC call.
    errval_t error = aos_send_receive (&args, false);

    // Check for errors.
    print_error (error, "aos_ping: %s\n", err_getstring (error));
    assert (args.message.words[0] == value); // maybe use something less deadly...
    return error;
}


errval_t aos_rpc_set_led (struct aos_rpc* rpc, bool new_state)
{
    debug_printf_quiet ("aos_rpc_set_led...\n");

    struct lmp_chan* channel = &rpc->channel;
    // Provide a set of message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the arguments according to the convention.
    args.message.words [0] = AOS_RPC_SET_LED;
    args.message.words [1] = new_state;

    // Do the actual IPC call.
    errval_t error = aos_send_receive (&args, false);

    // Check if there's an error.
    if (err_is_ok (error)) {
        error = args.message.words [0];
    }
    print_error (error, "aos_rpc_set_led: %s\n", err_getstring (error));
    return error;
}

/**
 * Set the specified domain as the foreground task, such that it is able
 * to receive input.
 */
errval_t aos_rpc_set_foreground (struct aos_rpc* rpc, domainid_t domain)
{
    debug_printf_quiet ("aos_rpc_set_foreground...\n");

    struct lmp_chan* channel = &rpc->channel;
    // Provide a set of message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the arguments according to the convention.
    args.message.words [0] = AOS_RPC_SET_FOREGROUND;
    args.message.words [1] = domain;

    // Do the actual IPC call.
    errval_t error = aos_send_receive (&args, false);

    // Check if there's an error.
    if (err_is_ok (error)) {
        error = args.message.words [0];
    }
    print_error (error, "aos_rpc_set_foreground: %s\n", err_getstring (error));
    return error;
}

/**
 * Kill the domain with the specified domain ID.
 */
errval_t aos_rpc_kill (struct aos_rpc* rpc, domainid_t domain)
{
    debug_printf_quiet ("aos_rpc_kill...\n");

    struct lmp_chan* channel = &rpc->channel;
    // Provide a set of message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the arguments according to the convention.
    args.message.words [0] = AOS_RPC_KILL;
    args.message.words [1] = domain;

    // Do the actual IPC call.
    errval_t error = aos_send_receive (&args, false);

    // Check if there's an error.
    if (err_is_ok (error)) {
        error = args.message.words [0];
    }
    print_error (error, "aos_rpc_kill: %s\n", err_getstring (error));
    return error;
}

/**
 * Send an exit request.
 * Current domain should be made unrunnable immediately.
 */
void aos_rpc_exit (struct aos_rpc* rpc)
{
    // TODO: maybe adapt to a more general aos_rpc_kill.
    struct lmp_chan* channel = &rpc->channel;
    // LMP_SEND_FLAGS_DEFAULT ~~ give up processor.
    lmp_chan_send2 (channel, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, AOS_RPC_KILL, disp_get_domain_id());
}


errval_t aos_rpc_wait_for_termination (struct aos_rpc* rpc, domainid_t domain)
{
    debug_printf_quiet ("aos_rpc_wait_for_termination...\n");

    struct lmp_chan* channel = &rpc->channel;
    // Provide a set of message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the arguments according to the convention.
    args.message.words [0] = AOS_RPC_WAIT_FOR_TERMINATION;
    args.message.words [1] = domain;

    // Do the actual IPC call.
    errval_t error = aos_send_receive (&args, false);

    // Check if there's an error.
    if (err_is_ok (error)) {
        error = args.message.words [0];
    }
    print_error (error, "aos_rpc_wait_for_termination: %s\n", err_getstring (error));
    return error;
}

/**
 * Set up a shared frame within channel "rpc".
 */
static errval_t aos_rpc_setup_shared_buffer (struct aos_rpc* rpc, uint8_t size_bits)
{
    debug_printf_quiet ("aos_rpc_setup_shared_buffer...\n");
    errval_t error = SYS_ERR_OK;
    void* buffer = NULL;

    // Allocate a frame.
    struct capref frame;
    error = frame_alloc (&frame, (1ul << size_bits), NULL);

    // Map it into our address space.
    if (err_is_ok (error)) {
        error = paging_map_frame (get_current_paging_state(), &buffer, (1ul << size_bits), frame, 0, 0);
    }
    // Ask the server to map it into his address space.
    if (err_is_ok (error)) {
        struct lmp_chan* channel = &rpc->channel;
        // Provide a set of message arguments.
        struct lmp_message_args args;
        init_lmp_message_args (&args, channel);

        // Set up the arguments according to the convention.
        args.cap = frame;
        args.message.words [0] = AOS_RPC_REGISTER_MEMORY;

        // Do the actual IPC call.
        error = aos_send_receive (&args, false);

        // Check if there's an error.
        if (err_is_ok (error)) {
            error = args.message.words [0];
            if (err_is_ok (error)) {
                rpc -> memory_descriptor = args.message.words [1];
                rpc -> shared_buffer = buffer;
                rpc -> shared_buffer_length = (1ul << size_bits);
            }
        }
    }

    if (err_is_fail (error)) {
        debug_printf ("Error: Buffer sharing failed!\n");
        // TODO: Clean up mapping and return frame.
    }

    return error;
}

errval_t aos_rpc_init(struct aos_rpc *rpc, struct capref receiver)
{
    // Initialize channel to receiver.
    // NOTE: We assume that the receiver is ready to accept messages, but doesn't
    // know our communication endpoint yet.

    struct lmp_chan* channel = &rpc->channel;
    lmp_chan_init (channel);
    //DBG: Uncomment if you really need it ==> debug_printf ("Initializing channel %p\n", channel);

    rpc -> memory_descriptor = 0;
    rpc -> shared_buffer_length = 0;
    rpc -> shared_buffer = NULL;

    // Provide a new set of message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Create a new local endpoint for this channel.
    errval_t error = lmp_chan_accept (channel, DEFAULT_LMP_BUF_WORDS, receiver);
    print_error (error, "aos_rpc_init: created local endpoint. %s\n", err_getstring (error));

    if (err_is_ok (error)) {
        // Reserve a slot for incoming messages.
        error = lmp_chan_alloc_recv_slot (channel);
        print_error (error, "aos_rpc_init: allocating receive slot. %s\n", err_getstring (error));
    }

    if (err_is_ok (error)) {
        error = aos_connection_init (channel);
    }

    if (err_is_fail (error)) {
        lmp_chan_destroy (channel);
        debug_printf ("Error in aos_rpc_init: %s\n", err_getstring (error));
    }
    return error;
}


errval_t aos_connection_init (struct lmp_chan* channel)
{
    // Send our endpoint to the other side.
    errval_t error = SYS_ERR_OK;

    // Initialize storage for message arguments.
    struct lmp_message_args args;
    init_lmp_message_args (&args, channel);

    // Set up the send arguments.
    args.message.words [0] = AOS_RPC_CONNECTION_INIT;
    args.cap = channel -> local_cap;

    // Do the IPC call.
    error = aos_send_receive (&args, false);
    print_error (error, "aos_connection_init: communication failed. %s\n", err_getstring (error));

    // Get the result.
    if (err_is_ok (error)) {
        error = args.message.words [0];
        print_error (error, "aos_connection_init: operation failed. %s\n", err_getstring (error));
    }
    return error;
}

