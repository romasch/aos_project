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

#define BINARY_PREFIX "armv7/sbin/"

#define COUNT_OF(x) (sizeof(x) / sizeof(x[0]))

// Entry of the process database maintained by init.
struct ddb_entry
{
           char     name[MAX_PROCESS_NAME_LENGTH + 1];
    struct lmp_chan channel                          ;
};

static struct ddb_entry ddb[32];

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
static void init_data_structures (void) 
{
    for (int i=0; i<COUNT_OF(ddb); i++) {
        ddb[i].name[0] = '\0';
    }    

    for (int i=0; i<aos_service_guard; i++) {
        services [i] = NULL;
    }

    example_index =          0 ;
    example_size  =        128 ;
    example_str   = malloc(128);
}

static void recv_handler (void *arg);

/**
 * Spawn a new domain with an initial channel to init.
 *
 * \arg domain_name: The name of the new process. NOTE: Name should come
 * without path prefix, i.e. just pass "memeater" instead of "armv7/sbin/memeater".
 *
 * \arg ret_channel: Structure to be filled in with new channel.
 */
static errval_t spawn_with_channel (char* domain_name, struct lmp_chan* ret_channel)
{
    debug_printf("Spawning new domain: %s...\n", domain_name);
    errval_t error = SYS_ERR_OK;

    // Struct to keep track of new domains cspace, vspace, etc...
    struct spawninfo new_domain;
    memset (&new_domain, 0, sizeof (struct spawninfo));

    // Concatenate the name.
    char prefixed_name [256]; // TODO: prevent buffer overflow attacks...
    strcpy (prefixed_name, BINARY_PREFIX);
    strcat (prefixed_name, domain_name);

    //TODO: Probably we shouldn't use spawn_load_with_bootinfo.
    // Try to find a better suited function
    error = spawn_load_with_bootinfo (&new_domain, bi, prefixed_name, my_core_id);


    // Initialize memeater.
    // NOTE: In upstream barrelfish, this function copies around some more capabilities.
    // It doesn't seem to be necessary though...
    // error = initialize_mem_serv(&new_domain);

    if (err_is_ok (error)) {

        // Set up an LMP endpoint in init for the new domain.
        lmp_chan_init (ret_channel);
        error = lmp_chan_accept(ret_channel, DEFAULT_LMP_BUF_WORDS, NULL_CAP);

        // Register for incoming requests with the default handler.
        if (err_is_ok (error)) {
            error = lmp_chan_register_recv(ret_channel, get_default_waitset(), MKCLOSURE (recv_handler, ret_channel));
        }
        // Reserve a slot for incoming capabilities.
        if (err_is_ok (error)) {
            error = lmp_chan_alloc_recv_slot(ret_channel);
        }
        // Clean up in case of errors.
        if (err_is_fail (error)) {
            lmp_chan_destroy (ret_channel);
        }
    }

    // Copy the endpoint of the new channel into the new domain.
    if (err_is_ok (error)) {
        struct capref init_remote_cap;
        init_remote_cap.cnode = new_domain.taskcn;
        init_remote_cap.slot  = TASKCN_SLOT_INITEP;

        error = cap_copy(init_remote_cap, ret_channel->local_cap);
    }

    // Make the domain runnable
    if (err_is_ok (error)) {
        error = spawn_run(&new_domain);
    }

    // Clean up structures in current domain.
    error = spawn_free(&new_domain);

    return error;
}

/*static bool str_to_args(const char* string, uint32_t* args, size_t args_length, int* indx, bool finished)
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
}*/

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
            // debug_printf ("Handled AOS_RPC_SEND_STRING with string: %s\n", example_str + example_index - char_count);
            if (example_str [example_index - 1] == '\0') {
                debug_printf ("Received last chunk. Contents: \n");
                printf("%s\n", example_str);
            }
            break;
        case AOS_ROUTE_REGISTER_SERVICE:;
            debug_printf ("Got AOS_ROUTE_REGISTER_SERVICE 0x%x\n", msg.words [1]);
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
            if (serv != NULL) {
                lmp_chan_send2 (serv, 0, NULL_CAP, AOS_ROUTE_REQUEST_EP, id);
            } else {
                // Service is unknown. Send error back.
                lmp_chan_send1 (lc, 0, NULL_CAP, -1);  
            }
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
        case AOS_RPC_SPAWN_PROCESS:;
            int       idx    = -1                   ;
            char    * name   = (char*)&msg.words [1];
            errval_t  status = -1                   ;

            //DBG: debug_printf ("Got AOS_RPC_SPAWN_PROCESS <- %s\n", name);

            // Lookup of free process slot in process database
            for (int i = 0; (i < COUNT_OF(ddb)) && (idx == -1); i++) {
                if (ddb[i].name[0] == '\0') {
                    idx = i;
                }
            }
            
            if (idx != -1) {
                status = spawn_with_channel(name, &ddb[idx].channel);
                if (!err_is_fail(err)) {
                    strcpy(ddb[idx].name, name);
                }
            }    
            
            // Send reply back to the client
            lmp_chan_send2(lc, 0, NULL_CAP, status, idx);            
            break;
        case AOS_RPC_GET_PROCESS_NAME:;
            domainid_t pid    = msg.words [1];
            
            status = -1;

            //DBG: debug_printf ("Got AOS_RPC_GET_PROCESS_NAME <== 0x%x\n", pid);

            if ((pid < COUNT_OF(ddb)) && (ddb[pid].name[0] != '\0')) {
                uint32_t args[8];
                
                strcpy((char*)args, ddb[pid].name);

                lmp_chan_send9 (lc, 0, NULL_CAP, 0, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
            } else {
                lmp_chan_send1 (lc, 0, NULL_CAP, -1);
            }
            break;
        case AOS_RPC_GET_PROCESS_LIST:;
            //DBG: debug_printf ("Got AOS_RPC_GET_PROCESS_LIST\n");
            uint32_t args[7] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
            int      didx    =  0                                                                                  ;

            idx = 0xffffffff;
            
            for (int i = msg.words [1]; (i < COUNT_OF(ddb)) && (didx < COUNT_OF(args)); i++) {
                if (ddb[i].name[0] != '\0') {
                    args[didx] = i;
                    idx  = i + 1;
                    didx++;
                }
            }

            lmp_chan_send9 (lc, 0, NULL_CAP, 0, idx, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
            break;
        default:
            // YK: Uncomment this if you really need it.
            //debug_printf ("Got default value\n");
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
        // bail out, there isn't much we can do without a self endpoint.
        abort();
    }

    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    err = initialize_device_frame_server (cap_io);

    // Initialize the LED driver.
    led_init ();
    led_set_state (true);

    // Initialize the serial driver.
    if (err_is_ok (err)) {
        init_uart_driver ();
    }

    if (err_is_fail (err)) {
        debug_printf ("Failed to initialize: %s\n", err_getstring (err));
    }
    debug_printf("initialized dev memory management\n");


    // Set up some data structures for RPC services.
    init_data_structures ();

    struct lmp_chan memeater_chan;
    spawn_with_channel ("memeater", &memeater_chan);

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
