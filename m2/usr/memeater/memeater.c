#include <barrelfish/barrelfish.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>
#include <barrelfish/lmp_chan.h>

#define MAX_PATH 1024
#define COUNT_OF(x) (sizeof(x) / sizeof(x[0])) 

typedef void (*command_handler)(char* const args);

struct command_desc
{
    const command_handler        handler;
    const char           * const name   ;
};

static struct aos_rpc* filesystem_channel;
static struct aos_rpc*        led_channel;
static struct aos_rpc*         pm_channel;
static struct aos_rpc*     serial_channel;

static char     current_dir[MAX_PATH];
static uint32_t current_len          ;

static bool shell_running;

// Set of command executors
static void exec_cat        (char* const args);
static void exec_cd         (char* const args);
static void exec_echo       (char* const args);
static void exec_exit       (char* const args);
static void exec_kill       (char* const args);
static void exec_leadoff    (char* const args);
static void exec_leadon     (char* const args);
static void exec_ls         (char* const args);
static void exec_oncore     (char* const args);
static void exec_ping       (char* const args);
static void exec_ps         (char* const args);
static void exec_run_memtest(char* const args);
static void exec_test_string(char* const args);

// Set of routines for AOS RPC API testing
static void test_fs_api(void);
static void test_pm_api(void);

static struct command_desc command_list[] =
{
    { exec_cat        , "cat"         },
    { exec_cd         , "cd"          },
    { exec_echo       , "echo"        },
    { exec_exit       , "exit"        },
    { exec_kill       , "kill"        },
    { exec_leadoff    , "leadoff"     },
    { exec_leadon     , "leadon"      },
    { exec_ls         , "ls"          },
    { exec_oncore     , "oncore"      },
    { exec_ping       , "ping"        },
    { exec_ps         , "ps"          },
    { exec_run_memtest, "run_memtest" },
    { exec_test_string, "test_string" }
};

/** 
  * Collect input characters in the string buffer until the moment
  * when we will have 'carriage return' character.
  * Then we will consider it as a single statement.
**/
static void receive_request(char* request, const size_t max_length)
{
    for (int i = 0; i < max_length - 1; i++) {
        aos_rpc_serial_getchar(serial_channel, &request[i]);

        if ((request[i] == '\r')) {
            request[i] = '\0'      ; // Terminate line
            i          = max_length; // Terminate cycle

            // Reflect character back to the terminal to
            // let the user see what he typing.
            aos_rpc_serial_putchar(serial_channel, '\n'      );
        } else {
            // Reflect character back to the terminal to
            // let the user see what he typing.
            aos_rpc_serial_putchar(serial_channel, request[i]);
        }
    }
}

/**
  * Splits request string received into two parts - 'cammand' and 'arguments'.
  * Command always starts from the first character of request.
  * Pointer to arguments string returned to caller.
**/
static char* split_request(char* request)
{
    for (; (isspace((int)*request) == false) && (*request != '\0'); request++);
    
    if (*request != '\0') {
        *request = '\0';
        
        for (request++; (isspace((int)*request) != false) && (*request != '\0'); request++);
    }

    return request;
}

static void exec_external_command(char* const command, char* const args)
{
    bool       background = false;
    errval_t   err       ; 
    domainid_t pid       ;
    bool       success    = false;

    if (args[strlen(args) - 1U] == '&') {
        background              = true;    
        args[strlen(args) - 1U] = '\0';
    } 
    
    for (int i = 0; command[i] != '\0'; i++) {
        if ((isalnum((int)command[i]) == false) && (command[i] != '-') && (command[i] != '_')) {
            command[i] = '\0';
            i--;
        }
    }
    
    err     = aos_rpc_process_spawn (pm_channel, command, 0, &pid);
    success = err_is_ok             (err                         );

    if (!background && success) { // not background
        aos_rpc_set_foreground       (serial_channel, pid                 );
        aos_rpc_wait_for_termination (pm_channel    , pid                 );
        aos_rpc_set_foreground       (serial_channel, disp_get_domain_id());
    }

    if (success == false) {
        printf ("Unknown command.\n");
    }
}

static void process_path_entry(const char* const entry)
{
    if (strcmp(entry, ".") == 0) {
        // skip
    } else if (strcmp(entry, "..") == 0) {
        if (current_len != 1) {
            // Remove ending '/' character
            current_len--;
            current_dir[current_len] = '\0';

            // Remove last current path entry
            for (; (current_len != 1) && (current_dir[current_len - 1] != '/');) {
                current_len--;
                current_dir[current_len] = '\0';
            }
        }
    } else {
        strcat(current_dir, entry);
        strcat(current_dir, "/"  );
        
        current_len = strlen(current_dir);
    }
}

static void exec_cat(char* const args)
{
    if (*args != '\0') {
        errval_t error;
        int      file ;

        strcat(current_dir, args);

        error = aos_rpc_open(filesystem_channel, current_dir, &file);
        if (!err_is_ok(error)) {
            printf ("File does not exist!\n");
        } else {
            void    * chunk    = NULL;
            uint32_t  chunk_id = 0   ;

            // TODO: We shouldn't split the messages here. Instead it should be done in aos_rpc_read().
            #define BUFFER_SIZE (1<<20)
            size_t chunk_len = BUFFER_SIZE;


            for (; (chunk_len == BUFFER_SIZE) && err_is_ok(error) ; chunk_id++){
                error = aos_rpc_read(filesystem_channel, file, chunk_id * BUFFER_SIZE, BUFFER_SIZE, &chunk, &chunk_len);

                for (int i=0; err_is_ok (error) && i < chunk_len; i++) {
                    error = aos_rpc_serial_putchar (serial_channel, ((char*) chunk) [i]);
                }
            }
            aos_rpc_serial_putchar (serial_channel, '\n');

            if (!err_is_ok(error)) {
                printf ("Error during reading!\n");    
            }

            aos_rpc_close(filesystem_channel, file);
        }

        current_dir[current_len] = '\0';
    } else {
        printf ("There is no argument required!\n");
    } 
}

static void exec_cd(char* const args)
{
    if (*args == '\0') {
        current_dir[1] = '\0';
        current_len    =   1 ;
    } else {
        if (*args == '/') {
            strcpy(current_dir, args);
            
            current_len = strlen(current_dir);
        } else {
            char* args_str = args;
            char* start    = args;
    
            for (; *args_str != '\0' ; args_str++) {
                if (*args_str == '/') {
                    *args_str = '\0';

                    process_path_entry(start);

                    start = args_str + 1;
                }
            }

            process_path_entry(start);
        }
    }
}

static void exec_echo(char* const args)
{
    printf("echo: %s\n", args);
}

static void exec_exit(char* const args)
{
    shell_running = false;
}

static void exec_kill(char* const args)
{
    int number = atoi(args);
                
    if (number > 0) {
        aos_rpc_kill(pm_channel, number);
    }
}

static void exec_leadoff(char* const args)
{
    aos_rpc_set_led (led_channel, false);
}

static void exec_leadon(char* const args)
{
    aos_rpc_set_led (led_channel, true);
}

static void exec_ls(char* const args)
{
    struct aos_dirent* dir       ;
    size_t             elem_count;
    errval_t           error     ;

    error = aos_rpc_readdir(filesystem_channel, current_dir, &dir, &elem_count);
    if (err_is_ok(error)) {
        printf (    "            Size | Name\n");
        printf ("===========================\n");

        for (int i = 0; i < elem_count; i++) {
            if (dir[i].size == 0) {
                printf ("\t           %s\n", dir[i].name);
            }
        }
        
        for (int i = 0; i < elem_count; i++) {
            if (dir[i].size != 0) {
                printf ("\t%8u | %s\n", dir[i].size, dir[i].name);
            }
        }

        free(dir);
    } else {
        printf ("Invalid directory!\n");
    }
}

static void exec_oncore(char* const args)
{
    char    * args_str = args;
    coreid_t  core    ;
    
    for (; (isdigit((int)*args_str) != false) && (*args_str != '\0'); args_str++);
    
    *args_str = '\0';
    args_str++;
    
    for (; (isalnum((int)*args_str) == false) && (*args_str != '-') && (*args_str != '_') && (*args_str != '\0'); args_str++);
    
    core = atoi(args);
    
    if (core == 0) {
        exec_external_command(args_str, split_request(args_str));
    } else {
        domainid_t newpid;

        aos_rpc_process_spawn(pm_channel, args_str, core, &newpid);
    }
}

static void exec_ping(char* const args)
{
    for (int i = 0; i < 100; i++) {
        struct capref test_domain;
        
        errval_t error = aos_find_service (aos_service_test, &test_domain);

        if (err_is_ok(error)) {
            struct aos_rpc test_domain_chan;

            error = aos_rpc_init (&test_domain_chan, test_domain);
            error = aos_ping     (&test_domain_chan, 42         );
            i = 100; // exit
        } else {
            // Try again in hope of quick test_domain registration in the system
        }
    }
}

static void exec_ps(char* const args)
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

static void exec_run_memtest(char* const args)
{
    int number = atoi(args); 
            
    if (number > 0) {
        char* mbuf;
        
        printf ("Running memtest.\n");

        mbuf = malloc (number);
        
        for (int mi=0; mi<number; mi++) { 
            mbuf[mi] = mi % 255;
        }
        free (mbuf);
        
        printf ("Memtest finished.\n");
    }
}

static void exec_test_string(char* const args)
{
    errval_t error = aos_rpc_send_string 
    (
        serial_channel, 
        "Very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very very very very very very very very very very very very very very very very "
        "very long test string\n"
    );
    
    if (err_is_ok(error)) {
        printf("Test passed successfully.\n");
    } else {
        printf("Test failed.             \n");
    }
}

/**
 * A simple shell that handles echo, run_memtest and exit commands.
 */
static void start_shell (void)
{
    char command[256];
    bool handled      = false;

    printf ("Started simple shell...\n");

    shell_running = true;
    for (; shell_running != false ;) {
        char* arguments;

        handled = false;

        aos_rpc_serial_putchar(serial_channel, '$');
        
                    receive_request(command, COUNT_OF(command));
        arguments =   split_request(command                   );
        
        for (int i = 0; (i < COUNT_OF(command_list)) && (handled == false); i++) {
            if (strcmp(command, command_list[i].name) == 0) {
                command_list[i].handler(arguments);

                i       = COUNT_OF(command_list);
                handled = true                  ; 
            }
        }

        if (handled == false) {
            exec_external_command(command, arguments);
        }
    }
}

int main(int argc, char *argv[])
{
    errval_t error;
    
    led_channel    = aos_rpc_get_init_channel          ();
    pm_channel     = aos_rpc_get_init_channel          ();
    serial_channel = aos_rpc_get_serial_driver_channel ();

    aos_rpc_set_foreground (serial_channel, disp_get_domain_id());

    printf("memeater started\n");

    filesystem_channel = malloc (sizeof (struct aos_rpc));
    if (filesystem_channel) {
        struct capref fs_cap;

        error = aos_find_service (aos_service_filesystem, &fs_cap); assert(err_is_ok(error));
        error = aos_rpc_init     (filesystem_channel    ,  fs_cap); assert(err_is_ok(error));
        error = aos_ping         (filesystem_channel    ,      42); assert(err_is_ok(error));
    }

    current_dir[0] = '/' ;
    current_dir[1] = '\0';
    current_len    = 1   ;

    start_shell ();

    printf ("memeater returned\n");

    return 0;
}

// Test API of fs.
__attribute__((unused))
static void test_fs_api(void)
{
    struct aos_dirent* dir;
    
    void  *  buf       ;
    size_t   buf_len   ;
    size_t   elem_count;
    errval_t error     ;
    int      file       = -1;

    // Test open() rpc call
    error = aos_rpc_open(filesystem_channel, "/testdir/hello.txt"                         , NULL ); assert((error == -1) && (file == -1));
    error = aos_rpc_open(filesystem_channel, "/veryveryveryveryveryverylongpath/hello.txt", &file); assert((error == -1) && (file == -1));
    error = aos_rpc_open(filesystem_channel, "/testdir/hello.txt"                         , &file); assert((error == 0 ) && (file != -1));
    // Test read() rpc call
    error = aos_rpc_read(filesystem_channel, file, 0, 20, &buf, &buf_len); assert (error == 0 );
    if (error == 0) {
        char str[20];
        
        memset(str,   0, sizeof(str));
        memcpy(str, buf,     buf_len);

        free(buf);
    }
    // Test close() rpc call
    error = aos_rpc_close(filesystem_channel, file); assert(error ==  0);
    error = aos_rpc_close(NULL              , file); assert(error == -1);
    // Test readdir() rpc call
    error = aos_rpc_readdir(filesystem_channel,  "/", &dir, &elem_count); 
    if (error == 0) {
        debug_printf ("Directory content:\n");

        for (int i = 0; i < elem_count; i++) {
            debug_printf ("\t%s\n", dir[i].name);
        }

        free(dir);
    } else {
        debug_printf ("ERR: aos_rpc_readdir() failed!\n");
    }
}

// Test API of pm.
__attribute__((unused))
static void test_pm_api (void)
{
    debug_printf ("\t Test process management API \n");

    size_t      count;
    errval_t    error;
    char      * name ;
    domainid_t  pid  ;
    domainid_t* pids ;

    error = aos_rpc_process_spawn(pm_channel, "test_domain", 0, &pid);
    if (err_is_fail (error)) {
        debug_printf ("\t\t (X) AOS_RPC_SPAWN_PROCESS   \n");
    } else {
        debug_printf ("\t\t (V) AOS_RPC_SPAWN_PROCESS   \n");
        debug_printf ("\t\t\t Pid: 0x%x\n", pid            );
    }

    error = aos_rpc_process_get_name(pm_channel, pid, &name);
    if (err_is_fail (error)) {
        debug_printf ("\t\t (X) AOS_RPC_GET_PROCESS_NAME\n");
    } else {
        debug_printf ("\t\t (V) AOS_RPC_GET_PROCESS_NAME\n");
        debug_printf ("\t\t\t Name: %s\n", name            );

        free(name);
    }

    error = aos_rpc_process_get_all_pids(pm_channel, &pids, &count);
    if (err_is_fail (error)) {
        debug_printf ("\t\t (X) AOS_RPC_GET_PROCESS_LIST\n"           );
    } else {
        debug_printf ("\t\t (V) AOS_RPC_GET_PROCESS_LIST\n"           );
        debug_printf ("\t\t\t Pid list: 0x%x {0x%x}\n", count, pids[0]);

        free(pids);
    }

    debug_printf ("\t Test of process management API finished\n");
}

