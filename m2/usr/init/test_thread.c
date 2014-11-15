/**
 *\brief A test thread for init which takes the role of a simple service provider.
 */

#include "init.h"
#include <barrelfish/aos_rpc.h>

extern struct lmp_chan* services [aos_service_guard];

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

/**
 * The thread's main function.
 *
 * \param arg: The remote endpoint of an LMP channel to init, similar to cap_initep for processes.
 */
static int test_thread (void* arg)
{
    // TODO: Rewrite this function, taking advantage of features in aos_rpc module.
    // TODO: We probably can't use the default waitset here and need to create our own waitset.
    // This has implications in aos_rpc.c as well.

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

    // We'll do a shortcut and finalize the connection by hand.
    services [aos_service_test] -> remote_cap = thread_channel->local_cap;

    while (true) {
        event_dispatch (get_default_waitset());
    }
    debug_printf ("test_thread: end of thread reached.\n");
    return 0;
}

errval_t spawn_test_thread ( void (*handler_func) (void* arg))
{
    // First we need to set up a new channel for the user-level thread
    // to communicate with main thread.
    errval_t error;

    struct lmp_chan* init_channel = malloc (sizeof (struct lmp_chan));
    if (!init_channel) {
        error = LIB_ERR_MALLOC_FAIL;

    } else {

        lmp_chan_init (init_channel);
        // TODO: error handling
        error = lmp_chan_accept (init_channel, DEFAULT_LMP_BUF_WORDS, NULL_CAP);
        error = lmp_chan_alloc_recv_slot (init_channel);
        error = lmp_chan_register_recv (init_channel, get_default_waitset(), MKCLOSURE (handler_func, init_channel));

        // We'll use a shortcut and register the service by hand.
        services [aos_service_test] = init_channel;

        // Now we can launch the thread.
        thread_create (test_thread, &init_channel->local_cap);
    }
    return error;
}