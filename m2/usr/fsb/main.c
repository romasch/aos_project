#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/lmp_chan.h>
#include <omap_timer/timer.h>

#define BUFSIZE (1UL*1024*1024)

static struct aos_rpc* filesystem_channel;

static inline void print_error (errval_t error, char* fmt, ...)
{
    if (err_is_fail (error)) {
        debug_printf (""); // print domain id
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
}

static errval_t evaluate_fs(const int file)
{
    char    * chunk     =       NULL;
    uint32_t  chunk_id  =          0;
    size_t    chunk_len =    BUFSIZE;
    uint64_t  elapsed  ;
    errval_t  error     = SYS_ERR_OK;
    uint32_t  size      =          0;
    uint64_t  start    ;

    omap_timer_init(    );
    omap_timer_ctrl(true);
                        
    start = omap_timer_read();

    // START
    for (; (chunk_len == BUFSIZE) && err_is_ok(error) ; chunk_id++){
        debug_printf("ping\n");
        error = aos_rpc_read(filesystem_channel, file, chunk_id * BUFSIZE, BUFSIZE, (void**)&chunk, &chunk_len);
        if (err_is_ok(error)) {
            size += chunk_len;
            free(chunk);
        }
    }
    // FINISH
    
    elapsed = omap_timer_read() - start;

    if (err_is_ok(error)) {
        printf ("Elapsed time: %llu\n", elapsed);
        printf ("File size   : %u\n"  , size   );
    } else {
        printf ("Error during reading!\n");    
    }

    return error;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("There is an argument required - path to the test file\n");
    } else {
        errval_t error = -1;

        printf ("Filesystem benchmark: %s\n", argv[1]);

        filesystem_channel = malloc (sizeof (struct aos_rpc));
        if (filesystem_channel) {
            struct capref fs_cap;

            error = aos_find_service (aos_service_filesystem, &fs_cap);
            print_error (error, "fsbench: aos_find_service failed. %s\n", err_getstring (error));
            if (err_is_ok(error)) {
                error = aos_rpc_init (filesystem_channel, fs_cap);
                print_error (error, "fsbench: aos_rpc_init failed. %s\n", err_getstring (error));
                if (err_is_ok(error)) { 
                    error = aos_ping (filesystem_channel, 42);
                    print_error (error, "fsbench: aos_ping failed. %s\n", err_getstring (error));
                    if (err_is_ok(error)) {
                        int      file ;

                        error = aos_rpc_open(filesystem_channel, argv[1], &file);
                        print_error (error, "fsbench: aos_rpc_open failed. %s\n", err_getstring (error));
                        if (err_is_ok(error)) {
                            error = evaluate_fs(file);
                        
                            aos_rpc_close(filesystem_channel, file);
                        }
                    }
                }
            }
        }
    }

    printf ("FS Benchmark finished\n");

    return 0;
}
