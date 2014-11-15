/**
 * \file
 * \brief init process.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include "ipc_protocol.h"
#include "init.h"
#include <stdlib.h>
#include <string.h>
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>

#include <barrelfish/aos_rpc.h>

// From Milestone 0...
#define UART_BASE 0x48020000
#define UART_SIZE 0x1000
#define UART_SIZE_BITS 12

#include <mm/mm.h>

#define MEMEATER_NAME "armv7/sbin/memeater"

struct bootinfo *bi;
static coreid_t my_core_id;

// Storage for incoming strings.
static uint32_t example_index;
static char*    example_str  ;
static uint32_t example_size ;

// static struct capref service_uart;

//Keeps track of registered services.
struct lmp_chan* services [aos_service_guard];

// Keeps track of FIND_SERVICE requests.
// TODO: use a system that supports removal as well.
static struct lmp_chan* find_request [100];
static int find_request_index = 0;

/**
 * Initialize some data structures.
 */
static void init_data_structures (void) {
    for (int i=0; i<aos_service_guard; i++) {
        services [i] = NULL;
    }

    example_index =          0 ;
    example_size  =        128 ;
    example_str   = malloc(128);
}

/**
 * A receive handler for init.
 */
static void recv_handler (void *arg)
{
//     debug_printf ("Handling LMP message, channel %p...\n", arg);
    errval_t err = SYS_ERR_OK;
    struct lmp_chan* lc = (struct lmp_chan*) arg;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;

    // Retrieve capability and arguments.
    err = lmp_chan_recv(lc, &msg, &cap);

    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(recv_handler, arg));
    }

    uint32_t type = msg.words [0];

    switch (type)
    {
        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...

        case AOS_PING:
            // Send a response to the ping request.
            lmp_ep_send1 (cap, 0, NULL_CAP, msg.words[1]);

            // Delete capability and reuse slot.
            err = cap_delete (cap);
            lmp_chan_set_recv_slot (lc, cap);
            debug_printf ("Handled AOS_PING: %s\n", err_getstring (err));
            break;
        case AOS_RPC_GET_RAM_CAP:;
            size_t bits = msg.words [1];
            struct capref ram;
            errval_t error = ram_alloc (&ram, bits);

            lmp_chan_send2 (lc, 0, ram, error, bits);

            //TODO: do we need to destroy ram capability here?
//          error = cap_destroy (ram);

            debug_printf ("Handled AOS_RPC_GET_RAM_CAP: %s\n", err_getstring (error));
            break;

        case AOS_RPC_SEND_STRING:;
            // TODO: maybe store the string somewhere else?

            // Enlarge receive buffer if necessary.
            uint32_t char_count = (LMP_MSG_LENGTH - 1) * sizeof (uint32_t);

            if (example_index + char_count + 1 >= example_size) {
                example_str = realloc(example_str, example_size * 2);
                memset(&example_str[example_size], 0, example_size);
                example_size *= 2;
            }

            memcpy(&example_str[example_index], &msg.words[1], char_count);
            example_index += char_count;

            // Append a null character to safely print the string.
            example_str [example_index] = '\0';
            debug_printf ("Handled AOS_RPC_SEND_STRING with string: %s\n", example_str + example_index - char_count);
            if (example_str [example_index - 1] == '\0') {
                debug_printf ("Received last chunk. Contents: \n");
                printf("%s\n", example_str);
            }
            break;
        case AOS_ROUTE_REGISTER_SERVICE:;
            // TODO: error handling
            assert (capref_is_null (cap));
            services [msg.words [1]] = lc;
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_SERIAL_PUTCHAR:;
//             debug_printf ("Got AOS_RPC_SERIAL_PUTCHAR\n");
            char output_character = msg.words [1];
            uart_putchar (output_character);
            break;
        case AOS_RPC_SERIAL_GETCHAR:;
//             debug_printf ("Got AOS_RPC_SERIAL_GETCHAR\n");
            char input_character = uart_getchar ();
            lmp_chan_send2 (lc, 0, NULL_CAP, SYS_ERR_OK, input_character);
            break;
        case AOS_RPC_CONNECTION_INIT:;
            debug_printf ("Got AOS_RPC_CONNECTION_INIT\n");
            lc->remote_cap = cap;
            err = lmp_chan_alloc_recv_slot (lc); // TODO: better error handling
            err = lmp_chan_send1 (lc, 0, NULL_CAP, err);
            break;

        case AOS_ROUTE_FIND_SERVICE:;
            debug_printf ("Got AOS_ROUTE_FIND_SERVICE\n");
            // generate new ID
            uint32_t id = find_request_index;
            find_request_index++;
            // store current channel at ID
            find_request [id] = lc;
            // find correct server channel
            uint32_t requested_service = msg.words [1];
            struct lmp_chan* serv = services [requested_service];
            // generate AOS_ROUTE_REQUEST_EP request with ID.
            lmp_chan_send2 (serv, 0, NULL_CAP, AOS_ROUTE_REQUEST_EP, id);
            break;
        case AOS_ROUTE_REQUEST_EP:;
            // NOTE: implemented by all servers, and init usually doesn't handle this.
            debug_printf ("Got AOS_ROUTE_REQUEST_EP\n");
            // Extract request ID
            // Create a new endpoint with accept (using NULL_CAP)
            // Generate answer with AOS_ROUTE_DELIVER_EP, error value and request ID
            break;
        case AOS_ROUTE_DELIVER_EP:;
            debug_printf ("Got AOS_ROUTE_DELIVER_EP\n");
            // get error value and ID from message
            errval_t error_ret = msg.words [1];
            id = msg.words [2];
            // lookup receiver channel at ID
            struct lmp_chan* recv = find_request [id]; // TODO: delete id
            // generate response with cap and error value
            lmp_chan_send1 (recv, 0, cap, error_ret);
            // Delete cap and reuse slot
            err = cap_delete (cap);
            lmp_chan_set_recv_slot (lc, cap);
            lmp_chan_alloc_recv_slot (lc);
            break;
        default:
            debug_printf ("Got default value\n");
            if (! capref_is_null (cap)) {
                cap_delete (cap);
                lmp_chan_set_recv_slot (lc, cap);
            }
    }
    lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(recv_handler, arg));
}

/**
 * Set up the user-level data structures for the
 * kernel-created endpoint for init.
 */
__attribute__((unused))
static errval_t initep_setup (void)
{
    errval_t err = SYS_ERR_OK;

    // Allocate an LMP channel and do basic initializaton.
    // TODO; maybe make the allocated channel structure available somewhere.
    struct lmp_chan* channel = malloc (sizeof (struct lmp_chan));

    if (channel != NULL) {
        lmp_chan_init (channel);

        // make local endpoint available -- this was minted in the kernel in a way
        // such that the buffer is directly after the dispatcher struct and the
        // buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel
        // sentinel word).

        // NOTE: lmp_endpoint_setup automatically adds the dispatcher offset.
        // Thus the offset of the first endpoint structure is zero.

        struct lmp_endpoint* endpoint; // Structure to be filled in.
        err = lmp_endpoint_setup (0, DEFAULT_LMP_BUF_WORDS, &endpoint);

        if (err_is_fail(err)) {
            debug_printf ("ERROR: On endpoint setup.\n");

        } else {
            // Update the channel with the newly created endpoint.
            channel->endpoint  = endpoint;
            // The channel needs to know about the (kernel-created) capability to receive objects.
            channel->local_cap = cap_initep;

            // Allocate a slot for incoming capabilities.
            err = lmp_chan_alloc_recv_slot(channel);
            if (err_is_fail(err)) {
                debug_printf ("ERROR: On allocation of recv slot.\n");

            } else {
                // Register a receive handler.
                err = lmp_chan_register_recv(channel, get_default_waitset(), MKCLOSURE(recv_handler, channel));
                if (err_is_fail(err))
                {
                    debug_printf ("ERROR: On channel registration.\n");
                }
            }
        }
    } else {
        // malloc returned NULL...
        err = LIB_ERR_MALLOC_FAIL;
    }
    return err;
} //*/

int main(int argc, char *argv[])
{
    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);

    debug_printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

    debug_printf("FIRSTEP_OFFSET should be %zu + %zu\n",
            get_dispatcher_size(), offsetof(struct lmp_endpoint, k));

    // First argument contains the bootinfo location
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);

    // setup memory serving
    err = initialize_ram_alloc();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init local ram allocator");
        abort();
    }
    debug_printf("initialized local ram alloc\n");

    // setup memory serving
    err = initialize_mem_serv();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init memory server module");
        abort();
    }

    // Create our endpoint to self
    err = cap_retype(cap_selfep, cap_dispatcher, ObjType_EndPoint, 0);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to create our endpoint to self");
        // bail out, there isn?t much we can do without a self endpoint.
        abort();
    }

    // Load memeater
    //TODO: Probably we shouldn't use spawn_load_with_bootinfo.
    debug_printf("Spawning memeater (%s)...\n", MEMEATER_NAME);
    struct spawninfo memeater_spawninfo;
    err = spawn_load_with_bootinfo(&memeater_spawninfo, bi, MEMEATER_NAME, my_core_id);
//    if (err_is_fail(err)) {
         debug_printf ("Error: %s\n", err_getstring (err));
//    }
//     // Initialize memeater
//     err = initialize_mem_serv(&memeater_spawninfo);
//     if (err_is_fail(err)) {
//          debug_printf ("Error: %s\n", err_getstring (err));
//     }


    // Allocate an incoming LMP endpoint for memeater
    struct lmp_chan memeater_chan;
    lmp_chan_init (&memeater_chan);
    err = lmp_chan_accept(&memeater_chan, DEFAULT_LMP_BUF_WORDS, NULL_CAP);
    err = lmp_chan_alloc_recv_slot(&memeater_chan);
    err = lmp_chan_register_recv(&memeater_chan, get_default_waitset(), MKCLOSURE (recv_handler, &memeater_chan));


    // Give endpoint to memeater
    struct capref memeater_remote_cap;
    memeater_remote_cap.cnode = memeater_spawninfo.taskcn;
    memeater_remote_cap.slot  = TASKCN_SLOT_INITEP;
    err = cap_copy(memeater_remote_cap, memeater_chan.local_cap);

    //Make mem_serv runnable
    err = spawn_run(&memeater_spawninfo);

    err = spawn_free(&memeater_spawninfo);

    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    err = initialize_device_frame_server (cap_io);

    // Initialize the serial driver.
    if (err_is_ok (err)) {
//         err = spawn_serial_driver ();
        init_uart_driver ();
    }

    if (err_is_fail (err)) {
        debug_printf ("Failed to initialize: %s\n", err_getstring (err));
    }
    debug_printf("initialized dev memory management\n");


    // TODO (milestone 3) STEP 2:

    // Set up some data structures for RPC services.
    init_data_structures ();

    // Set up init's kernel-created endpoint.
//     err = initep_setup ();
//     if (err_is_fail (err)) {
//         debug_printf ("FATAL: Could not set up init endpoint\n");
//         abort();
//     }

    // Spawn a user-level thread.
    // NOTE: This thread is linked to a routine in memeater that tests this thread.
//    err = spawn_test_thread(recv_handler);

    // Go into messaging main loop.
    while (true) {
        err = event_dispatch (get_default_waitset());// TODO: error handling
        if (err_is_fail (err)) {
            debug_printf ("Handling LMP message: %s\n", err_getstring (err));
        }
    }

    debug_printf ("init returned.");
    return EXIT_SUCCESS;

}
