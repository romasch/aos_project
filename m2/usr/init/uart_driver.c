#include "ipc_protocol.h"
#include "init.h"

#include <barrelfish/lmp_chan.h>
#include <mm/mm.h>

//static struct lmp_chan client_channel;
static struct lmp_chan server_channel;

static bool connected = false;
static bool terminate = false;

static uint32_t dev_base;

/*static void client_handler(void *arg)
{
    struct capref        cap;
    struct lmp_chan    * lc  = &client_channel  ;
    struct lmp_recv_msg  msg = LMP_RECV_MSG_INIT;
    
    errval_t err  = lmp_chan_recv(lc, &msg, &cap);
    uint32_t type;

    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(client_handler, NULL));
    }

    type = msg.words[0];

    debug_printf ("uart_driver_thread:c: Have LMP message received - %u\n", type);

    switch (type)
    {
        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...

        case UART_DISCONNECT:;
            debug_printf ("uart_driver_thread: UART_DISCONNECT\n", type);
            
            lmp_chan_destroy(&client_channel);
            
            connected = false;
            
            break;
            
        case UART_RECV_BYTE:;
            debug_printf ("uart_driver_thread: UART_RECV_BYTE\n", type);
            break;
            
        case UART_SEND_BYTE:;
            debug_printf ("uart_driver_thread: UART_SEND_BYTE\n", type);
            break;
            
        default:
            debug_printf ("Got default value\n");
            if (! capref_is_null (cap)) {
                cap_delete (cap);
                lmp_chan_set_recv_slot (lc, cap);
            }
    }

    lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(client_handler, NULL));
}*/

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

static void server_handler(void *arg)
{
    struct capref        cap;
    struct lmp_chan    * lc  = &server_channel  ;
    struct lmp_recv_msg  msg = LMP_RECV_MSG_INIT;
    
    errval_t err  = lmp_chan_recv(lc, &msg, &cap);
    uint32_t type;

    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(server_handler, NULL));
    }

    type = msg.words[0];

    debug_printf ("uart_driver_thread:S: Have LMP message received - %u\n", type);

    switch (type)
    {
        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...

        case AOS_REGISTRATION_COMPETE:;
            debug_printf ("uart_driver_thread:S: Have LMP message AOS_REGISTRATION_COMPETE\n");
                if (msg.words[1] != STATUS_SUCCESS) {
                terminate = true;
            } else {
                dev_base = msg.words[2];

                debug_printf ("uart_driver_thread:S: Successfully registered with addr: %u\n", dev_base);
            }
            break;
            
        case UART_CONNECT:;
            debug_printf ("uart_driver_thread:S: Have LMP message UART_CONNECT\n");

            connected = true;
            break;

        case UART_DISCONNECT:;
            debug_printf ("uart_driver_thread:S: Have LMP message UART_DISCONNECT\n");

            connected = false;
            break;

        case UART_RECV_BYTE:;
            debug_printf ("uart_driver_thread:S: Have LMP message UART_RECV_BYTE\n");
            
            if (connected) {
                char ch = uart_getchar();

                err = lmp_ep_send2(cap_initep, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, UART_RECV_BYTE, ch);
                if (err_is_fail (err)) {
                    debug_printf ("\tERROR: uart_driver_thread: failed to register self in init = 0x%x.\n");
                }
            }

            break;

        case UART_SEND_BYTE:;
            debug_printf ("uart_driver_thread:S: Have LMP message UART_SEND_BYTE\n");
            
            if (connected) {
                uart_putchar(msg.words[1]);
            }
            
            break;
            
        default:
            debug_printf ("Got default value\n");
            if (! capref_is_null (cap)) {
                cap_delete (cap);
                lmp_chan_set_recv_slot (lc, cap);
            }
    }

    lmp_chan_register_recv(lc, get_default_waitset(), MKCLOSURE(server_handler, NULL));
}

int uart_driver_thread(void* arg)
{
    // A small test for our separate page fault handler.
    debug_printf ("uart_driver_thread: STARTED.\n");
    // ----------------------------------------------

    errval_t err;

    err = lmp_chan_accept(&server_channel, DEFAULT_LMP_BUF_WORDS, cap_initep);
    if (err_is_fail (err)) {
        debug_printf ("\tERROR: uart_driver_thread: failed to accept LMP channel.\n");
    }

    err = lmp_chan_register_recv(&server_channel, get_default_waitset(), MKCLOSURE(server_handler, NULL));
    if (err_is_fail (err)) {
        debug_printf ("\tERROR: uart_driver_thread: failed to register LMP channel.\n");
    }

    err = lmp_ep_send2(cap_initep, LMP_SEND_FLAGS_DEFAULT, server_channel.local_cap, AOS_REGISTER_SERVICE, SERVICE_UART_DRIVER);
    if (err_is_fail (err)) {
        debug_printf ("\tERROR: uart_driver_thread: failed to register self in init = 0x%x.\n");
    }

    debug_printf ("\tSUCCESS!\n");
    
    for (; terminate == false ;) {
        err = event_dispatch (get_default_waitset()); 
        if (err_is_fail (err)) {
            debug_printf ("\tERROR: uart_driver_thread: Handling LMP message: %s\n", err_getstring (err));
        }
    }
    // ----------------------------------------------
    debug_printf ("uart_driver_thread: FINISHED.\n");
    
    return EXIT_SUCCESS;
}

