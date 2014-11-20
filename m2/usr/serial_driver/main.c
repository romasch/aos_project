/**
 *\brief A user-space serial driver.
 */

#include <barrelfish/aos_rpc.h>

// From Milestone 0...
#define UART_BASE 0x48020000
#define UART_SIZE 0x1000
#define UART_SIZE_BITS 12

static uint32_t dev_base;

static domainid_t foreground_domain = 0;

static void init_uart_driver (struct capref uart_cap)
{
    errval_t error = SYS_ERR_OK;
    // Map the device into virtual address space.
    void* buf;
    int flags = KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE | KPI_PAGING_FLAGS_NOCACHE;
    error = paging_map_frame_attr (get_current_paging_state(), &buf, UART_SIZE, uart_cap, flags, NULL, NULL);

    dev_base = (uint32_t) buf;

    //TODO: Get the device in a consistent state.
}

static char uart_getchar(void)
{
    volatile uint32_t* uart_lsr = (uint32_t*) (dev_base + 0x14);
    volatile uint32_t* uart_rhr = (uint32_t*) (dev_base       );

    for (; ((*uart_lsr) & 0x01) == 0 ;);

    return *uart_rhr;
}

static void uart_putchar(char c)
{
    volatile uint32_t* uart_lsr = (uint32_t*) (dev_base + 0x14);
    volatile uint32_t* uart_thr = (uint32_t*) (dev_base       );

    if (c == '\n')
        uart_putchar('\r');

    for (; ((*uart_lsr) & 0x20) == 0 ;);

    *uart_thr = c;
}

static void handler (void *arg)
{
//     debug_printf ("LMP message in serial driver...\n");
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
        case AOS_RPC_SERIAL_PUTCHAR:;
            char output_character = msg.words [1];
            uart_putchar (output_character);
            break;
        case AOS_RPC_SERIAL_GETCHAR:;
            if (foreground_domain == 0 || msg.words [1] == foreground_domain) {
                char input_character = uart_getchar ();
                lmp_chan_send2 (lc, 0, NULL_CAP, SYS_ERR_OK, input_character);
            } else {
                // TODO: let it wait!
                lmp_chan_send2 (lc, 0, NULL_CAP, -1, 0);
            }
            break;
        case AOS_RPC_SET_FOREGROUND:;
            foreground_domain = msg.words [1];
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
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
    debug_printf ("serial_driver: started as %s\n", argv[0]);

    struct aos_rpc* rpc = aos_rpc_get_init_channel ();
    struct lmp_chan* init_channel = &(rpc->channel);

    struct capref frame;
    errval_t error = aos_rpc_get_dev_cap (rpc, UART_BASE, UART_SIZE, &frame, NULL);
    debug_printf ("Getting frame cap: %s\n", err_getstring (error));

    init_uart_driver (frame);

    error = aos_register_service (rpc, aos_service_serial);

    error = lmp_chan_register_recv (init_channel, get_default_waitset (), MKCLOSURE (handler, init_channel));// TODO: error handling

    while (true) {
        error = event_dispatch (get_default_waitset());
    }
    debug_printf ("serial_driver returned\n", error);
}
