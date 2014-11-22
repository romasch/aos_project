#include <barrelfish/barrelfish.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>

#define BUFSIZE (48UL*1024*1024)

int main(int argc, char *argv[])
{
    printf ("memtest started as: %s\n", argv[0]);

    char* mbuf = malloc (BUFSIZE);

    printf ("memtest: buffer allocated.\n");
    
    for (int mi=0; mi<BUFSIZE; mi++) {
        mbuf[mi] = mi % 255;
    }

    printf ("memtest: buffer filled.\n");

    free (mbuf);

    printf ("memtest returned\n");
    return 0;
}
