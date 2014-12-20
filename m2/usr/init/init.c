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

#include "init.h"
#include <stdlib.h>
#include <string.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>

#include <barrelfish/aos_rpc.h>

#include <barrelfish/aos_dbg.h>
#include <aos_support/server.h>
#include <aos_support/shared_buffer.h>
#include <aos_support/module_manager.h>

static struct bootinfo *bi;
struct bootinfo* get_bootinfo (void)
{
    return bi;
}

static coreid_t my_core_id;
coreid_t get_core_id (void)
{
    return my_core_id;
}

//Keeps track of registered services.
struct lmp_chan* services [aos_service_guard];

// Keeps track of FIND_SERVICE requests.
// TODO: use a system that supports removal as well.
#define MAX_FIND_REQUESTS 100
static struct lmp_chan* find_request [MAX_FIND_REQUESTS];
static int find_request_index = 0;

/**
 * Initialize some data structures.
 */
static void init_data_structures (char *argv[])
{
    errval_t error = SYS_ERR_OK;

    // Set the core id in the disp_priv struct
    error = invoke_kernel_get_core_id(cap_kernel, &my_core_id);

    if (err_is_fail (error)) {
        // Bail out. We need the core ID for spawning other domains.
        debug_printf ("Failed to get core ID: %s\n", err_getstring (error));
        abort();
    }

    disp_set_core_id(my_core_id);

    // First argument contains the bootinfo location
    bi = (struct bootinfo*) strtol(argv[1], NULL, 10);

    // Create our endpoint to self
    error = cap_retype(cap_selfep, cap_dispatcher, ObjType_EndPoint, 0);
    if (err_is_fail (error)) {
        // bail out, there isn't much we can do without a self endpoint.
        debug_printf ("Failed to create our endpoint to self: %s\n", err_getstring (error));
        abort();
    }

    // Make sure other data structures are correctly initialized.
    for (int i=0; i < aos_service_guard; i++) {
        services [i] = NULL;
    }
    for (int i=0; i < MAX_FIND_REQUESTS; i++) {
        find_request [i] = NULL;
    }
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

static bool is_spawned = false;
struct remote_spawn_message {
    uintptr_t message_id                              ;
    char      name      [64U - 2U * sizeof(uintptr_t)];
};

// Spawn on the second core.
static errval_t spawn_remotely (char* domain_name, coreid_t core_id)
{
    errval_t error = SYS_ERR_OK;
    // Currently this function is only supported on init.0
    assert (get_core_id () == 0);

    if (core_id == 1) {

        // We may need to spawn the second kernel first.
        if (get_core_id () == 0 && !is_spawned) {
            is_spawned = true;
            // Zero out the shared memory.
            memset (get_cross_core_buffer(), 0, BASE_PAGE_SIZE);
            // Spawn the core.
            error = spawn_core (core_id);
            debug_printf ("spawn_core: %s\n", err_getstring (error));
            // For some reason we have to wait, otherwise the message gets lost.
            for (volatile int wait = 0; wait < 5000000; wait++);
            debug_printf_quiet ("After wait\n");
        }

        struct remote_spawn_message rsm = { .message_id = IKC_MSG_REMOTE_SPAWN };

        strcpy(rsm.name, domain_name);

        void* reply_error = ikc_rpc_call(&rsm, sizeof(rsm));
        error = *(errval_t*) reply_error;
    } else {
        // Invalid remote core ID.
        error = SYS_ERR_CORE_NOT_FOUND;
    }

    return error;
}



/**
 * The main receive handler for init.
 */
static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message, struct capref cap, uint32_t type)
{
    // TODO: search-and-replace these uses.
    struct lmp_chan* lc = channel;
    struct lmp_recv_msg msg = *message;

    errval_t err = SYS_ERR_OK;

        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...
    switch (type)
    {
        case AOS_RPC_GET_RAM_CAP:;
            size_t bits = msg.words [1];
            struct capref ram;
            errval_t error = ram_alloc (&ram, bits);

            lmp_chan_send2 (lc, 0, ram, error, bits);

            error = cap_destroy (ram);
            debug_printf_quiet ("Handled AOS_RPC_GET_RAM_CAP: %s\n", err_getstring (error));
            break;
        case AOS_ROUTE_REGISTER_SERVICE:;
            debug_printf_quiet ("Got AOS_ROUTE_REGISTER_SERVICE 0x%x\n", msg.words [1]);
            assert (capref_is_null (cap));
            services [msg.words [1]] = lc;
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_GET_DEVICE_FRAME:;
            uint32_t device_addr = msg.words [1];
            uint8_t device_bits = msg.words [2];
            struct capref device_frame;
            error = allocate_device_frame (device_addr, device_bits, &device_frame);

            lmp_chan_send1 (lc, 0, device_frame, error);

            error = cap_destroy (device_frame);
            debug_printf_quiet ("Handled AOS_RPC_GET_DEVICE_FRAME: %s\n", err_getstring (error));
            break;
        case AOS_ROUTE_FIND_SERVICE:;
            debug_printf_quiet ("Got AOS_ROUTE_FIND_SERVICE 0x%x\n", msg.words [1]);
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
                lmp_chan_send1 (lc, 0, NULL_CAP, -1); // TODO: proper error value
            }
            break;
        case AOS_ROUTE_DELIVER_EP:;
            debug_printf_quiet ("Got AOS_ROUTE_DELIVER_EP\n");
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
        case AOS_RPC_SPAWN_DOMAIN:;
            uint32_t mem_desc = msg.words [1];
            coreid_t core_id = msg.words [2];
            debug_printf_quiet ("AOS_RPC_SPAWN_DOMAIN on core %u\n", core_id);
            domainid_t new_domain_id = -1;

            void* domain_name = NULL;
            err = get_shared_buffer (mem_desc, &domain_name, NULL);

            if (err_is_ok (err)) {
                if (core_id == get_core_id()) {
                    // We can spawn locally.
                    err = spawn (domain_name, &new_domain_id);
                } else {
                    err = spawn_remotely (domain_name, core_id);
                }
            }
            // Send reply back to the client
            lmp_chan_send2(lc, 0, NULL_CAP, err, new_domain_id);
            break;
        case AOS_RPC_GET_PROCESS_NAME:;
            domainid_t pid    = msg.words [1];
            debug_printf_quiet ("Got AOS_RPC_GET_PROCESS_NAME  for process %u\n", pid);

            if ((pid < max_domain_id()) && (get_domain_info (pid) -> name[0] != '\0')) {
                uint32_t args[8];

                strcpy((char*)args, get_domain_info (pid) -> name);

                lmp_chan_send9 (lc, 0, NULL_CAP, SYS_ERR_OK, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
            } else {
                lmp_chan_send1 (lc, 0, NULL_CAP, -1); //TODO: Error value
            }
            break;
        case AOS_RPC_GET_PROCESS_LIST:;
            debug_printf_quiet ("Got AOS_RPC_GET_PROCESS_LIST\n");
            uint32_t args[7] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
            int args_index = 0;

            int idx2 = 0xffffffff;

            for (int i = msg.words [1]; (i < max_domain_id()) && (args_index < 7); i++) {
                if (get_domain_info (i) -> name[0] != '\0') {
                    args[args_index] = i;
                    args_index++;
                    idx2  = i + 1;
                }
            }
            lmp_chan_send9 (lc, 0, NULL_CAP, SYS_ERR_OK, idx2, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
            break;
        case AOS_RPC_SET_LED:;
            bool state = msg.words [1];
            led_set_state (state);
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_KILL:;
            // TODO: error handling;
            uint32_t pid_to_kill = msg.words [1];
            debug_printf_quiet ("Got AOS_RPC_KILL: %u\n", pid_to_kill);
            // TODO: Check if this is a self-kill. If yes sending a message is unnecessary.
            struct domain_info* domain = get_domain_info (pid_to_kill);
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            domain -> name[0] = '\0';
            error = cap_revoke (domain -> dispatcher_frame);
            error = cap_destroy (domain -> dispatcher_frame);

            // Notify the observer about termination.
            if (domain -> termination_observer) {
                lmp_chan_send1 (domain -> termination_observer, 0, NULL_CAP, SYS_ERR_OK);
                domain -> termination_observer = NULL;
            }

            // TODO: properly clean up processor, i.e. its full cspace.
            break;

        case AOS_RPC_WAIT_FOR_TERMINATION:;
            uint32_t pid_to_wait = msg.words [1];
            debug_printf_quiet ("Got AOS_RPC_WAIT_FOR_TERMINATION: %u\n", pid_to_wait);
            struct domain_info* wait_domain = get_domain_info (pid_to_wait);

            if (wait_domain -> name[0] == '\0') {
                // Domain is already dead!
                lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            } else {
                wait_domain -> termination_observer = lc;
            }
            break;
        default:
            handle_unknown_message (channel, cap);
            debug_printf ("ERROR: Got unknown message\n");
    }

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
                err = lmp_chan_register_recv(channel, get_default_waitset(), MKCLOSURE(get_default_handler(), channel));
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

__attribute__((unused))
static struct thread* ikcsrv;

int main(int argc, char *argv[])
{
    errval_t err = SYS_ERR_OK;

    // Initialize some core data structures.
    init_data_structures (argv);

    debug_printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

    debug_printf_quiet ("FIRSTEP_OFFSET should be %zu + %zu\n", get_dispatcher_size(), offsetof(struct lmp_endpoint, k));


    // setup memory serving
    err = initialize_ram_alloc();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init local ram allocator");
        abort();
    }
    debug_printf_quiet ("initialized local ram alloc\n");

    // setup memory serving
    err = initialize_mem_serv();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init memory server module");
        abort();
    }

    // Set the external handler in the server module.
    set_external_handler (my_handler);

    // (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    err = initialize_device_frame_server (cap_io);

    // Initialize the LED driver.
    err = led_init ();
    if (err_is_ok (err)) {
        led_set_state (true);
    } else {
        debug_printf ("Failed to initialize LED driver: %s\n", err_getstring (err));
    }


    // Initialize the module manager.
    err = module_manager_init (bi);
    assert (err_is_ok (err));

    // Initialize the domain manager.
    err = init_domain_manager ();
    assert (err_is_ok (err));

    // Map the shared cross core buffer into our address space.
    err = init_cross_core_buffer ();
    assert (err_is_ok (err));

    if (get_core_id () != 0) {
        // We're the second init and need to create a listener thread.
        ikcsrv = thread_create(ikc_server, NULL);

    } else {
        // Initialize the serial driver.
        err = spawn ("serial_driver", NULL);
        if (err_is_fail (err)) {
            debug_printf ("Error: Failed to spawn serial driver: %s\n", err_getstring (err));
        } else {
            while (services [aos_service_serial] == NULL) {
                event_dispatch (get_default_waitset());
            }
        }
        // Initialize the filesystem.
        err = spawn ("mmchs", NULL);
        if (err_is_fail (err)) {
            debug_printf ("Error: Failed to spawn mmchs: %s\n", err_getstring (err));
        } else {
            while (services [aos_service_filesystem] == NULL) {
                event_dispatch (get_default_waitset());
            }
        }
        debug_printf_quiet ("initialized core services\n");

        // Spawn the shell.
        err = spawn ("memeater", NULL);
    }

    // Go into messaging main loop.
    while (err_is_ok (err)) {
        err = event_dispatch (get_default_waitset());
        if (err_is_fail (err)) {
            debug_printf ("Handling LMP message: %s\n", err_getstring (err));
        }
    }

    debug_printf ("Init returned: %s.\n", err_getstring (err));
    return EXIT_SUCCESS;
}
