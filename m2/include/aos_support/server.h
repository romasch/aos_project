/**
 * Support features for building servers.
 */

#ifndef SERVER_H
#define SERVER_H

#include <barrelfish/aos_rpc.h>

/**
 * A handler function for a server.
 */
typedef void (*handler_function_t) (
    struct lmp_chan* channel,
    struct lmp_recv_msg* message,
    struct capref capability,
    uint32_t message_type);

// Copy-Paste template for handler:
// static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message, struct capref capability, uint32_t type)

/**
 * \brief Handle an unknown message.
 *
 * Deletes the capability (if any) and sends back an error.
 */
void handle_unknown_message (struct lmp_chan* channel, struct capref capability);

/**
 * Initialize and start a server.
 *
 * \param service The AOS service provided by this server.
 * \param handler A handler function to handle client requests.
 */
errval_t start_server (enum aos_service service, handler_function_t handler);

#endif
