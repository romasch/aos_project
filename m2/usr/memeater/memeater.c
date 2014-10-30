#include <barrelfish/barrelfish.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>
#include <barrelfish/lmp_chan.h>

//#define BUFSIZE (128UL*1024*1024)
#define BUFSIZE (48UL*1024*1024)

extern struct lmp_chan bootstrap_channel;

int main(int argc, char *argv[])
{
    struct aos_rpc arpc;

    debug_printf("memeater started\n");
    errval_t error = SYS_ERR_OK;


//    struct capref received;
//    error = aos_find_service (aos_service_ram, &received);
//    debug_printf ("find_service: %s\n", err_getstring (error));



    // TODO STEP 1: connect & send msg to init using syscall

//    debug_printf ("cap_initep: %u, %u\n", cap_initep.cnode.address, cap_initep.slot);

//    uint32_t flags = 0;
//     uint32_t flags = LMP_FLAG_SYNC | LMP_FLAG_YIELD;

    // Test sending a basic message.
//    errval_t error = lmp_ep_send1 (cap_initep, flags, NULL_CAP, 42);
//    debug_printf ("Send message: %s\n", err_getstring (error));

    // Test sending a capability.
//     error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
//     debug_printf ("Send message: %s\n", err_getstring (error));
//     error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
//     debug_printf ("Send message: %s\n", err_getstring (error));

    error = aos_rpc_init(&arpc, cap_initep);
    if (error != SYS_ERR_OK) {
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

    debug_printf ("blub\n");
//    error = aos_find_service (aos_service_ram, &received);
    debug_printf ("find_service: %s\n", err_getstring (error));


    // NOTE: debug_printf probably has a size restriction, therefore we should use printf.
//     debug_printf ("Send message: ");
//     printf ("%s\n", "Very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very long test string");
//     error = aos_rpc_send_string(&arpc, "Very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very very long test string");
    
    if (error != SYS_ERR_OK) {
        debug_printf ("Error! in aos_rpc_send_string(0x%x, %s) = %u", &arpc, "Test string", error);
    }

    // TODO STEP 5: test memory allocation using memserv

    char* buf = malloc (BUFSIZE);
    debug_printf ("Buffer allocated.\n");
    for (int i=0; i<BUFSIZE; i++) {
        buf[i] = i % 255;
    }
    debug_printf ("Buffer filled.\n");

    debug_printf ("memeater returned\n");
    return 0;
}
