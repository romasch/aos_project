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
static struct capref services [aos_service_guard];

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
        services [i] = NULL_CAP;
    }

    // Currently we're using init as the ram server.
    services [aos_service_ram] = cap_initep;
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

        case INIT_FIND_SERVICE:;
            // Get the endpoint capability to a service.
            // TODO: check validity.
            uint32_t requested_service = msg.words [1];

            // debug_printf ("Requested service: %u\n", requested_service);

            // TODO<-done: Find out why lookup for services[0] == cap_initep fails.

            // TODO: Apparently an endpoint can only connect to exactly one other endpoint.
            // Therefore we need to change this function: init has to request a new endpoint
            // at the service provider and later send it back to first domain.

            // This can be done at a later point however... For now we could just implement
            // all services in init and handle them in this global request handler.
            lmp_ep_send0 (cap, 0, services [requested_service]);

            // Delete capability and reuse slot.
            cap_delete (cap);
            lmp_chan_set_recv_slot (lc, cap);
            debug_printf ("Handled INIT_FIND_SERVICE\n");
            break;

        case AOS_RPC_CONNECT:;
            // Create a new channel.
            struct lmp_chan* new_channel = malloc (sizeof (struct lmp_chan));// TODO: error handling
            lmp_chan_init (new_channel);

            // Set up channel for receiving.
            err = lmp_chan_accept (new_channel, DEFAULT_LMP_BUF_WORDS, cap);
            err = lmp_chan_alloc_recv_slot (new_channel);

            // Register a receive handler for the new channel.
            // TODO: maybe also use a different receive handler for connected clients.
            err = lmp_chan_register_recv (new_channel, get_default_waitset(), MKCLOSURE (recv_handler, new_channel));

            // Need to allocate a new slot for the main channel.
            err = lmp_chan_alloc_recv_slot (lc);

            err = lmp_chan_send2 (new_channel, 0, new_channel -> local_cap, 0, msg.words[1]);
            debug_printf ("Handled AOS_RPC_CONNECT\n");
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

static int test_thread (void* arg)
{
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

    debug_printf ("test_thread: end of thread reached.\n");
    return 0;
}

static errval_t create_channel(struct capref* cap, void (*handler)(void*))
{
    struct lmp_chan* channel = malloc (sizeof (struct lmp_chan));

    errval_t err = -1;

    if (channel != NULL) {
        struct lmp_endpoint* endpoint;

        lmp_chan_init (channel);
        
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
    }

    return err;
}

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

    // Allocate an LMP channel and do basic initializaton.
    err = create_channel(&cap_initep, recv_handler);
    if (err_is_fail(err)) {
        debug_printf("ERROR! Channel for INIT wasn't created\n");
    }
    /*err = create_channel(&cap_uartep, recv_handler);
    if (err_is_fail(err)) {
        debug_printf("ERROR! Channel for UART wasn't created\n");
    }
    err = create_channel(&cap_termep, recv_handler);
    if (err_is_fail(err)) {
        debug_printf("ERROR! Channel for TERM wasn't created\n");
    }*/

    if (err_is_ok (err)) {
        err = spawn_serial_driver ();
    }

    if (err_is_fail (err)) {
        debug_printf ("Failed to initialize: %s\n", err_getstring (err));
    }

    debug_printf("initialized dev memory management\n");

    // TODO (milestone 3) STEP 2:

    // Set up the basic service registration mechanism.
    init_services ();

    example_index =          0 ;
    example_size  =        128 ;
    example_str   = malloc(128);

    // Test thread creation.
    thread_create (test_thread, NULL);

    // Go into messaging main loop.
    while (true) {
        err = event_dispatch (get_default_waitset());// TODO: error handling
        if (err_is_fail (err)) {
            debug_printf ("Handling LMP message: %s\n", err_getstring (err));
        }
    }

//     for (;;) sys_yield(CPTR_NULL);
    debug_printf ("init returned.");
    return EXIT_SUCCESS;

}
