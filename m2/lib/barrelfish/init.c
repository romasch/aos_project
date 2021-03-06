/**
 * \file
 * \brief Barrelfish library initialization.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011, 2012, 2013 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/idc.h>
#include <barrelfish/dispatch.h>
#include <barrelfish/curdispatcher_arch.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish_kpi/dispatcher_shared.h>
#include <barrelfish/morecore.h>
#include <barrelfish/monitor_client.h>
#include <barrelfish/nameservice_client.h>
#include <barrelfish/paging.h>
#include <barrelfish_kpi/domain_params.h>
#include <if/monitor_defs.h>
#include <trace/trace.h>
#include <octopus/init.h>
#include "threads_priv.h"
#include "init.h"

#include <barrelfish/aos_rpc.h>
// #include <barrelfish/lmp_chan.h>

/// Are we the init domain (and thus need to take some special paths)?
static bool init_domain;

extern size_t (*_libc_terminal_read_func)(char *, size_t);
extern size_t (*_libc_terminal_write_func)(const char *, size_t);
extern void (*_libc_exit_func)(int);
extern void (*_libc_assert_func)(const char *, const char *, const char *, int);

void libc_exit(int);

void libc_exit(int status)
{
    // Use spawnd if spawned through spawnd
    if(disp_get_domain_id() == 0) {
        errval_t err = cap_revoke(cap_dispatcher);
        if (err_is_fail(err)) {
            sys_print("revoking dispatcher failed in _Exit, spinning!", 100);
            while (1) {}
        }
        err = cap_delete(cap_dispatcher);
        sys_print("deleting dispatcher failed in _Exit, spinning!", 100);

        // XXX: Leak all other domain allocations
    } else {
        aos_rpc_exit (aos_rpc_get_init_channel());
        debug_printf("Error: aos_rpc_exit returned\n");
    }

    // If we're not dead by now, we wait
    while (1) {}
}

static void libc_assert(const char *expression, const char *file,
                        const char *function, int line)
{
    char buf[512];
    size_t len;

    /* Formatting as per suggestion in C99 spec 7.2.1.1 */
    len = snprintf(buf, sizeof(buf), "Assertion failed on core %d in %.*s: %s,"
                   " function %s, file %s, line %d.\n",
                   disp_get_core_id(), DISP_NAME_LEN,
                   disp_name(), expression, function, file, line);
    sys_print(buf, len < sizeof(buf) ? len : sizeof(buf));
}

static size_t syscall_terminal_write(const char *buf, size_t len)
{
    if (len) {
        return sys_print(buf, len);
    }
    return 0;
}

static size_t dummy_terminal_read(char *buf, size_t len)
{
    debug_printf("terminal read NYI! returning %d characters read\n", len);
    return len;
}


/// Write function for the serial driver.
static size_t aos_rpc_terminal_write(const char *buf, size_t len)
{
    for (int i=0; i<len; i++) {
        aos_rpc_serial_putchar (aos_rpc_get_serial_driver_channel (), buf[i]);
    }
    return 0;
}

/// Read function for the serial driver.
static size_t aos_rpc_terminal_read (char *buf, size_t len)
{
    // probably scanf always only wants to read one character anyway...
    int i = 0;
    char c;
    do {
        //TODO: error handling.
        aos_rpc_serial_getchar (aos_rpc_get_serial_driver_channel (), &c);
        buf [i] = c;
        i++;
    } while (c != '\n' && c != '\r' && i+1 < len);
    return i;
}


/* Set libc function pointers */
void barrelfish_libc_glue_init(void)
{
    // TODO: change these to use the user-space serial driver if possible
    _libc_terminal_read_func = dummy_terminal_read;
    _libc_terminal_write_func = syscall_terminal_write;
    _libc_exit_func = libc_exit;
    _libc_assert_func = libc_assert;
    /* morecore func is setup by morecore_init() */

    // XXX: set a static buffer for stdout
    // this avoids an implicit call to malloc() on the first printf
    static char buf[BUFSIZ];
    setvbuf(stdout, buf, _IOLBF, sizeof(buf));
    static char ebuf[BUFSIZ];
    setvbuf(stderr, ebuf, _IOLBF, sizeof(buf));
}

#if 0
static bool register_w_init_done = false;
static void init_recv_handler(struct aos_chan *ac, struct lmp_recv_msg *msg, struct capref cap)
{
    assert(ac == get_init_chan());
    debug_printf("in libbf init_recv_handler\n");
    register_w_init_done = true;
}
#endif

// RAM allocation function over IPC.
static struct aos_rpc* ram_server_connection;
static errval_t ram_alloc_ipc (struct capref *ret, uint8_t size_bits, uint64_t minbase, uint64_t maxlimit)
{
    size_t ret_bits; // TODO: handle this...
    errval_t error = aos_rpc_get_ram_cap (ram_server_connection, size_bits, ret, &ret_bits);
    // DBG: Uncomment if you really need it ==> debug_printf ("Allocating %u bits: %s\n", size_bits, err_getstring (error));
    return error;
}

/** \brief Initialise libbarrelfish.
 *
 * This runs on a thread in every domain, after the dispatcher is setup but
 * before main() runs.
 */
errval_t barrelfish_init_onthread(struct spawn_domain_params *params)
{
    errval_t err;

    // do we have an environment?
    if (params != NULL && params->envp[0] != NULL) {
        extern char **environ;
        environ = params->envp;
    }

    // Init default waitset for this dispatcher
    struct waitset *default_ws = get_default_waitset();
    waitset_init(default_ws);

    // Initialize ram_alloc state
    ram_alloc_init();
    /* All domains use smallcn to initialize */
    err = ram_alloc_set(ram_alloc_fixed);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_RAM_ALLOC_SET);
    }

    err = paging_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_VSPACE_INIT);
    }

    err = slot_alloc_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SLOT_ALLOC_INIT);
    }

    err = morecore_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_MORECORE_INIT);
    }

    lmp_endpoint_init();

    // init domains only get partial init
    if (init_domain) {
        return SYS_ERR_OK;
    }

    // STEP 3: register ourselves with init
    //DBG: Uncomment if you really need it ==> debug_printf ("Initializing LMP system...\n");

    // The following function does all of these steps:
    // Create local endpoint
    // Set remote endpoint to init's endpoint
    // Tell init endpoint the new local endpoint.
    errval_t error = aos_rpc_init (aos_rpc_get_init_channel(), cap_initep);

    if (err_is_fail (error)) {
        debug_printf ("FATAL: Failed to connect to init!\n");
        abort();
    }

    // At this point we can test our channel with a ping.
//     aos_ping (aos_rpc_get_init_channel(), 42);

    // STEP 5: now we should have a channel with init set up and can
    // use it for the ram allocator

    // NOTE: Uncomment this as soon as the RAM server is on its own thread or domain.
//     struct capref ram_server_endpoint = NULL_CAP;
//     error = aos_find_service (aos_service_ram, &ram_server_endpoint);
//     static stuct aos_rpc ram_channel;
//     error = aos_rpc_init (&ram_channel, ram_server_endpoint);
//     ram_server_connection = &ram_channel;
    ram_server_connection = aos_rpc_get_init_channel ();

    // Change ram allocation to use the IPC mechanism.
    error = ram_alloc_set (ram_alloc_ipc);

    // Initialize printf and scanf:

    // Return if we're the serial driver.
    assert ((params != NULL) && (params->argv != NULL));
    const char* domain_name = params -> argv [0];
    if (strcmp (domain_name, "serial_driver") == 0) {
        return SYS_ERR_OK;
    }

    // Find the serial driver.
    struct capref serial_ep;
    error = aos_find_service (aos_service_serial, &serial_ep);

    // Register ourselves with the serial driver.
    if (err_is_ok (error)) {
        aos_rpc_init (aos_rpc_get_serial_driver_channel (), serial_ep);
    }

    // Tell libc to use the IPC mechanism to print characters.
    if (err_is_ok (error)) {
        _libc_terminal_read_func = aos_rpc_terminal_read;
        _libc_terminal_write_func = aos_rpc_terminal_write;
    } else {
        debug_printf ("Error: Could not establish connection to serial driver.\n Error code: %s\n Using sys_print system call instead.\n", err_getstring (error));
    }


    // right now we don't have the nameservice & don't need the terminal
    // and domain spanning, so we return here
    return SYS_ERR_OK;
}

/**
 *  \brief Initialise libbarrelfish, while disabled.
 *
 * This runs on the dispatcher's stack, while disabled, before the dispatcher is
 * setup. We can't call anything that needs to be enabled (ie. cap invocations)
 * or uses threads. This is called from crt0.
 */
void barrelfish_init_disabled(dispatcher_handle_t handle, bool init_dom_arg);
void barrelfish_init_disabled(dispatcher_handle_t handle, bool init_dom_arg)
{
    init_domain = init_dom_arg;
    disp_init_disabled(handle);
    thread_init_disabled(handle, init_dom_arg);
}
