/**
 * \file
 * \brief User-side system call wrappers
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef LIBSOS_SYSCALL_H
#define LIBSOS_SYSCALL_H

/* standard libc types and assert */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

#include <errors/errno.h>

/* NOP system call */
errval_t sys_nop(void);

/**
 * \brief Print a string through the kernel.
 *
 * This calls #SYSCALL_PRINT to print 'string' of length 'length' through
 * the kernel. Whether and where 'string' is printed is determined by
 * the kernel.
 *
 * \param string        Pointer to string to print.
 * \param length        Length of string.
 *
 * \return Syscall error code (#SYS_ERR_OK on success).
 */
errval_t sys_print(const char *string, size_t length);

/**
 * \brief Switch the LED on or off.
 * 
 * \param bool     Set to true to enable, false to disable.
 */
errval_t sys_led (bool);

/**
 * Surprize the kernel with an undefined system call.
 */
errval_t sys_undefined (void);

#endif //LIBSOS_SYSCALL_H
