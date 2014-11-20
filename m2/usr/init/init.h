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
#include <spawndomain/spawndomain.h>

extern struct bootinfo *bi;

errval_t initialize_ram_alloc(void);
errval_t initialize_mem_serv(void);

errval_t initialize_device_frame_server (struct capref io_space_cap);
errval_t allocate_device_frame (lpaddr_t physical_base, uint8_t size_bits, struct capref* ret_frame);

// Entry point of the UART driver thread
int terminal_thread (void* arg);
// int uart_driver_thread (void* arg);

char uart_getchar(void);
void uart_putchar(char c);
void init_uart_driver (void);
errval_t spawn_serial_driver_thread (void);

// A test thread for init.
errval_t spawn_test_thread ( void (*handler_func) (void* arg));


// LED controls:
errval_t led_init (void);
void led_set_state (bool new_state);

#endif // INIT_H
