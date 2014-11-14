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

#include <stdarg.h>

#define QUIET // Suppress some output.

#ifdef QUIET
    #define debug_printf_quiet(x, ...)
#else
    #define debug_printf_quiet debug_printf
#endif

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

/// The first LMP channel to init which every domain gets.
// NOTE: We can't use malloc here, because the initial ram allocator
// only allows frames of one page size, and our paging code doesn't allow this.
static struct aos_rpc init_channel;

struct aos_rpc* aos_rpc_get_init_channel (void)
{
    return &init_channel;
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


errval_t aos_rpc_send_string(struct aos_rpc *chan, const char *string)
{
    // NOTE: Splitted due to size restriction in debug_printf
    debug_printf ("aos_rpc_send_string(%u, ", chan);
    printf ("%s)\n", string);

    errval_t error = SYS_ERR_INVARGS_SYSCALL;

    if (chan != NULL) {
        bool finished = false;
        int  indx = 0;

        for (; finished == false ;) {
            uint32_t buf[LMP_MSG_LENGTH] = {AOS_RPC_SEND_STRING,0,0,0,0,0,0,0,0};
	
            for (int i = 1; (i < (sizeof(buf)/sizeof(buf[0]))) && (finished == false); i++) {
                for (int j = 0; j < 4; j ++) {
                    buf[i] |= (uint32_t)(string[indx]) << (8 * j);

                    if (string[indx] == '\0' && !finished) {
                        finished = true;
                        debug_printf_quiet ("last_message\n");
                    }

                    indx++;
                }
            }

            error = lmp_chan_send9(&chan->channel, LMP_FLAG_SYNC | LMP_FLAG_YIELD, NULL_CAP, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
            debug_printf_quiet ("transferred 32 bytes: %s\n", err_getstring (error));

            if (err_is_fail (error)) {
                finished = true;

                print_error(error, "aos_rpc_send_string (0x%X) experienced error: %s\n", chan, err_getstring (error));
            }
        }
        
        print_error (error, "aos_rpc_send_string (channel 0x%X): %s\n", err_getstring (error));
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
    // TODO (milestone 4): implement functionality to request device memory
    // capability.
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

    // Do the IPC call.
    error = aos_send_receive (&args, true);
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


errval_t aos_ping (struct aos_rpc* chan, uint32_t value)
{
    debug_printf_quiet ("aos_ping, channel %p...\n", channel);

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
    print_error (error, "aos_ping: %s\n");
    assert (args.message.words[0] == value); // maybe use something less deadly...
    return error;
}


errval_t aos_rpc_init(struct aos_rpc *rpc, struct capref receiver)
{
    // Initialize channel to receiver.
    // NOTE: We assume that the receiver is ready to accept messages, but doesn't
    // know our communication endpoint yet.

    struct lmp_chan* channel = &rpc->channel;
    lmp_chan_init (channel);
    debug_printf ("Initializing channel %p\n", channel);

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
    error = aos_send_receive (&args, true);
    print_error (error, "aos_connection_init: communication failed. %s\n", err_getstring (error));

    // Get the result.
    if (err_is_ok (error)) {
        error = args.message.words [0];
        print_error (error, "aos_connection_init: operation failed. %s\n", err_getstring (error));
    }
    return error;
}