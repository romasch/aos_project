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
#include <string.h>

#include <barrelfish/aos_rpc.h>

#include <barrelfish/aos_dbg.h>
#include <aos_support/server.h>
#include <aos_support/shared_buffer.h>
#include <aos_support/module_manager.h>

// Forward declaration
static errval_t enable_elf_loading (struct lmp_chan* fs_chan);

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
#define MAX_FIND_REQUESTS 100
static struct lmp_chan* find_request [MAX_FIND_REQUESTS];

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
    errval_t error = SYS_ERR_OK;

    struct domain_info* domain = NULL;

        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...
    switch (type)
    {
        case AOS_RPC_GET_RAM_CAP:;
            size_t bits = message -> words [1];
            struct capref ram;
            error = ram_alloc (&ram, bits);

            lmp_chan_send2 (channel, 0, ram, error, bits);

            error = cap_destroy (ram);
            debug_printf_quiet ("Handled AOS_RPC_GET_RAM_CAP: %s\n", err_getstring (error));
            break;
        case AOS_ROUTE_REGISTER_SERVICE:;
            debug_printf_quiet ("Got AOS_ROUTE_REGISTER_SERVICE 0x%x\n", message -> words [1]);
            assert (capref_is_null (cap));
            services [message -> words [1]] = channel;
            lmp_chan_send1 (channel, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_GET_DEVICE_FRAME:;
            uint32_t device_addr = message -> words [1];
            uint8_t device_bits = message -> words [2];
            struct capref device_frame;
            error = allocate_device_frame (device_addr, device_bits, &device_frame);

            lmp_chan_send1 (channel, 0, device_frame, error);

            error = cap_destroy (device_frame);
            debug_printf_quiet ("Handled AOS_RPC_GET_DEVICE_FRAME: %s\n", err_getstring (error));
            break;
        case AOS_ROUTE_FIND_SERVICE:;
            debug_printf_quiet ("Got AOS_ROUTE_FIND_SERVICE 0x%x\n", message -> words [1]);

            // find correct server channel
            uint32_t requested_service = message -> words [1];
            if (0 <= requested_service && requested_service < aos_service_guard) {
                struct lmp_chan* serv = services [requested_service];

                if (serv != NULL) {
                    // generate new ID
                    bool found = false;
                    for (int i = 0; i < MAX_FIND_REQUESTS && !found; i++) {

                        if (find_request [i] == NULL) {
                            found = true;
                            // store current channel at ID
                            find_request [i] = channel;
                            // generate AOS_ROUTE_REQUEST_EP request with ID.
                            lmp_chan_send2 (serv, 0, NULL_CAP, AOS_ROUTE_REQUEST_EP, i);
                        }
                    }
                    if (!found) {
                        // No free space to allocate a find request ID.
                        // This is very rare...
                        lmp_chan_send1 (channel, 0, NULL_CAP, -1); // TODO: Proper error value
                    }
                } else {
                    // Service is not registered.
                    lmp_chan_send1 (channel, 0, NULL_CAP, -1); // TODO: proper error value
                }
            } else {
                // Invalid service identifier.
                lmp_chan_send1 (channel, 0, NULL_CAP, AOS_ERR_LMP_INVALID_ARGS);
            }
            break;
        case AOS_ROUTE_DELIVER_EP:;
            debug_printf_quiet ("Got AOS_ROUTE_DELIVER_EP\n");
            // get error value and ID from message
            errval_t error_ret = message -> words [1];
            uint32_t req_id = message -> words [2];
            // lookup receiver channel at ID
            struct lmp_chan* recv = find_request [req_id];
            find_request [req_id] = NULL;
            // generate response with cap and error value
            lmp_chan_send1 (recv, 0, cap, error_ret);
            // Delete capability and reuse slot.
            error = cap_destroy (cap);
            break;
        case AOS_RPC_SPAWN_DOMAIN:;
            uint32_t mem_desc = message -> words [1];
            coreid_t core_id = message -> words [2];
            debug_printf_quiet ("AOS_RPC_SPAWN_DOMAIN on core %u\n", core_id);
            domainid_t new_domain_id = -1;

            void* domain_name = NULL;
            error = get_shared_buffer (mem_desc, &domain_name, NULL);

            if (err_is_ok (error)) {
                if (core_id == get_core_id()) {
                    // We can spawn locally.
                    error = spawn (domain_name, &new_domain_id);
                } else {
                    error = spawn_remotely (domain_name, core_id);
                }
            }
            // Send reply back to the client
            lmp_chan_send2(channel, 0, NULL_CAP, error, new_domain_id);
            break;
        case AOS_RPC_GET_PROCESS_NAME:;
            domainid_t pid    = message -> words [1];
            debug_printf_quiet ("Got AOS_RPC_GET_PROCESS_NAME  for process %u\n", pid);

            domain = get_domain_info (pid);

            if ((pid < max_domain_id()) && domain -> state == domain_info_state_running) {
                uint32_t args[8];

                strcpy((char*)args, domain -> name);

                lmp_chan_send9 (channel, 0, NULL_CAP, SYS_ERR_OK, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
            } else {
                lmp_chan_send1 (channel, 0, NULL_CAP, -1); //TODO: Error value
            }
            break;
        case AOS_RPC_GET_PROCESS_LIST:;
            debug_printf_quiet ("Got AOS_RPC_GET_PROCESS_LIST\n");
            uint32_t args[7] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
            int args_index = 0;

            int idx2 = 0xffffffff;

            for (int i = message -> words [1]; (i < max_domain_id()) && (args_index < 7); i++) {
                if (get_domain_info (i) -> state == domain_info_state_running) {
                    args[args_index] = i;
                    args_index++;
                    idx2  = i + 1;
                }
            }
            lmp_chan_send9 (channel, 0, NULL_CAP, SYS_ERR_OK, idx2, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
            break;
        case AOS_RPC_SET_LED:;
            bool state = message -> words [1];
            led_set_state (state);
            lmp_chan_send1 (channel, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_KILL:;
            // TODO: Find a way to get back frames and device frames.
            uint32_t pid_to_kill = message -> words [1];
            debug_printf_quiet ("Got AOS_RPC_KILL: %u\n", pid_to_kill);

            domain = get_domain_info (pid_to_kill);

            // Users can only kill running domains.
            if (domain && domain -> state == domain_info_state_running) {
                domain -> state = domain_info_state_zombie;

                // Revoke dispatcher and root cnode.

                error = cap_revoke (domain -> dispatcher_capability);

                if (err_is_ok (error)) {
                    error = cap_destroy (domain -> dispatcher_capability);
                }
                if (err_is_ok (error)) {
                    error = cap_revoke (domain -> root_cnode_capability);
                }
                if (err_is_ok (error)) {
                    error = cap_destroy (domain -> root_cnode_capability);
                }

                // Send back an acknowledgement if it's not a self-kill.
                if (channel != domain -> channel) {
                    lmp_chan_send1 (channel, 0, NULL_CAP, SYS_ERR_OK);
                }

                // Notify any observer about termination.
                if (domain -> termination_observer) {
                    lmp_chan_send1 (domain -> termination_observer, 0, NULL_CAP, SYS_ERR_OK);
                    domain -> termination_observer = NULL;
                }
            } else {
                lmp_chan_send1 (channel, 0, NULL_CAP, AOS_ERR_LMP_INVALID_ARGS);
            }
            break;

        case AOS_RPC_WAIT_FOR_TERMINATION:;
            uint32_t pid_to_wait = message -> words [1];
            debug_printf_quiet ("Got AOS_RPC_WAIT_FOR_TERMINATION: %u\n", pid_to_wait);

            domain = get_domain_info (pid_to_wait);

            if (domain -> state != domain_info_state_running) {
                lmp_chan_send1 (channel, 0, NULL_CAP, SYS_ERR_OK);
            } else {
                domain -> termination_observer = channel;
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
        // NOTE: This is a workaround for a bug.
        // init.1 crashes if the CSpace of all its child domains are revoked.
        // With test_domain we can make sure that at least one domain stays active.
        spawn ("test_domain", NULL);
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
        domainid_t filesystem_domain = 0;
        err = spawn ("mmchs", &filesystem_domain);
        if (err_is_fail (err)) {
            debug_printf ("Error: Failed to spawn mmchs: %s\n", err_getstring (err));
        } else {
            while (services [aos_service_filesystem] == NULL) {
                event_dispatch (get_default_waitset());
            }
            enable_elf_loading (services[aos_service_filesystem]);
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

// ELF loading setup code.
// This is a bit hacky, but it should work.

static struct capref fs_client_cap;
static struct aos_rpc fs_client_channel;

static void fs_alloc_handler (void* arg)
{
    debug_printf_quiet ("Handling LMP message...\n");
    errval_t error = SYS_ERR_OK;
    struct lmp_chan* channel = (struct lmp_chan*) arg;
    struct lmp_recv_msg message = LMP_RECV_MSG_INIT;
    struct capref capability;

    // Retrieve capability and arguments.
    error = lmp_chan_recv(channel, &message, &capability);

    // Reallocate a slot if we just received a capability.
    if (err_is_ok (error) && !capref_is_null (capability)) {
        lmp_chan_alloc_recv_slot (channel);
        fs_client_cap = capability;
    } else {
        fs_client_cap = NULL_CAP;
    }
}

// TODO: Error handling!
static errval_t enable_elf_loading (struct lmp_chan* fs_chan)
{
    errval_t error = SYS_ERR_OK;
    error = lmp_chan_deregister_recv (fs_chan);
    error = lmp_chan_register_recv (fs_chan, get_default_waitset(), MKCLOSURE (fs_alloc_handler, fs_chan));
    error = lmp_chan_send2 (fs_chan, 0, NULL_CAP, AOS_ROUTE_REQUEST_EP, 0);
    error = event_dispatch (get_default_waitset());
    assert (!capref_is_null (fs_client_cap));
    error = aos_rpc_init (&fs_client_channel, fs_client_cap);
    error = lmp_chan_register_recv (fs_chan, get_default_waitset(), MKCLOSURE (get_default_handler(), fs_chan));
    module_manager_enable_filesystem (&fs_client_channel);
    return error;
}
