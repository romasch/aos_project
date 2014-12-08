#include "mmchs.h"

#include <aos_support/fat32.h>
#include <aos_support/server.h>
#include <barrelfish/aos_dbg.h>

__attribute__((unused))
static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message,
                           struct capref capability, uint32_t type)
{
    // TODO: Implement this.

    handle_unknown_message (channel, capability);
}


errval_t start_filesystem_server (void)
{
    errval_t error = SYS_ERR_OK;

    error = fat32_init (mmchs_read_block);

    if (err_is_ok (error)) {
        test_fs (); // TODO remove when not needed any more.
        error = start_server (aos_service_filesystem, my_handler);
    }
    return error;
}