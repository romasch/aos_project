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

struct bootinfo *bi;
static coreid_t my_core_id;

/**
 * A basic receive handler.
 * This code is mostly copy-pasted from the AOS tutorial lecture slides.
 */
static void recv_handler(void *arg)
{
    debug_printf ("Handling LMP message...\n");
    errval_t err = SYS_ERR_OK;
    struct lmp_chan* lc = (struct lmp_chan*) arg;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;
    err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(recv_handler, arg));
    }

    // TODO: devise a protocol and properly demultiplex different messages
    if (msg.words[0] == 100) {
        // Send a kind of echo message.
        // NOTE: don't use LMP_SEND_FLAGS_DEFAULT, otherwise control
        // will be transfered back to sender immediately...
        lmp_ep_send0 (cap, 0, NULL_CAP);
        // Delete capability and reuse slot.
        cap_delete (cap);
        lmp_chan_set_recv_slot (lc, cap);
    }
    lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(recv_handler, arg));
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
    debug_printf("initialized dev memory management\n");

    // TODO (milestone 3) STEP 2:

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

    // Go into messaging main loop.
    while (true) {
        err = event_dispatch (default_ws);// TODO: error handling
        debug_printf ("Handling LMP message: %s\n", err_getstring (err));
    }

    //NOTE: added such that init doesn't finish before memserver can run.
    // Can be removed later.

    event_dispatch(get_default_waitset());

//     for (;;) sys_yield(CPTR_NULL);
    debug_printf ("init returned.");
    return EXIT_SUCCESS;
}
