/*
 * Copyright (c) 2009-2013 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <dispatch.h>
#include <arm.h>
#include <arm_hal.h>
#include <cp15.h>
#include <exceptions.h>
#include <exec.h>
#include <misc.h>
#include <stdio.h>
#include <wakeup.h>
#include <barrelfish_kpi/syscalls.h>
#include <omap44xx_led.h> // for LED syscall
#include <serial.h> // for serial_putchar();

__attribute__((noreturn)) void sys_syscall_kernel(void);
__attribute__((noreturn)) void sys_syscall(arch_registers_state_t*);

#define UNUSED(x) x=x

// Defined here as "real" Barrelfish uses another system anyway and I don't want to modify <syscalls.h>.
#define ERR_SYSCALL_ARGUMENT_INVALID 2    ///< Error for invalid arguments, e.g. negative sizes.
#define ERR_SYSCALL_UNKNOWN 3 ///< For unknown syscalls.

void handle_user_page_fault(lvaddr_t                fault_address,
                            arch_registers_state_t* save_area)
{
    uintptr_t saved_pc = save_area->named.pc;
    panic("user page fault: addr %"PRIxLVADDR" IP %"PRIxPTR"\n",
            fault_address, saved_pc);
}

void handle_user_undef(lvaddr_t fault_address,
                       arch_registers_state_t* save_area)
{
    panic("user undef fault IP %"PRIxPTR"\n", fault_address);
}

void fatal_kernel_fault(uint32_t evector, lvaddr_t address, arch_registers_state_t* save_area
    )
{
    panic("Kernel fault at %08"PRIxLVADDR" vector %08"PRIx32"\nsave_area->ip = %"PRIx32"\n",
	  address, evector, save_area->named.pc);
}

void handle_irq(arch_registers_state_t* save_area, uintptr_t fault_pc)
{
    panic("Unhandled IRQ\n");
}

void sys_syscall_kernel(void)
{
    panic("Why is the kernel making a system call?");
}

static int check_arg_count (uint32_t actual, uint32_t theoretical)
{
	return (actual == theoretical) ? 0 : ERR_SYSCALL_ARGUMENT_MISMATCH;
}

void sys_syscall(arch_registers_state_t* context)
{
    // extract syscall number and number of syscall arguments from
    // registers
    struct registers_arm_syscall_args* sa = &context->syscall_args;
    uintptr_t   syscall = sa->arg0 & 0xf;
    uintptr_t   argc    = (sa->arg0 >> 4) & 0xf;

	char* buffer = 0;
	uint32_t length = 0;
	
	uint32_t error_value = 0;

    // Implement syscall handling here for milestone 1.
	
	switch (syscall) {
		case  SYSCALL_NOP:
			printf ("Received NOP syscall.\n");
			error_value = check_arg_count (argc, 1);
			break;
		case SYSCALL_PRINT:
			printf ("Received PRINT syscall, length %u\n", length);
			error_value = check_arg_count (argc, 3);

			buffer = (char*) sa ->  arg1;
			length = sa -> arg2;
			error_value = check_arg_count (argc, 3);
			
			// It may be possible to read kernel memory...
			if ((buffer + length) >= (char*) 0x80000000 || buffer + length <= buffer || buffer == NULL) {
				error_value = ERR_SYSCALL_ARGUMENT_INVALID;
			}
			
			if (error_value == 0) {
				for (int i=0; i<length; ++i) {
					serial_putchar (buffer[i]);
				}
			}
			
			break;
		case SYSCALL_LED:
			printf ("Received LED syscall\n");
			error_value = check_arg_count (argc, 2);
			
			bool new_state = (bool) sa -> arg1;
			
			if (error_value == 0) {
				led_set_state (new_state);
			}
			
			break;
		default:
			printf ("Unknown syscall. Ignore and return.\n");
			error_value = ERR_SYSCALL_UNKNOWN;
	}

	// Apparently Register R0 should contain the return value.
	sa -> arg0 = error_value;
	
    // resume user process
    resume(context);
}

