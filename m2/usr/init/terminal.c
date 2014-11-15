#if 0
#include "ipc_protocol.h"
#include "init.h"

#include <barrelfish/lmp_chan.h>
#include <mm/mm.h>


static struct lmp_chan server_channel;
//static struct lmp_chan uart___channel;

static char buffer[1024];
static int  pointer      = 0;

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

    debug_printf ("terminal_thread:S: Have LMP message received - %u\n", type);

    switch (type)
    {
        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...

        case AOS_GET_SERVICE:;
            /*if (msg.words[1] == STATUS_SUCCESS) {
                debug_printf ("terminal_thread:S: BLA-BLA-BLA");
                
                debug_printf ("terminal_thread:S: Successfully registered\n", type);
            }*/
            break;
            
        case UART_CONNECT:;
            /*errval_t status = STATUS_ALREADY_EXIST;
            
            debug_printf ("terminal_thread:S: UART_CONNECT\n", type);
            
            if (!capref_is_null(cap))
            {
                if (connected == false) {
                    status = lmp_chan_accept(&client_channel, DEFAULT_LMP_BUF_WORDS, cap);
                    if (err_is_fail (err)) {
                        debug_printf ("\tERROR: terminal_thread: failed to accept client LMP channel.\n");
                    } else {
                        status = lmp_chan_register_recv(&client_channel, get_default_waitset(), MKCLOSURE(client_handler, NULL));
                        if (err_is_fail (err)) {
                            debug_printf ("\tERROR: terminal_thread: failed to register client LMP channel.\n");
                        } else {
                            connected = true;
                        }
                          
                    }
                }
                
                lmp_ep_send2(cap, 0, NULL_CAP, UART_CONNECT, status);
            }*/
            break;

        case UART_RECV_BYTE:;
            buffer[pointer] = msg.words[1];           
            pointer++;

            if (msg.words[1] == '\n') {
                for (int i = 0; buffer[i] != '\n'; i++) {
                    lmp_ep_send2(cap_initep, 0, NULL_CAP, UART_SEND_BYTE, buffer[i]);
                }

                pointer = 0;
            }    
            
            lmp_ep_send1(cap_initep, 0, NULL_CAP, UART_RECV_BYTE);
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

int terminal_thread(void* arg)
{
    errval_t err;

    // A small test for our separate page fault handler.
    debug_printf ("terminal_thread: STARTED.\n");
    // ----------------------------------------------

    err = lmp_chan_accept(&server_channel, DEFAULT_LMP_BUF_WORDS, cap_initep);
    if (err_is_fail (err)) {
        debug_printf ("\tERROR: terminal_thread: failed to accept LMP channel.\n");
    }

    err = lmp_chan_register_recv(&server_channel, get_default_waitset(), MKCLOSURE(server_handler, NULL));
    if (err_is_fail (err)) {
        debug_printf ("\tERROR: terminal_thread: failed to register LMP channel.\n");
    }
    
    err = lmp_ep_send2(cap_initep, LMP_SEND_FLAGS_DEFAULT, NULL_CAP/*server_channel.local_cap*/, AOS_GET_SERVICE, SERVICE_UART_DRIVER);
    if (err_is_fail (err)) {
        debug_printf ("\tERROR: terminal_thread: failed to register self in init 0x%x.\n");
    }

    debug_printf ("\tSUCCESS!\n");
    
    for (;;) {
        err = event_dispatch (get_default_waitset()); 
        if (err_is_fail (err)) {
            debug_printf ("\tERROR: terminal_thread: Handling LMP message: %s\n", err_getstring (err));
        }
    }
    // ----------------------------------------------
    debug_printf ("terminal_thread: FINISHED.\n");
    
    return EXIT_SUCCESS;
}
#endif