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
    char name[MAX_PROCESS_NAME_LENGTH + 1];
    struct capref dispatcher_frame;
    struct lmp_chan channel;
    struct lmp_chan* termination_observer;
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

// Keep track of mapped modules.
struct module_map_entry {
    char name [MAX_PROCESS_NAME_LENGTH + 1];
    struct mem_region* module;
    lvaddr_t binary;
    size_t binary_size;
};
static struct module_map_entry module_map [32];
static int module_map_size = 0;

/**
 * Initialize some data structures.
 */
static void init_data_structures (void) 
{
    for (int i=0; i<COUNT_OF(ddb); i++) {
        ddb[i].name[0] = '\0';
    }
    ddb[0].name[0] = 'i'; // TODO: write init...
    ddb[0].name[1] = '\0';

    for (int i=0; i<aos_service_guard; i++) {
        services [i] = NULL;
    }
    memset (&module_map, 0, sizeof (module_map));

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
static errval_t spawn_with_channel (char* domain_name, uint32_t domain_id, struct capref* ret_dispatcher, struct lmp_chan* ret_channel)
{
    debug_printf("Spawning new domain: %s...\n", domain_name);
    errval_t error = SYS_ERR_OK;

    // Struct to keep track of new domains cspace, vspace, etc...
    struct spawninfo new_domain;
    memset (&new_domain, 0, sizeof (struct spawninfo));
    new_domain.domain_id = domain_id;

    // Concatenate the name.
    char prefixed_name [256]; // TODO: prevent buffer overflow attacks...
    strcpy (prefixed_name, BINARY_PREFIX);
    strcat (prefixed_name, domain_name);

    //TODO: Probably we shouldn't use spawn_load_with_bootinfo.
    // Try to find a better suited function
//     error = spawn_load_with_bootinfo (&new_domain, bi, prefixed_name, my_core_id);

    // Find the module.
    struct mem_region* module = NULL;
    lvaddr_t binary = 0;
    size_t binary_size = 0;

    for (int i=0; i<module_map_size; i++) {
        if (strcmp (module_map [i].name, domain_name) == 0)  {
            module = module_map [i].module;
            binary = module_map [i].binary;
            binary_size = module_map [i].binary_size;
            break;
        }
    }
    // Not found. Try bootinfo.
    if (!module) {
        module = multiboot_find_module(bi, prefixed_name);
        if (module == NULL) {
            debug_printf("could not find module [%s] in multiboot image\n", prefixed_name);
            error = SPAWN_ERR_FIND_MODULE;
        } else {
            // Lookup and map the elf image
            error = spawn_map_module(module, &binary_size, &binary, NULL);
            if (err_is_fail(error)) {
                error = err_push(error, SPAWN_ERR_ELF_MAP);
            } else {
                strcpy ( module_map [module_map_size].name, domain_name);
                module_map [module_map_size].module = module;
                module_map [module_map_size].binary = binary;
                module_map [module_map_size].binary_size = binary_size;
                module_map_size++;
            }
        }
    }
    char* argv [] = {domain_name, NULL};
    char* envp [] = {NULL};

    if (err_is_ok (error)) {
        error = spawn_load_image (
            &new_domain,
            binary,
            binary_size,
            CPU_ARM,
            domain_name,
            my_core_id,
            argv,
            envp,
            NULL_CAP,
            NULL_CAP
        );
    }

    // Set data structures.
    if (err_is_ok(error) && ret_dispatcher) {
        // TODO; error handling. Also, it may be possible to not delete new_domain.dcb
        get_dispatcher_generic (new_domain.handle)->domain_id = domain_id;
        slot_alloc (ret_dispatcher);
        cap_copy (*ret_dispatcher, new_domain.dcb);
    } else {
        return error;
    }

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
        case AOS_RPC_GET_DEVICE_FRAME:;
            uint32_t device_addr = msg.words [1];
            uint8_t device_bits = msg.words [2];
            struct capref device_frame;
            error = allocate_device_frame (device_addr, device_bits, &device_frame);

            lmp_chan_send1 (lc, 0, device_frame, error);

            //TODO: do we need to destroy frame capability here?
//          error = cap_destroy (device_frame);

            debug_printf ("Handled AOS_RPC_GET_DEVICE_FRAME: %s\n", err_getstring (error));
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
            debug_printf ("Got AOS_ROUTE_FIND_SERVICE 0x%x\n", msg.words [1]);
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

            //DBG: debug_printf ("Got AOS_RPC_SPAWN_PROCESS <- %s\n", name);

            // Lookup of free process slot in process database
            for (int i = 0; (i < COUNT_OF(ddb)) && (idx == -1); i++) {
                if (ddb[i].name[0] == '\0') {
                    idx = i;
                }
            }
            
            if (idx != -1) {
                err = spawn_with_channel(name, idx, &ddb[idx].dispatcher_frame, &ddb[idx].channel);
                if (err_is_ok(err)) {
                    strcpy(ddb[idx].name, name);
                }
            }    
            
            // Send reply back to the client
            lmp_chan_send2(lc, 0, NULL_CAP, err, idx);
            break;
        case AOS_RPC_GET_PROCESS_NAME:;
            domainid_t pid    = msg.words [1];

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
                    didx++;
                    idx  = i + 1;
                }
            }

            lmp_chan_send9 (lc, 0, NULL_CAP, 0, idx, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
            break;
        case AOS_RPC_SET_LED:;
            bool state = msg.words [1];
            led_set_state (state);
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_KILL:;
            debug_printf ("Got AOS_RPC_KILL\n");
            // TODO: error handling;
            uint32_t pid_to_kill = msg.words [1];
            // TODO: Check if this is a self-kill. If yes sending a message is unnecessary.
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            error = cap_revoke (ddb [pid_to_kill].dispatcher_frame);
            error = cap_delete (ddb [pid_to_kill].dispatcher_frame);
            ddb [pid_to_kill].name[0] = '\0';

            // Notify the observer about termination.
            if (ddb [pid_to_kill].termination_observer) {
                lmp_chan_send1 (ddb [pid_to_kill].termination_observer, 0, NULL_CAP, SYS_ERR_OK);
            }

            // TODO: properly clean up processor, i.e. its full cspace.
            break;
        case AOS_RPC_SET_FOREGROUND:;
            debug_printf ("TODO\n");
            break;

        case AOS_RPC_WAIT_FOR_TERMINATION:;
            uint32_t pid_to_wait = msg.words [1];
            if (ddb [pid_to_wait].name[0] == '\0') {
                // Domain is already dead!
                lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            } else {
                ddb [pid_to_wait].termination_observer = lc;
            }
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

    // Set up some data structures for RPC services.
    init_data_structures ();

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
    struct lmp_chan serial_chan;
    if (err_is_ok (err)) {
//         init_uart_driver ();
        strcpy(ddb[2].name, "serial_driver");
        err = spawn_with_channel ("serial_driver", 2, &(ddb[2].dispatcher_frame), &serial_chan);
        debug_printf ("Spawning serial driver: %s\n", err_getstring (err));
        while (services [aos_service_serial] == NULL) {
            event_dispatch (get_default_waitset());
        }
    }


    if (err_is_fail (err)) {
        debug_printf ("Failed to initialize: %s\n", err_getstring (err));
    }
    debug_printf("initialized dev memory management\n");

    struct lmp_chan memeater_chan;
    strcpy(ddb[1].name, "memeater");
    spawn_with_channel ("memeater",  1, &(ddb[1].dispatcher_frame), &memeater_chan);

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
