#include <barrelfish/barrelfish.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <barrelfish/aos_rpc.h>


extern size_t (*_libc_terminal_read_func)(char *, size_t);
extern size_t (*_libc_terminal_write_func)(const char *, size_t);

#define BUFSIZE (4UL*1024*1024)

static struct aos_rpc* serial_channel;
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

int main(int argc, char *argv[])
{
    debug_printf("memtest started\n");

    struct capref serial_ep;
    aos_find_service (aos_service_serial, &serial_ep);
    serial_channel = malloc (sizeof (struct aos_rpc));
    aos_rpc_init (serial_channel, serial_ep);

    // TODO: actually we should do this way earlier, in lib/barrelfish/init.c
    _libc_terminal_read_func = aos_rpc_terminal_read;
    _libc_terminal_write_func = aos_rpc_terminal_write;

    char* mbuf = malloc (BUFSIZE);
    for (int mi=0; mi<BUFSIZE; mi++) {
        mbuf[mi] = mi % 255;
    }
    free (mbuf);

    debug_printf ("memtest returned\n");
    return 0;
}
