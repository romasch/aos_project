#include <barrelfish/barrelfish.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>
#include <barrelfish/lmp_chan.h>


extern size_t (*_libc_terminal_read_func)(char *, size_t);
extern size_t (*_libc_terminal_write_func)(const char *, size_t);

// #define BUFSIZE (128UL*1024*1024)
#define BUFSIZE (4UL*1024*1024)

static struct aos_rpc* serial_channel;
static struct aos_rpc* test_channel;


static void test_route_to_init_thread (void);

static bool starts_with(const char *prefix, const char *str)
{
    uint32_t lenpre = strlen(prefix);

    return (( strlen(str) < lenpre ) ? ( false ) : ( strncmp(prefix, str, lenpre) == 0 ));
}

static size_t aos_rpc_terminal_write(const char *buf, size_t len)
{
    for (int i=0; i<len; i++) {
        aos_rpc_serial_putchar (serial_channel, buf[i]);
    }
    return 0;
}

static size_t aos_rpc_terminal_read (char *buf, size_t len)
{
    // probably scanf always only wants to read one character anyway...
    int i = 0;
    char c;
    do {
        aos_rpc_serial_getchar (serial_channel, &c);
        buf [i] = c;
        i++;
    } while (c != '\n' && c != '\r' && i+1 < len);
    return i;
}


/**
 * A simple shell that handles echo, run_memtest and exit commands.
 */
static void start_shell (void)
{
    debug_printf ("Started simple shell...\n");
    
    char buf[256];
    int  number   = 0;

    while (true) {
        bool finished = false;
        int  i        = 0    ;

        // Collect input characters in the string buffer until the moment
        // when we will have 'carriage return' character.
        // Then we will consider it as a single statement
        for (; (finished == false) && (i < 256); i++) {
            aos_rpc_serial_getchar(serial_channel, &buf[i]);
            
            if (buf[i] == '\r') {
                finished = true; 

                // We don't want to see 'CRNL' line ending
                buf[i] = '\n';

                // Reflect character back to the terminal to 
                // let the user see what he typing.
                aos_rpc_serial_putchar(serial_channel, buf[i]);
            } else {     
                aos_rpc_serial_putchar(serial_channel, buf[i]);
            }
        }
        
        // Start statement processing.
        if (starts_with("echo", buf) != false) {
            printf ("echo: %s", &buf[5]);
        } else if (starts_with("run_memtest ", buf) != false) {
            number = atoi(&buf[12]); 
            
            if (number > 0) {
                debug_printf ("Running memtest.\n");
                char* mbuf = malloc (number);
                for (int mi=0; mi<number; mi++) {
                    mbuf[mi] = mi % 255;
                }
                free (mbuf);
                debug_printf ("Memtest finished.\n");
            }
        } else if (starts_with ("exit", buf) != false) {
            break;
        } else {
            // ignore
        }
    }
}

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");
    serial_channel = aos_rpc_get_init_channel ();
    test_channel = aos_rpc_get_init_channel ();


    errval_t error = SYS_ERR_OK;

    // STEP 1: connect & send msg to init using syscall
    uint32_t flags = LMP_FLAG_SYNC | LMP_FLAG_YIELD;
    //Test sending a basic message.
    error = lmp_ep_send1 (cap_initep, flags, NULL_CAP, 42);
    debug_printf ("Send message: %s\n", err_getstring (error));

    //Test sending a capability.
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));

    test_route_to_init_thread();

    // Test opening another channel.
    // NOTE: A first channel is already created to talk to RAM server.

//    error = aos_rpc_init(&arpc, cap_initep);
    if (err_is_fail (error)) {
        debug_printf ("Error! in aos_rpc_init(0x%x) = %u", 0U, error);
    }

    aos_ping (test_channel, 46);
    aos_ping (test_channel, 42);
    aos_ping (test_channel, 43);
    aos_ping (test_channel, 44);
    aos_ping (test_channel, 45);

    // NOTE: debug_printf probably has a size restriction, therefore we should use printf.
//     debug_printf ("Send message: ");
//     printf ("%s\n", "Very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very long test string");
    error = aos_rpc_send_string(test_channel, "Very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very long test string");
    
    if (err_is_fail (error)) {
        debug_printf ("Error! in aos_rpc_send_string(0x%x, %s) = %u", test_channel, "Test string", error);
    }

    // TODO STEP 5: test memory allocation using memserv
    char* buf = malloc (BUFSIZE);
    debug_printf ("Buffer allocated.\n");
    for (int i=0; i<BUFSIZE; i++) {
        buf[i] = i % 255;
    }
    debug_printf ("Buffer filled.\n");

    aos_rpc_serial_putchar (serial_channel, '+');
    char c [3] = {0, '\n',0};
    aos_rpc_serial_getchar (serial_channel, c);
    debug_printf (c);

    // TODO: actually we should do this way earlier, in lib/barrelfish/init.c
    _libc_terminal_read_func = aos_rpc_terminal_read;
    _libc_terminal_write_func = aos_rpc_terminal_write;

    struct capref test_ep;
    aos_find_service (aos_service_test, &test_ep);

    start_shell ();

    debug_printf ("memeater returned\n");
    return 0;
}

static void test_handler (void* arg) {
    struct lmp_chan* channel = arg;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;
    errval_t error = lmp_chan_recv(channel, &msg, &cap);
    debug_printf ("Received ACK: %s\n", err_getstring (error));
}

static void test_route_to_init_thread (void)
{
    // Test routing
    struct capref test_thread;
    errval_t error;
    error = aos_find_service (aos_service_test, &test_thread);
    struct lmp_chan test;
    lmp_chan_init (&test);
    lmp_chan_accept (&test, DEFAULT_LMP_BUF_WORDS, test_thread);
    lmp_ep_send1 (test_thread, LMP_SEND_FLAGS_DEFAULT, test.local_cap, AOS_PING);
    error = lmp_chan_register_recv (&test, get_default_waitset(), MKCLOSURE (test_handler, &test));
    error = event_dispatch (get_default_waitset());
    debug_printf ("aos_find_service: %s\n", err_getstring (error));
}
