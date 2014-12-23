#include <barrelfish/barrelfish.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>

#define DEFAULT_SIZE 48ul
#define MEGABYTE (1024*1024)

int main(int argc, char *argv[])
{
    printf ("memtest started with %u arguments: ", argc);
    for (int i=0; i<argc; i++) {
        printf ("%s ", argv[i]);
    }
    printf ("\n");

    int32_t size = 0;

    if (argc >= 2) {
        size = atoi (argv [1]);
    }
    if (size <= 0) {
        printf ("Invalid buffer size. Using default value.\n");
        size = DEFAULT_SIZE;
    }

    printf ("memtest: Allocating buffer of %u MB.\n", size);
    size = size * MEGABYTE;
    char* mbuf = malloc (size);
    
    for (int mi=0; mi<size; mi++) {
        mbuf[mi] = mi % 255;
    }

    printf ("memtest: buffer filled.\n");

    free (mbuf);

    printf ("memtest returned\n");
    return 0;
}
