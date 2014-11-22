/**
 *\brief A test thread for init which takes the role of a simple service provider.
 */

#define VERBOSE

#include <barrelfish/aos_dbg.h>
#include <aos_support/server.h>

static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message,
                           struct capref capability, uint32_t type)
{
    handle_unknown_message (channel, capability);
}

int main (int argc, char *argv[])
{
    errval_t error = SYS_ERR_OK;
    debug_printf_quiet ("test_domain: started as %s\n", argv[0]);

    error = start_server (aos_service_test, my_handler);

    debug_printf_quiet ("test_domain returned\n", error);
}
