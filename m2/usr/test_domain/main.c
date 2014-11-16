/**
 *\brief A test thread for init which takes the role of a simple service provider.
 */

#include <barrelfish/aos_rpc.h>

static void handler (void *arg)
{
    debug_printf ("LMP message in test_domain...\n");
    errval_t err = SYS_ERR_OK;
    struct lmp_chan* lc = (struct lmp_chan*) arg;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;

    // Retrieve capability and arguments.
    err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(handler, arg));
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
            error_ret = lmp_chan_register_recv (channel, get_default_waitset (), MKCLOSURE (handler, channel));// TODO: error handling

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
    lmp_chan_register_recv (lc, get_default_waitset(), MKCLOSURE(handler, arg));
}

int main (int argc, char *argv[])
{
    debug_printf ("test_domain: started as %s\n", argv[0]);

    struct aos_rpc* rpc = aos_rpc_get_init_channel ();
    struct lmp_chan* init_channel = &(rpc->channel);

    errval_t error = aos_register_service (rpc, aos_service_test);

    lmp_chan_register_recv (init_channel, get_default_waitset (), MKCLOSURE (handler, init_channel));// TODO: error handling

    while (true) {
       error = event_dispatch (get_default_waitset());
    }
}
