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
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>

#include <barrelfish/aos_rpc.h>

// From Milestone 0...
#define UART_BASE 0x48020000
#define UART_SIZE 0x1000

#include <mm/mm.h>

struct bootinfo *bi;
static coreid_t my_core_id;

static uint32_t example_index;
static char*    example_str  ;
static uint32_t example_size ;

/**
 * Keeps track of registered services.
 */
static struct capref services [aos_service_guard];



static struct mm device_memory_manager;
static struct slot_alloc_basecn device_memory_slot_manager;

static errval_t device_memory_refill (struct slab_alloc* allocator)
{
    debug_printf ("init_device_memory...\n");
    errval_t error = SYS_ERR_OK;

    // Allocate space for 64 device memory nodes.
    // NOTE: node size depends on maximum number of children
    // (in bits, here 1 bit ~ 2 children)
    size_t size = 64 * MM_NODE_SIZE (1);
    void* buf = malloc (size);

    if (buf == NULL) {
        error =  1; // TODO: find a suitable error.
    } else {
        slab_grow (allocator, buf, size);
    }
    debug_printf ("init_device_memory finished.\n");
    return error;
}

static void test_putchar (uint32_t base, char c)
{
    // we need to send \r\n over the serial line for a newline
    if (c == '\n') test_putchar(base, '\r');

    volatile uint32_t* uart_lsr = (uint32_t*) (base + 0x14);
    uint32_t tx_fifo_e = 0x20;
    volatile uint32_t* uart_thr = (uint32_t*) (base);

    // Wait until FIFO can hold more characters (i.e. TX_FIFO_E == 1)
    while ( ((*uart_lsr) & tx_fifo_e) == 0 ) {
        // Do nothing
    }
    // Write character
    *uart_thr = c;
}


// struct device_node {
//     struct capref cap;
//     uint8_t size_bits;
//     struct device_node* left;
//     struct device_node* right;
//     // TODO; maybe add a flag if dev node is mapped.
// };
// struct device_node root_node;

static void init_device_memory (void)
{
    // Trial-and-error findings:
    // It seems like cap_io covers an address space starting at 0x40000000
    // with 30 bits. Retyping it with 29 size bits seems to split
    // it in two new DevFrame caps located at dest and dest+1.
    // I guess retyping it with 28 size bits
    // generates 4 DevFrame caps.

    // Therefore we need to split the IO space recursively
    // until we get the dev frame cap at the right location
    // and with the correct size, and then manage the fragments...
//     root_node.cap = cap_io;
//     root_node.size_bits = 30;
//     root_node.left = NULL;
//     root_node.right = NULL;

    // Forget it, there's a predefined solution...

    // Initialize the slot allocator used by the device memory manager.
    errval_t error = slot_alloc_basecn_init(&device_memory_slot_manager);
    debug_printf ("init_device_memory: %s\n", err_getstring (error));

    // Initialize device memory manager:
    error = mm_init (
        &device_memory_manager,
        ObjType_DevFrame,       // Memory type
        0x40000000,             // Base address
        30,                     // Size bits
        1,                      // Maximum number of children in bits (i.e. 2 in this case)
        device_memory_refill,   // Slab refill function
        slot_alloc_basecn,      // Slot allocator function (defined in lib/barrelfish/mm/slot_alloc.c)
        &device_memory_slot_manager, // Slot allocator instance for this manager
        true                    // Delete chunked memory nodes (i.e. nodes with children)
    );

    debug_printf ("init_device_memory: %s\n", err_getstring (error));

    // Add the global device frame cap to the memory manager.
    error = mm_add (&device_memory_manager, cap_io, 30, 0x40000000);

    // Allocate the UART device frame.
    struct capref uart_cap;
    uint64_t uart_base = 0;

    error = mm_alloc_range (
        &device_memory_manager,
        12,                 // 12 bits for 4096==0x1000 sized page.
        UART_BASE,          // Minimum start address
        UART_BASE+UART_SIZE,// Maximum address. This boundary selection forces the allocator to return the correct page.
        &uart_cap,          // Capability to be returned.
        &uart_base          // Base address of allocated dev frame.
    );

    debug_printf ("init_device_memory: %s\n", err_getstring (error));
    // Check if allocation was successful.
    assert (uart_base == UART_BASE);


    // Map the frame
    void* buf;
    int flags = KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE | KPI_PAGING_FLAGS_NOCACHE;
    error = paging_map_frame_attr (get_current_paging_state(), &buf, UART_SIZE, uart_cap, flags, NULL, NULL);

    debug_printf ("init_device_memory: %s\n", err_getstring (error));

    uint32_t virtual_uart_base = (uint32_t) buf;

    test_putchar (virtual_uart_base, '\n');
    test_putchar (virtual_uart_base, '\n');
    test_putchar (virtual_uart_base, '*');
    test_putchar (virtual_uart_base, '\n');
    test_putchar (virtual_uart_base, '\n');

}

/**
 * Split a device node in two equal parts.
 */
// __attribute__((unused))
// static errval_t device_node_split (struct device_node* node)
// {
//     errval_t error;
//     uint8_t new_bits = node -> size_bits - 1;
//
//     struct capref new_cap;
//     // TODO: sometimes the slot after new_cap is occupied. Handle this case.
//     error = devframe_type (&new_cap, node -> cap, new_bits);
//
//     struct device_node* left = malloc (sizeof (struct device_node));
//     struct device_node* right = malloc (sizeof (struct device_node));
//
//     left -> cap = new_cap;
//     left -> size_bits = new_bits;
//     left -> left = NULL;
//     left -> right = NULL;
//
//     node -> left = left;
//
//     new_cap.slot +=1;// TODO: Check if this is actually correct.
//     right -> cap = new_cap;
//     right -> size_bits = new_bits;
//     right -> left = NULL;
//     right -> right = NULL;
//
//     node -> right = right;
//     return error;
// }


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

            // TODO: Find out why lookup for services[0] == cap_initep fails.
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

    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    init_device_memory ();
    debug_printf("initialized dev memory management\n");

    // TODO (milestone 3) STEP 2:

    // Set up the basic service registration mechanism.
    init_services ();

    // Get the default waitset.
    struct waitset* default_ws = get_default_waitset ();

    // Allocate an LMP channel and do basic initializaton.
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

    example_index =          0 ;
    example_size  =        128 ;
    example_str   = malloc(128);

    // Test thread creation.
    thread_create (test_thread, NULL);

    // Go into messaging main loop.
    while (true) {
        err = event_dispatch (default_ws);// TODO: error handling
        if (err_is_fail (err)) {
            debug_printf ("Handling LMP message: %s\n", err_getstring (err));
        }
    }

//     for (;;) sys_yield(CPTR_NULL);
    debug_printf ("init returned.");
    return EXIT_SUCCESS;

}
