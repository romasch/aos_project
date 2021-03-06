/**
 * \file
 * \brief Interface declaration for AOS rpc-like messaging
 */

/*
 * Copyright (c) 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef _LIB_BARRELFISH_AOS_MESSAGES_H
#define _LIB_BARRELFISH_AOS_MESSAGES_H

#include <barrelfish/barrelfish.h>
#include <barrelfish/lmp_chan.h>

// Some predefined services
enum aos_service {
    aos_service_ram = 0,
    aos_service_serial,
    aos_service_led,
	aos_service_init,
	aos_service_domain,
    aos_service_filesystem,
    //...

    aos_service_test,
    // NOTE: Must be last!
    aos_service_guard
};

#define MAX_PROCESS_NAME_LENGTH (7 * 4 - 1)

// Basic protocol:
// The first argument is the type of message.
// If there's a reply, the first argument is an errval_t. // TODO: this is currently not always implemented.

/**
 * Do a small exchange for testing purposes.
 *
 * Type: Synchronous
 * Target: any
 * Send Args: An arbitrary value
 * Send Capability: Endpoint of current processor (for response)
 * Receive Args: Arbitrary value (same as send)
 * Receive Capability: -
 */
#define AOS_PING 100

/**
 * Register a service provided by the current domain.
 *
 * Type: Synchronous
 * Target: init
 * Send Args: Service identifier
 * Receive Args: Error value
 */
#define AOS_ROUTE_REGISTER_SERVICE 1

/**
 * Find endpoint capability of a service.
 *
 * NOTE: Internally this request triggers
 * an AOS_ROUTE_REQUEST_EP
 *
 * Type: Synchronous
 * Target: init
 * Send Args: Service identifier
 * Send Capability: -
 * Receive Args: Error code
 * Receive Capability: endpoint capability of specified service
 */
#define AOS_ROUTE_FIND_SERVICE 2


/**
 * Get a new endpoint from a known service.
 *
 * NOTE: This request is special:
 * Due to the fact that the response is handled
 * by init, the response message is an AOS_ROUTE_DELIVER_EP
 * and therefore has a message type identifier.
 *
 * Target: A registered server.
 * Source: Only init
 * Send Args: identifier (~cookie)
 * Send Capability: -
 * Receive Args: AOS_ROUTE_DELIVER_EP, error code, identifier
 * Receive Capability: Newly created endpoint
 */
#define AOS_ROUTE_REQUEST_EP 3

/**
 * Deliver an endpoint to the correct target.
 *
 * NOTE: This message is sent
 * after an AOS_ROUTE_REQUEST_EP.
 *
 * Target: init
 * Source: A registered server
 * Send Args: error code, identifier
 * Send Capability: A newly created endpoint.
 *
 * Response: None (The message will be
 * used as a response to AOS_ROUTE_FIND_SERVICE)
 */
#define AOS_ROUTE_DELIVER_EP 4

/**
 * Get a RAM capability.
 *
 * Type: Synchronous
 * Target: RAM service
 * Send Args: Requested size in bits
 * Send Capability: -
 * Receive Args: error value, actual size in bits
 * Receive Capability: RAM cap
 */
#define AOS_RPC_GET_RAM_CAP 5

/**
 * Send a string using a shared buffer.
 *
 * Type: Synchronous
 * Target: Serial driver
 * Send Args: Memory descriptor of shared buffer.
 * Send Capability: -
 * Receive Args: Error value
 * Receive Capability: -
 */
#define AOS_RPC_SEND_STRING 6

/**
 * Send a character to be printed by the UART driver.
 *
 * Type: Asynchronous // TODO: shall we wait for acknowledgement?
 * Target: Serial driver
 * Send Args: a character
 * Send Capability: -
 * Receive Args: no reply
 * Receive Capability: -
 */
#define AOS_RPC_SERIAL_PUTCHAR 7

/**
 * Request a single character from the UART driver.
 *
 * Type: Synchronous
 * Target: Serial driver
 * Send Args: Domain ID of current domain
 * Send Capability: -
 * Receive Args: error value, input character
 * Receive Capability: -
 */
#define AOS_RPC_SERIAL_GETCHAR 8

/**
 * Finalize an LMP connection by sending the local endpoint capability.
 *
 * Type: Synchronous
 * Target: any
 * Send Args: -
 * Send Capability: The local endpoint of the channel.
 * Receive Args: error value
 * Receive Capability: -
 */
#define AOS_RPC_CONNECTION_INIT 9

/**
 * Spawn new domain from the requested binary.
 *
 * Type: Synchronous
 * Target: process manager (init)
 * Send Args: process name
 * Send Capability: -
 * Receive Args: error value and pid of the created process.
 * Receive Capability: -
 */
// #define AOS_RPC_SPAWN_PROCESS 10

/**
 * Request for the process name.
 *
 * Type: Synchronous
 * Target: process manager (init)
 * Send Args: process identifier
 * Send Capability: -
 * Receive Args: error value and process name.
 * Receive Capability: -
 */
#define AOS_RPC_GET_PROCESS_NAME 11

/**
 * Request for the list of all processes known for process manager.
 *
 * Type: Synchronous
 * Target: process manager (init)
 * Send Args: -
 * Send Capability: -
 * Receive Args: error value and list of process id's.
 * Receive Capability: -
 */
#define AOS_RPC_GET_PROCESS_LIST 12

/**
 * Turn the LED on or off.
 *
 * Type: Synchronous
 * Target: LED driver.
 * Send Args: A boolean indicating if the LED should be on (true) or off (false).
 * Send Capability: -
 * Receive Args: error value
 * Receive Capability: -
 */
#define AOS_RPC_SET_LED 13

/**
 * Kill the indicated domain.
 * NOTE: By using its own domain id, this function can
 * also be used to exit a domain.
 *
 * Type: Synchronous
 * Target: Process manager (init)
 * Send Args: ID of the current domain.
 * Send Capability: -
 * Receive Args: error value (although may never receive this if killing itself)
 * Receive Capability: -
 */
#define AOS_RPC_KILL 14

/**
 * Set the foreground task in serial driver.
 * Only this domain can receive input.
 *
 * Type: Synchronous
 * Target: Serial driver
 * Send Args: Domain ID of the selected task.
 * Send Capability: -
 * Receive Args: error value
 * Receive Capability: -
 */
#define AOS_RPC_SET_FOREGROUND 15

/**
 * Get a device frame capability.
 *
 * Type: Synchronous
 * Target: Device frame manager (= init)
 * Send Args: Physical Address Base, Size Bits
 * Send Capability: -
 * Receive Args: error value
 * Receive Capability: device frame cap
 */
#define AOS_RPC_GET_DEVICE_FRAME 16

/**
 * Wait for termination of a process.
 *
 * Type: Synchronous
 * Target: Process manager
 * Send Args: Domain ID
 * Send Capability: -
 * Receive Args: error value
 * Receive Capability: -
 */
#define AOS_RPC_WAIT_FOR_TERMINATION 17

/**
 * Spawn new domain from the requested binary on the remote core.
 *
 * Type: Synchronous
 * Target: process manager (init)
 * Send Args: process name
 * Send Capability: -
 * Receive Args: error value and pid of the created process.
 * Receive Capability: -
 */
// #define AOS_RPC_SPAWN_PROCESS_REMOTELY 18

/**
 * Open file on the removable storage.
 *
 * Type: Synchronous
 * Target: filesystem driver
 * Send Args: path to file
 * Send Capability: -
 * Receive Args: error value and file handle.
 * Receive Capability: -
 */
#define AOS_RPC_OPEN_FILE 19

/**
 * Get list of files in directory (directories itself are files too)
 *
 * Type: Synchronous
 * Target: filesystem driver
 * Send Args: path to directory
 * Send Capability: -
 * Receive Args: error value and handle of directory and number of files contained in directory.
 * Receive Capability: -
 */
// #define AOS_RPC_READ_DIR 20

/**
 * Get name of file in directory (directories itself are files too)
 *
 * Type: Synchronous
 * Target: filesystem driver
 * Send Args: directory handle and index of file inside
 * Send Capability: -
 * Receive Args: error value and name and type of file contained in directory.
 * Receive Capability: -
 */
// #define AOS_RPC_READ_DIRX 21

/**
 * Read data from opened file.
 *
 * Type: Synchronous
 * Target: filesystem driver
 * Send Args: memory descriptor for result buffer, file descriptor, position, size
 * Send Capability: -
 * Receive Args: Error value, number of characters read
 * Receive Buffer: File contents
 *
 * Receive Capability: -
 */
#define AOS_RPC_READ_FILE 22

/**
 * Close previously opened file.
 *
 * Type: Synchronous
 * Target: filesystem driver
 * Send Args: file handle
 * Send Capability: -
 * Receive Args: error value
 * Receive Capability: -
 */
#define AOS_RPC_CLOSE_FILE 23

/**
 * Register an area of shared memory.
 *
 * Type: Synchronous
 * Target: Any server.
 * Send Arguments: -
 * Send Capability: Frame to be shared
 * Receive Args: Error value, memory descriptor
 * Receive Capability: -
 */
#define AOS_RPC_REGISTER_MEMORY 24

/**
 * Delete an area of shared memory.
 *
 * Type: Synchronous
 * Target: Any server.
 * Send Arguments: Memory descriptor
 * Send Capability: -
 * Receive Args: Error value
 * Receive Capability: -
 */
#define AOS_RPC_UNREGISTER_MEMORY 25

/**
 * Read a directory.
 *
 * Type: Synchronous.
 * Target: Filesystem driver
 * Send Arguments: Memory descriptor
 * Send Buffer: Path
 * Receive Arguments: Error value, number of entries.
 * Receive Buffer: Array of struct dirent.
 */
#define AOS_RPC_READ_DIR_NEW 26

/**
 * Spawn a new domain
 *
 * Type: Synchronous
 * Target: Process manager (init)
 * Send Arguments: Memory descriptor, Core ID
 * Send Buffer: Process Name
 * Send Capability: -
 * Receive Arguments: Error value, PID of process (if not remote).
 * Receive Capability: -
 */
#define AOS_RPC_SPAWN_DOMAIN 27

struct aos_rpc {
    uint32_t memory_descriptor;
    void* shared_buffer;
    uint32_t shared_buffer_length;
    struct lmp_chan channel;
};

/// The global init channel.
/// This channel is always available, except in init.
/// NOTE: Initialization happens in libbarrelfish/init.c
struct aos_rpc* aos_rpc_get_init_channel (void);

/// The channel to the serial driver.
/// This channel is always available, except in init and serial_driver.
/// NOTE: Initialization happens in libbarrelfish/init.c
struct aos_rpc* aos_rpc_get_serial_driver_channel (void);


//NOTE: Start of protected API

/**
 * \brief send a string over the given channel
 */
errval_t aos_rpc_send_string(struct aos_rpc *chan, const char *string); // TODO:

/**
 * \brief request a RAM capability with >= request_bits of size over the given
 * channel.
 */
errval_t aos_rpc_get_ram_cap(struct aos_rpc *chan, size_t request_bits,
                             struct capref *retcap, size_t *ret_bits); // TODO:

/**
 * \brief get one character from the serial port
 */
errval_t aos_rpc_serial_getchar(struct aos_rpc *chan, char *retc);

/**
 * \brief send one character to the serial port
 */
errval_t aos_rpc_serial_putchar(struct aos_rpc *chan, char c);

/**
 * \brief Request device memory capability from memory server.
 */
errval_t aos_rpc_get_dev_cap(struct aos_rpc *chan, lpaddr_t paddr,
                             size_t length, struct capref *retcap,
                             size_t *retlen);

/**
 * \brief Request process manager to start a new process
 * \arg name the name of the process that needs to be spawned (without a path prefix)
 * \arg newpid the process id of the newly spawned process
 */
errval_t aos_rpc_process_spawn (struct aos_rpc *chan,
                                char *name,
                                coreid_t core,
                                domainid_t *newpid);

/**
 * \brief Get name of process with id pid.
 * \arg pid the process id to lookup
 * \arg name A null-terminated character array with the name of the process
 * that is allocated by the rpc implementation. Freeing is the caller's
 * responsibility.
 */
errval_t aos_rpc_process_get_name(struct aos_rpc *chan, domainid_t pid, char **name);

/**
 * \brief Get process ids of all running processes
 * \arg pids An array containing the process ids of all currently active
 * processes. Will be allocated by the rpc implementation. Freeing is the
 * caller's  responsibility.
 * \arg pid_count The number of entries in `pids' if the call was successful
 */
errval_t aos_rpc_process_get_all_pids(struct aos_rpc *chan, domainid_t **pids, size_t *pid_count);






/**
 * \brief open a file
 * \arg path the file's path
 * \arg fd a numeric identifier (file descriptor) which can then be used to
 * access the open file
 */
errval_t aos_rpc_open(struct aos_rpc *chan, char *path, int *fd);

#define MAXNAMELEN (7 * 4)
struct aos_dirent {
    /// name of the directory entry
    char name[MAXNAMELEN];
    /// size of the item referenced
    size_t size;
};

/**
 * \brief list a directory's contents
 * \arg path the directory's path
 * \arg dir An array containing a struct dirent for each directory entry. Will
 * be allocated by the rpc implementation. Freeing is the caller's
 * responsibility.
 * \arg elem_count the size of `dir' in elements
 */
errval_t aos_rpc_readdir(struct aos_rpc *chan, char* path, struct aos_dirent **dir, size_t *elem_count);

/**
 * \brief read from an open file
 * \arg fd the file descriptor returned by a previous call to open
 * \arg position the position in the file to read from
 * \arg size the number of bytes to read from the file
 * \arg buf A buffer containing the bytes read. Will
 * be allocated by the rpc implementation. Freeing is the caller's
 * responsibility.
 * \arg buflen the amount of bytes read and stored in `buf'
 */
errval_t aos_rpc_read(struct aos_rpc *chan, int fd, size_t position, size_t size, void** buf, size_t *buflen);

/**
 * \brief close an open file
 * \arg fd the file descriptor returned by a previous call to open
 */
errval_t aos_rpc_close(struct aos_rpc *chan, int fd);

/// Optional modification support for the file system

/**
 * \brief write to an open file
 * \arg fd the file descriptor returned by a previous call to open
 * \arg position the position in the file to write to
 * \arg size the number of bytes actually written to the file
 * \arg buf A buffer containing the bytes to write.
 * \arg buflen the amount of bytes in `buf' that should be written to the file
 */
errval_t aos_rpc_write(struct aos_rpc *chan, int fd, size_t position, size_t *size,
                       void *buf, size_t buflen);

/**
 * \brief create a new file. Will perform an implicit open when successful.
 * Shouldn't succeed when a file (or directory) with the same path exists. Can
 * create missing directories.
 * \arg path the path of the file to create
 * \arg fd the file descriptor of the newly created file
 */
errval_t aos_rpc_create(struct aos_rpc *chan, char *path, int *fd);

/**
 * \brief delete a file. Should fail / be deferred if that file is still open
 * (i.e. there is a fd referencing that file)
 * \arg path the file to delete
 */
errval_t aos_rpc_delete(struct aos_rpc *chan, char *path);

///NOTE: End of protected API.

/**
 * \brief Initialize given rpc channel.
 * TODO: you may want to change the inteface of your init function
 */
errval_t aos_rpc_init(struct aos_rpc *rpc, struct capref receiver);

errval_t aos_connection_init (struct lmp_chan* channel);

/**
 * \brief Find the endpoint for the domain which provides the specified service.
 * This request gets handled by init.
 */
errval_t aos_find_service (uint32_t service, struct capref* endpoint);

/**
 * \brief Register a service of the current process with init.
 */
errval_t aos_register_service (struct aos_rpc* rpc, uint32_t service);

/**
 * \brief Turn the LED on or off.
 */
errval_t aos_rpc_set_led (struct aos_rpc* rpc, bool new_state);

/**
 * \brief Set domain as the foreground task.
 */
errval_t aos_rpc_set_foreground (struct aos_rpc* rpc, domainid_t domain);

/**
 * \brief Kill another domain.
 */
errval_t aos_rpc_kill (struct aos_rpc*, domainid_t domain);

/**
 * \brief Exit the current domain.
 * Internally uses kill.
 */
void aos_rpc_exit (struct aos_rpc* rpc);

/**
 * \brief Block until domain terminates.
 */
errval_t aos_rpc_wait_for_termination (struct aos_rpc* rpc, domainid_t domain);

/**
 * Ping a domain.
 */
errval_t aos_ping (struct aos_rpc* channel, uint32_t value);

#endif // _LIB_BARRELFISH_AOS_MESSAGES_H
