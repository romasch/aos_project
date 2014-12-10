#include <aos_support/server.h>
#include <aos_support/shared_buffer.h>
#include <barrelfish/aos_dbg.h>

// The user-defined, external handler.
// NOTE: Maybe it might be better to not use a global variable.
static handler_function_t external_handler;

/**
 * Set the external handler.
 */
void set_external_handler (handler_function_t handler)
{
    external_handler = handler;
}

// Forward declaration of the shared handler function.
static void default_handler (void* arg);

/**
 * Get the default handler.
 */
dflt_handler_t get_default_handler (void)
{
    return default_handler;
}

/**
 * Creates a new LMP channel that is ready to accept messages.
 * If successful, client is responsible to free() the channel later.
 */
static errval_t create_channel (struct lmp_chan** ret_channel)
{
    errval_t error = SYS_ERR_OK;
    *ret_channel = NULL;

    // Create a new endpoint.
    struct lmp_chan* channel = malloc (sizeof (struct lmp_chan));

    if (channel) {
        lmp_chan_init (channel);

        // Initialize endpoint to receive messages.
        error = lmp_chan_accept (channel, DEFAULT_LMP_BUF_WORDS, NULL_CAP);

        // Register a receive handler.
        if (err_is_ok (error)) {
            error = lmp_chan_register_recv (channel, get_default_waitset (), MKCLOSURE (default_handler, channel));
        }
        // Allocate a slot for incoming capabilities.
        if (err_is_ok (error)) {
            error = lmp_chan_alloc_recv_slot (channel);
        }

        if (err_is_ok (error)) {
            *ret_channel = channel;
        } else{
            // Clean up...
            lmp_chan_destroy (channel);
            free (channel);
        }
    } else {
        error = LIB_ERR_MALLOC_FAIL;
    }
    return error;
}

/**
 * The shared handler function.
 *
 * Handles AOS_PING, AOS_ROUTE_REQUEST_EP and AOS_RPC_CONNECTION_INIT and forwards
 * all other message types to the external handler.
 *
 * \arg arg: The channel where the message was received from.
 */
static void default_handler (void* arg)
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
    }

    // Re-register ourselves.
    lmp_chan_register_recv (channel, get_default_waitset(), MKCLOSURE(default_handler, arg));

    // Now we're ready to handle the message.
    if (err_is_ok (error)) {

        // Extract the type of message.
        uint32_t type = message.words [0];

        // Handle common server messages.
        switch (type)
        {
            case AOS_PING:;

                // Extract message parameters.
                uint32_t ping_value = message.words [1];
                assert (!capref_is_null (capability));

                // Print a message. NOTE: Here we're always printing, as this call is mostly for debugging.
                debug_printf ("Handling PING message with value %u\n", ping_value);

                // Send a response to the ping request.
                lmp_ep_send1 (capability, 0, NULL_CAP, ping_value);

                // Destroy received capability and reuse slot.
                cap_destroy (capability);
                break;

            case AOS_ROUTE_REQUEST_EP:;

                debug_printf_quiet ("Got AOS_ROUTE_REQUEST_EP\n");

                // Extract request ID
                uint32_t id = message.words [1];
                assert (capref_is_null (capability));

                // Create the new channel.
                struct lmp_chan* new_channel;
                errval_t ret_error = create_channel (&new_channel);

                if (err_is_ok (ret_error)) {
                    // Send back the endpoint of the new channel.
                    lmp_chan_send3 (channel, 0, new_channel->local_cap, AOS_ROUTE_DELIVER_EP, ret_error, id);
                } else {
                    // Channel creation failed. Send back error message.
                    lmp_chan_send3 (channel, 0, NULL_CAP, AOS_ROUTE_DELIVER_EP, ret_error, id);
                }
                break;
            case AOS_RPC_CONNECTION_INIT:;

                debug_printf_quiet ("Got AOS_RPC_CONNECTION_INIT\n");

                // Check validity of message.
                assert (!capref_is_null (capability));

                // Set the remote capability in our channel.
                channel->remote_cap = capability;

                // Send back a reply.
                lmp_chan_send1 (channel, 0, NULL_CAP, error);
                break;

            default:
                // This is a message that may be handled by the external handler.
                debug_printf_quiet ("Message for external handler\n");
                assert (external_handler);

                external_handler (channel, &message, capability, type);
        }
    }
}

/**
 * Handle a message whose type is not known by this server.
 */
void handle_unknown_message (struct lmp_chan* channel, struct capref capability)
{
    debug_printf_quiet ("Got an unknown message!\n");
    if (! capref_is_null (capability)) {
        cap_destroy (capability);
    }
    lmp_chan_send1 (channel, 0, NULL_CAP, AOS_ERR_LMP_MSGTYPE_UNKNOWN);
}

/**
 * Register this service at init, set up the receive handler,
 * and go into the event dispatch loop.
 *
 * \arg handler: A handler function for all messages handled by this server.
 */
errval_t start_server (enum aos_service service, handler_function_t handler)
{
    // Set up the static external handler.
    external_handler = handler;
    errval_t error = SYS_ERR_OK;

    struct aos_rpc* init_rpc = aos_rpc_get_init_channel ();
    struct lmp_chan* init_channel = &(init_rpc->channel);

    error = aos_register_service (init_rpc, service);

    if (err_is_ok (error)) {
        error = lmp_chan_register_recv (init_channel, get_default_waitset (), MKCLOSURE (default_handler, init_channel));
    }

    while (err_is_ok (error)) {
        error = event_dispatch (get_default_waitset());
    }

    return error;
}
