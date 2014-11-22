#include <barrelfish/barrelfish.h>
#include <ctype.h>
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
static struct aos_rpc* test_channel  ;
static struct aos_rpc* pm_channel    ;
static struct aos_rpc* led_channel;

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

static void test_routing_to_domain (void);

static void run_memtest(const char* const buf)
{
    int number = atoi(&buf[12]); 
            
    if (number > 0) {
        debug_printf ("Running memtest.\n");
        char* mbuf = malloc (number);
        for (int mi=0; mi<number; mi++) {
            mbuf[mi] = mi % 255;
        }
        free (mbuf);
        debug_printf ("Memtest finished.\n");
    }
}

static void print_process_list(void)
{
    size_t      count;
    errval_t    err  ;
    domainid_t* pids ;

    printf(" PID\t\tName\n");

    err = aos_rpc_process_get_all_pids(pm_channel, &pids, &count);
    if (err_is_fail (err)) {
        printf("\t ERROR: Failed to get a process list from the Process Manager (0x%x).\n", err);
    } else {
        printf("=================================\n");

        for (int i = 0; i < count; i++) {
            char* name = NULL;

            err = aos_rpc_process_get_name(pm_channel, pids[i], &name);
            if (err_is_fail(err)) {
                printf(" %u\t\t---- Unknown process ----\n", pids[i]      );
            } else {
                printf(" %u\t\t%s\n"                       , pids[i], name);

                free(name);
            }
        }
        
        printf("---------------------------------\n");

        free(pids);
    }
}

static void execute_external_command(char* const cmd)
{
    errval_t err    ; 
    domainid_t  pid  ;
    bool     success = false;

    bool background = false;

    for (int i=0; cmd[i] != '\0'; i++) {
        if (cmd[i] == '&') {
            background = true;
        }
    }

    for (int i = 0; cmd[i] != '\0'; i++) {
        if ((isalnum((int)cmd[i]) == false) && (cmd[i] != '-') && (cmd[i] != '_')) {
            cmd[i] = '\0';
            i--;
        }
    }
    
    err = aos_rpc_process_spawn(pm_channel, cmd, &pid);
    if (err_is_fail (err) == false) {
        success = true;
    }

    if (!background && success) { // not background
        aos_rpc_set_foreground (serial_channel, pid);
        aos_rpc_wait_for_termination (pm_channel, pid);
        aos_rpc_set_foreground (serial_channel, disp_get_domain_id());
    }


    if (success == false) {
        printf ("Unknown command.\n");
    }
}

/**
 * A simple shell that handles echo, run_memtest and exit commands.
 */
static void start_shell (void)
{
    char buf[256];
        
    debug_printf ("Started simple shell...\n");
    
    test_routing_to_domain();

    while (true) {
        bool finished = false;
        int  i        = 0    ;
        aos_rpc_serial_putchar (serial_channel, '$');

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
        if (       starts_with("echo"        , buf) != false) {
            printf ("echo: %s", &buf[5]);
        } else if (starts_with("run_memtest ", buf) != false) {
            run_memtest(buf);
        } else if (starts_with ("exit"       , buf) != false) {
            break;
        } else if (starts_with ("ledon"      , buf) != false) {
            aos_rpc_set_led (led_channel, true);
        } else if (starts_with ("ledoff"     , buf) != false) {
            aos_rpc_set_led (led_channel, false);
        } else if (starts_with ("ps"         , buf) != false) {
            print_process_list();
        } else if (starts_with ("kill ", buf)) {
            int number = atoi(&buf[5]);
            aos_rpc_kill (pm_channel, number);
        } else if (starts_with ("ping", buf)) {
            test_routing_to_domain();
        } else {
            execute_external_command(&buf[0]);
        }
    }
}

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");
    pm_channel     = aos_rpc_get_init_channel ();
    serial_channel = aos_rpc_get_init_channel ();
    test_channel   = aos_rpc_get_init_channel ();
    led_channel = aos_rpc_get_init_channel ();

    struct capref serial_ep;
    aos_find_service (aos_service_serial, &serial_ep);
    serial_channel = malloc (sizeof (struct aos_rpc));
    aos_rpc_init (serial_channel, serial_ep);
    aos_rpc_set_foreground (serial_channel, disp_get_domain_id());


    errval_t error = SYS_ERR_OK;

/*    // STEP 1: connect & send msg to init using syscall
    uint32_t flags = LMP_FLAG_SYNC | LMP_FLAG_YIELD;
    //Test sending a basic message.
    error = lmp_ep_send1 (cap_initep, flags, NULL_CAP, 42);
    debug_printf ("Send message: %s\n", err_getstring (error));

    //Test sending a capability.
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));
*/
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

    ///////////////////////////////////////////////////
    debug_printf ("\t Test process management API \n");

    size_t      count;
    char      * name ;
    domainid_t  pid  ;
    domainid_t* pids ;
    
    error = aos_rpc_process_spawn(pm_channel, "test_domain", &pid);
    if (err_is_fail (error)) {
        debug_printf ("\t\t (X) AOS_RPC_SPAWN_PROCESS   \n");
    } else {
        debug_printf ("\t\t (V) AOS_RPC_SPAWN_PROCESS   \n");
        debug_printf ("\t\t\t Pid: 0x%x\n", pid);
    }

    error = aos_rpc_process_get_name(pm_channel, pid, &name);
    if (err_is_fail (error)) {
        debug_printf ("\t\t (X) AOS_RPC_GET_PROCESS_NAME\n");
    } else {
        debug_printf ("\t\t (V) AOS_RPC_GET_PROCESS_NAME\n");
        debug_printf ("\t\t\t Name: %s\n", name);

        free(name);
    }

    error = aos_rpc_process_get_all_pids(pm_channel, &pids, &count);
    if (err_is_fail (error)) {
        debug_printf ("\t\t (X) AOS_RPC_GET_PROCESS_LIST\n");
    } else {
        debug_printf ("\t\t (V) AOS_RPC_GET_PROCESS_LIST\n");
        debug_printf ("\t\t\t Pid list: 0x%x {0x%x}\n", count, pids[0]);

        free(pids);
    }
    
    debug_printf ("\t Test of process management API finished\n");
    ///////////////////////////////////////////////////

    // TODO: actually we should do this way earlier, in lib/barrelfish/init.c
    _libc_terminal_read_func = aos_rpc_terminal_read;
    _libc_terminal_write_func = aos_rpc_terminal_write;

    start_shell ();

    //test_routing_to_domain();

    debug_printf ("memeater returned\n");
    return 0;
}


// Connect to the test domain and send a message.
static void test_routing_to_domain (void)
{   
    // Test routing
    //for (bool success = false; success == false;) {
    for (int i = 0; i < 100; i++) {
        errval_t error;
        struct capref test_domain;
        error = aos_find_service (aos_service_test, &test_domain);
        if (!err_is_fail (error)) {
            struct aos_rpc test_domain_chan;

            error = aos_rpc_init (&test_domain_chan, test_domain);

            error = aos_ping (&test_domain_chan, 42);
            i = 100; // exit
        } else {
            // Try again in hope of quick test_domain registration in the system
        }
    }
}
