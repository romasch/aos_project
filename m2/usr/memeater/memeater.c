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

extern struct lmp_chan bootstrap_channel;


static struct aos_rpc arpc;

static bool starts_with(const char *prefix, const char *str)
{
    uint32_t lenpre = strlen(prefix);

    return (( strlen(str) < lenpre ) ? ( false ) : ( strncmp(prefix, str, lenpre) == 0 ));
}

static size_t aos_rpc_terminal_write(const char *buf, size_t len)
{
    for (int i=0; i<len; i++) {
        aos_rpc_serial_putchar (&arpc, buf[i]);
    }
    return 0;
}

static size_t aos_rpc_terminal_read (char *buf, size_t len)
{
    // probably scanf always only wants to read one character anyway...
    int i = 0;
    char c;
    do {
        aos_rpc_serial_getchar (&arpc, &c);
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
            aos_rpc_serial_getchar(&arpc, &buf[i]);
            
            if (buf[i] == '\r') {
                finished = true; 

                // We don't want to see 'CRNL' line ending
                buf[i] = '\n';

                // Reflect character back to the terminal to 
                // let the user see what he typing.
                aos_rpc_serial_putchar(&arpc, buf[i]);       
            } else {     
                aos_rpc_serial_putchar(&arpc, buf[i]);       
            }
        }
        
        // Start statement processing.
        if (starts_with("echo", buf) != false) {
            printf ("echo: %s", &buf[5]);
        } else if (starts_with("run_memtest ", buf) != false) {
            number = atoi(&buf[12]); 
            
            if (number > 0) {
                debug_printf ("Running memtest.\n");
                char* mbuf = malloc (BUFSIZE);
                for (int mi=0; mi<BUFSIZE; mi++) {
                    mbuf[mi] = mi % 255;
                }
                free (mbuf);
                debug_printf ("Memtest finished.\n");
            }
        } else if (start_with ("exit", buf) != false) {
            break;
        } else {
            // ignore
        }
    }
}

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    errval_t error = SYS_ERR_OK;

    // TODO STEP 1: connect & send msg to init using syscall
    uint32_t flags = LMP_FLAG_SYNC | LMP_FLAG_YIELD;

    //Test sending a basic message.
    error = lmp_ep_send1 (cap_initep, flags, NULL_CAP, 42);
    debug_printf ("Send message: %s\n", err_getstring (error));

    //Test sending a capability.
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));


    // Test opening another channel.
    // NOTE: A first channel is already created to talk to RAM server.

    error = aos_rpc_init(&arpc, cap_initep);
    if (err_is_fail (error)) {
        debug_printf ("Error! in aos_rpc_init(0x%x) = %u", 0U, error);
    }

    aos_ping (&bootstrap_channel, 46);
    aos_ping (&arpc.channel, 42);
    aos_ping (&arpc.channel, 43);
    aos_ping (&arpc.channel, 44);
    aos_ping (&arpc.channel, 45);
    aos_ping (&bootstrap_channel, 47);
    aos_ping (&bootstrap_channel, 46);
    aos_ping (&bootstrap_channel, 48);

//    error = aos_find_service (aos_service_ram, &received);
//    debug_printf ("find_service: %s\n", err_getstring (error));


    // NOTE: debug_printf probably has a size restriction, therefore we should use printf.
//     debug_printf ("Send message: ");
//     printf ("%s\n", "Very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very long test string");
    error = aos_rpc_send_string(&arpc, "Very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very long test string");
    
    if (err_is_fail (error)) {
        debug_printf ("Error! in aos_rpc_send_string(0x%x, %s) = %u", &arpc, "Test string", error);
    }

    // TODO STEP 5: test memory allocation using memserv
    char* buf = malloc (BUFSIZE);
    debug_printf ("Buffer allocated.\n");
    for (int i=0; i<BUFSIZE; i++) {
        buf[i] = i % 255;
    }
    debug_printf ("Buffer filled.\n");

    aos_rpc_serial_putchar (&arpc, '+');
    char c [3] = {0, '\n',0};
    aos_rpc_serial_getchar (&arpc, c);
    debug_printf (c);

    // NOTE: actually we should do this way earlier, in lib/barrelfish/init.c
    _libc_terminal_read_func = aos_rpc_terminal_read;
    _libc_terminal_write_func = aos_rpc_terminal_write;

    start_shell ();

    debug_printf ("memeater returned\n");
    return 0;
}
