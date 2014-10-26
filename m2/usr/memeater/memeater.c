#include <barrelfish/barrelfish.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/lmp_chan.h>

#define BUFSIZE (128UL*1024*1024)

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");
    // TODO STEP 1: connect & send msg to init using syscall

    debug_printf ("cap_initep: %u, %u\n", cap_initep.cnode.address, cap_initep.slot);

//    uint32_t flags = 0;
    uint32_t flags = LMP_FLAG_SYNC | LMP_FLAG_YIELD;
    
    // Test sending a basic message.
    errval_t error = lmp_ep_send1 (cap_initep, flags, NULL_CAP, 42);
    debug_printf ("Send message: %s\n", err_getstring (error));

    // Test sending a capability.
    error = lmp_ep_send1 (cap_initep, flags, cap_selfep, 43);
    debug_printf ("Send message: %s\n", err_getstring (error));

    // TODO STEP 5: test memory allocation using memserv
    return 0;
}
