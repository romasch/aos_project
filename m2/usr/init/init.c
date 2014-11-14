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

struct bootinfo *bi;
static coreid_t my_core_id;

static uint32_t example_index;
static char*    example_str  ;
static uint32_t example_size ;

static struct capref service_uart;

/**
 * Keeps track of registered services.
 */
static struct lmp_chan* services [aos_service_guard];

// TODO: use a system that supports removal as well.
static struct lmp_chan* find_request [100];
static int find_request_index = 0;

__attribute__((unused))
static errval_t spawn_serial_driver (void)
{
    // TODO: This function just shows that serial driver works.
    // We should actually create a thread and an endpoint, register ourselves with init,
    // and listen for aos_rpc_connect, aos_rpc_serial_getchar and *putchar requests.

    errval_t error = SYS_ERR_OK;

    thread_create(uart_driver_thread, NULL);
    thread_create(   terminal_thread, NULL);

    return error;
}

/**
 * Initialize the service lookup facility.
 */
static void init_services (void) {

    for (int i=0; i<aos_service_guard; i++) {
        services [i] = NULL;
    }

    // Currently we're using init as the ram server.
//     services [aos_service_ram] = cap_initep;
}

/**
 * A receive handler for init.
 */
static void recv_handler (void *arg)
{
//    debug_printf ("Handling LMP message...\n");
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
        case AOS_REGISTER_SERVICE:;
            errval_t status = STATUS_ALREADY_EXIST;

            debug_printf ("Handled AOS_REGISTER_SERVICE ot type 0x%x\n", msg.words [1]);

            // Requested service must be unfilled and provider must pass a capp for communication
            if (!capref_is_null(cap)) {
                void* buf;

                debug_printf ("Handled AOS_REGISTER_SERVICE of type 0x%x with cap 1\n", msg.words [1]);
                if (capref_is_null(service_uart)) {
                    if (msg.words[1] == SERVICE_UART_DRIVER) {
                        debug_printf ("Handled AOS_REGISTER_SERVICE of type 0x%x with cap 2\n", msg.words [1]);

                        struct capref uart_cap;

                        status = allocate_device_frame (UART_BASE, UART_SIZE_BITS, &uart_cap);
                        if (!err_is_fail (status)) {
                            int flags = KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE | KPI_PAGING_FLAGS_NOCACHE;

                            status = paging_map_frame_attr (get_current_paging_state(), &buf, UART_SIZE, uart_cap, flags, NULL, NULL);
                            if (!err_is_fail (status)) {
                                debug_printf ("Handled AOS_REGISTER_SERVICE of type 0x%x with cap 2\n", msg.words [1]);
                                service_uart = cap           ;
                                status       = STATUS_SUCCESS;
                            }
                        }
                    }
                }

                lmp_ep_send3(cap, 0, NULL_CAP, AOS_REGISTRATION_COMPETE, status, (uint32_t)buf);

                lmp_ep_send1(cap, 0, NULL_CAP, UART_CONNECT   );
                lmp_ep_send1(cap, 0, NULL_CAP, UART_RECV_BYTE );
                lmp_ep_send2(cap, 0, NULL_CAP, UART_SEND_BYTE , 'x');
                lmp_ep_send1(cap, 0, NULL_CAP, UART_DISCONNECT);
            }
            break;

        case AOS_GET_SERVICE:;

            debug_printf ("Handled AOS_GET_SERVICE ot type 0x%x\n", msg.words [1]);

            if (!capref_is_null(cap)) {
                if (msg.words[1] == SERVICE_UART_DRIVER) {
                    lmp_ep_send1(cap, 0, service_uart, AOS_GET_SERVICE);
                }
            }

            break;


        case AOS_RPC_SERIAL_PUTCHAR:;
//             debug_printf ("Got AOS_RPC_SERIAL_PUTCHAR\n");
            char output_character = msg.words [1];
            uart_putchar (output_character);
//            uart_putchar ('\n');
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
            lmp_chan_send1 (lc, 0, NULL_CAP, err);
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
            debug_printf ("Got AOS_ROUTE_REQUEST_EP\n");
            // Extract request ID
            // Create a new endpoint with accept (using NULL_CAP)
            // Generate answer with AOS_ROUTE_DELIVER_EP, error value and request ID
            // NOTE: implemented by all servers, and init usually doesn't handle this.
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
        case UART_RECV_BYTE:;
            debug_printf ("Handled UART_RECV_BYTE received '%c'\n", msg.words [1]);
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

static void test_thread_handler (void *arg)
{
    debug_printf ("LMP message in thread...\n");
    errval_t err = SYS_ERR_OK;
    struct lmp_chan* lc = (struct lmp_chan*) arg;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;

    // Retrieve capability and arguments.
    err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(test_thread_handler, arg));
    }
    uint32_t type = msg.words [0];
    switch (type)
    {
        case AOS_PING:
            debug_printf ("Got AOS_PING\n");
            // Send a response to the ping request.
            lmp_ep_send1 (cap, 0, NULL_CAP, msg.words[1]);

            // Delete capability and reuse slot.
            err = cap_delete (cap);
            lmp_chan_set_recv_slot (lc, cap);
            debug_printf ("Handled AOS_PING: %s\n", err_getstring (err));
            break;
        case AOS_ROUTE_REQUEST_EP:;
            debug_printf ("Got AOS_ROUTE_REQUEST_EP\n");

            // Extract request ID
            uint32_t id = msg.words [1];
            uint32_t error_ret = SYS_ERR_OK;

            // Create a new endpoint.
            struct lmp_chan* channel = malloc (sizeof (struct lmp_chan));
            lmp_chan_init (channel);

            // Initialize endpoint to receive messages.
            error_ret = lmp_chan_accept (channel, DEFAULT_LMP_BUF_WORDS, NULL_CAP);
            error_ret = lmp_chan_alloc_recv_slot (channel);
            error_ret = lmp_chan_register_recv (channel, get_default_waitset (), MKCLOSURE (test_thread_handler, channel));// TODO: error handling

            // Generate answer with AOS_ROUTE_DELIVER_EP, error value and request ID
            lmp_chan_send3 (lc,0, channel->local_cap, AOS_ROUTE_DELIVER_EP, error_ret, id);

            break;
        case AOS_RPC_CONNECTION_INIT:;
            debug_printf ("Got AOS_RPC_CONNECTION_INIT\n");
            lc->remote_cap = cap;
            err = lmp_chan_alloc_recv_slot (lc); // TODO: better error handling
            lmp_chan_send1 (lc, 0, NULL_CAP, err);
            break;
        default:
            debug_printf ("Got default value\n");
            if (! capref_is_null (cap)) {
                cap_delete (cap);
                lmp_chan_set_recv_slot (lc, cap);
            }
    }
    lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(test_thread_handler, arg));
}

static int test_thread (void* arg)
{
    errval_t error;
    // A small test for our separate page fault handler.
    debug_printf ("test_thread: new thread created...\n");
    size_t bufsize = 4*1024*1024;
    char* buf = malloc (bufsize);
    debug_printf ("test_thread: buffer allocated.\n");
    for (int i=0; i<bufsize; i++) {
        buf [i] = i%256;
    }
    debug_printf ("test_thread: buffer filled.\n");
    free (buf);

    struct capref* init_capability = arg;

    // Manually create the connection between init's main thread this test thread.
    struct lmp_chan* thread_channel = malloc (sizeof (struct lmp_chan));
    lmp_chan_init (thread_channel);

    error = lmp_chan_accept (thread_channel, DEFAULT_LMP_BUF_WORDS, *init_capability);
    error = lmp_chan_alloc_recv_slot (thread_channel);
    lmp_chan_register_recv (thread_channel, get_default_waitset (), MKCLOSURE (test_thread_handler, thread_channel));// TODO: error handling

    // We do it the lazy way now...
    services [aos_service_test] -> remote_cap = thread_channel->local_cap;

    while (true) {
        event_dispatch (get_default_waitset());
    }

    debug_printf ("aos_rpc_init: created local endpoint. %s\n", err_getstring (error));
    debug_printf ("test_thread: end of thread reached.\n");
    return 0;
}




// NOTE: can only be used for cap_initep...
/*
static errval_t create_channel(struct capref* cap, void (*handler)(void*))
{
    errval_t err = SYS_ERR_OK;

    // Allocate an LMP channel and do basic initializaton.
    struct lmp_chan* channel = malloc (sizeof (struct lmp_chan));

    if (channel != NULL) {
        lmp_chan_init (channel);


        struct lmp_endpoint* endpoint; // Structure to be filled in.

        err = lmp_endpoint_setup (0, DEFAULT_LMP_BUF_WORDS, &endpoint);
        if (err_is_fail(err)) {
            debug_printf ("ERROR: On endpoint setup.\n");
        } else {
            channel->endpoint  = endpoint;
            channel->local_cap = *cap    ;

            err = lmp_chan_alloc_recv_slot(channel);
            if (err_is_fail(err)) {
                debug_printf ("ERROR: On allocation of recv slot.\n");
            } else {
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
}//*/

int main(int argc, char *argv[])
{
    errval_t err;

    service_uart = NULL_CAP;

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

    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    err = initialize_device_frame_server (cap_io);


    // TODO (milestone 3) STEP 2:

    /*
    // Allocate an LMP channel and do basic initializaton.
    err = create_channel(&cap_initep, recv_handler);
    if (err_is_fail(err)) {
        debug_printf("ERROR! Channel for INIT wasn't created\n");
    }
    err = create_channel(&cap_uartep, recv_handler);
    if (err_is_fail(err)) {
        debug_printf("ERROR! Channel for UART wasn't created\n");
    }
    err = create_channel(&cap_termep, recv_handler);
    if (err_is_fail(err)) {
        debug_printf("ERROR! Channel for TERM wasn't created\n");
    }*/

    if (err_is_ok (err)) {
//         err = spawn_serial_driver ();
        init_uart_driver ();
    }

    if (err_is_fail (err)) {
        debug_printf ("Failed to initialize: %s\n", err_getstring (err));
    }

    debug_printf("initialized dev memory management\n");


    // Set up the basic service registration mechanism.
    init_services ();
#if 1
    // Get the default waitset.
    struct waitset* default_ws = get_default_waitset ();

    struct lmp_chan* my_channel = malloc (sizeof (struct lmp_chan));// TODO: error handling
    lmp_chan_init (my_channel);

    /* make local endpoint available -- this was minted in the kernel in a way
     * such that the buffer is directly after the dispatcher struct and the
     * buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel
     * sentinel word).
     *
     * NOTE: lmp_endpoint_setup automatically adds the dispatcher offset.
     * Thus the offset of the first endpoint structure is zero.
     */
    struct lmp_endpoint* my_endpoint; // Structure to be filled in.
    err = lmp_endpoint_setup (0, DEFAULT_LMP_BUF_WORDS, &my_endpoint);// TODO: error handling

    // Update the channel with the newly created endpoint.
    my_channel -> endpoint = my_endpoint;

    // The channel needs to know about the (kernel-created) capability to receive objects.
    my_channel -> local_cap = cap_initep;

    // Allocate a slot for incoming capabilities.
    err = lmp_chan_alloc_recv_slot (my_channel);// TODO: error handling

    // Register a receive handler.
    lmp_chan_register_recv (my_channel, default_ws, MKCLOSURE (recv_handler, my_channel));// TODO: error handling
#endif
    example_index =          0 ;
    example_size  =        128 ;
    example_str   = malloc(128);


    // TEST: set up a new channel for a user-level thread to communicate with main thread.
    struct lmp_chan* init_channel = malloc (sizeof (struct lmp_chan));
    lmp_chan_init (init_channel);
    err = lmp_chan_accept (init_channel, DEFAULT_LMP_BUF_WORDS, NULL_CAP);
    err = lmp_chan_alloc_recv_slot (init_channel);
    lmp_chan_register_recv (init_channel, default_ws, MKCLOSURE (recv_handler, init_channel));// TODO: error handling
    services [aos_service_test] = init_channel;
    // Test thread creation.
    thread_create (test_thread, &init_channel->local_cap);

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
