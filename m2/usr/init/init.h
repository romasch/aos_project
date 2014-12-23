/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2007, 2008, 2009, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef INIT_H
#define INIT_H

#include <stdio.h>
#include <barrelfish/barrelfish.h>

#include <barrelfish/aos_rpc.h>

struct bootinfo* get_bootinfo (void);
coreid_t get_core_id (void);

// Physical memory server functions.
errval_t initialize_ram_alloc(void);
errval_t initialize_mem_serv(void);

// Device frame server functions.
errval_t initialize_device_frame_server (struct capref io_space_cap);
errval_t allocate_device_frame (lpaddr_t physical_base, uint8_t size_bits, struct capref* ret_frame);

// Process management:
enum domain_info_state {domain_info_state_free = 0, domain_info_state_running, domain_info_state_zombie};
struct domain_info
{
    // The first MAX_PROCESS_NAME_LENGTH characters of the domain name.
    // TODO: Storing the full name of a domain would be nice, although it complicates management.
    char name[MAX_PROCESS_NAME_LENGTH + 1];

    // The dispatcher and root cnode capabilities of the domain.
    struct capref dispatcher_capability;
    struct capref root_cnode_capability;

    // The current state (running, zombie etc...)
    enum domain_info_state state;

    // Initial connection to domain.
    // NOTE: Malloced and owned by this struct, needs to be freed.
    struct lmp_chan* channel;

    // Possible observer for termination events.
    // NOTE: Not owned, do not free!
    struct lmp_chan* termination_observer;
};

errval_t init_domain_manager (void);
uint32_t max_domain_id (void);
struct domain_info* get_domain_info (domainid_t id);
errval_t spawn (char* name, domainid_t* ret_id);

// Cross core setup:
errval_t init_cross_core_buffer (void);
void* get_cross_core_buffer (void);
errval_t spawn_core (coreid_t core_identifier);

// Cross core communication:
#define IKC_MSG_REMOTE_SPAWN 0x0FFFFFFFU
void* ikc_rpc_call(void* message, int size);
int ikc_server(void* data);

// LED controls:
errval_t led_init (void);
void led_set_state (bool new_state);

// A test thread.
errval_t spawn_test_thread ( void (*handler_func) (void* arg));
#endif // INIT_H
