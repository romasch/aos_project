/**
 *\brief A user-space serial driver.
 */

#include <barrelfish/aos_dbg.h>
#include <barrelfish/aos_rpc.h>
#include <aos_support/server.h>

// From Milestone 0...
#define UART_BASE 0x48020000
#define UART_SIZE 0x1000
#define UART_SIZE_BITS 12

static uint32_t dev_base;

static domainid_t foreground_domain = 0;

static errval_t init_uart_driver (struct capref uart_cap)
{
    errval_t error = SYS_ERR_OK;
    void* buf = NULL;
    int flags = KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE | KPI_PAGING_FLAGS_NOCACHE;

    // Map the device into virtual address space.
    error = paging_map_frame_attr (get_current_paging_state(), &buf, UART_SIZE, uart_cap, flags, NULL, NULL);

    if (err_is_fail (error)) {
        debug_printf ("Error while mapping device frame: %s\n", err_getstring (error));
    } else {
        dev_base = (uint32_t) buf;
        //TODO: Get the device in a consistent state.
    }

    return error;
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

static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message, struct capref capability, uint32_t message_type)
{
    switch (message_type) {
        case AOS_RPC_SERIAL_PUTCHAR:;
            char output_character = message -> words [1];
            uart_putchar (output_character);
            break;
        case AOS_RPC_SERIAL_GETCHAR:;
            if (foreground_domain == 0 || message -> words [1] == foreground_domain) {
                char input_character = uart_getchar ();
                lmp_chan_send2 (channel, 0, NULL_CAP, SYS_ERR_OK, input_character);
            } else {
                // TODO: let it wait!
                lmp_chan_send2 (channel, 0, NULL_CAP, -1, 0);
            }
            break;
        case AOS_RPC_SET_FOREGROUND:;
            foreground_domain = message -> words [1];
            debug_printf_quiet ("Setting Domain %u as foreground domain\n", foreground_domain);
            lmp_chan_send1 (channel, 0, NULL_CAP, SYS_ERR_OK);
            break;
        default:
            handle_unknown_message (channel, capability);
    }
}

int main (int argc, char *argv[])
{
    debug_printf_quiet ("serial_driver: started as %s\n", argv[0]);

    struct aos_rpc* rpc = aos_rpc_get_init_channel ();
    struct capref frame;

    // Get the device frame.
    errval_t error = aos_rpc_get_dev_cap (rpc, UART_BASE, UART_SIZE, &frame, NULL);

    debug_printf_quiet ("Getting frame cap: %s\n", err_getstring (error));

    // Map the frame and initialize the device.
    if (err_is_ok (error)) {
        error = init_uart_driver (frame);
    }

    // Register with init and start the event loop.
    if (err_is_ok (error)) {
        start_server (aos_service_serial, my_handler);
    }
    debug_printf_quiet ("serial_driver returned\n", error);
}
