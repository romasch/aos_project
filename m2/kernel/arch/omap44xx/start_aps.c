/**
 * \file
 * \brief Start the application processors
 *
 *  This file sends all needed IPIs to the other cores to start them.
 */

/*
 * Copyright (c) 2007, 2008, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <paging_kernel_arch.h>
#include <arch/armv7/start_aps.h>
#include <arm_hal.h>
#include <cp15.h>

#define STARTUP_TIMEOUT         0xffffff


static volatile uint32_t* aux_core_boot_0 = (volatile uint32_t*) AUX_CORE_BOOT_0;
static volatile uint32_t* aux_core_boot_1 = (volatile uint32_t*) AUX_CORE_BOOT_1;


// Remap the base registers.
void start_aps_remap (uint32_t base)
{
    aux_core_boot_0 = (volatile uint32_t*)(base +(AUX_CORE_BOOT_0 & ARM_L1_SECTION_MASK));
    aux_core_boot_1 = (volatile uint32_t*)(base +(AUX_CORE_BOOT_1 & ARM_L1_SECTION_MASK));
}

/**
 * Send event to other core
 * Note: this is PandaBoard_specific
 */
void send_event(void)
{
    __asm__ volatile ("SEV");
}



// Entry point into the kernel for second core
void app_core_start(void); // defined in boot.S


/**
 * \brief Boot an arm app core
 *
 * \param core_id   APIC ID of the core to try booting
 * \param entry     Entry address for new kernel in the destination
 *                  architecture's lvaddr_t
 *
 * NOTE: If entry is NULL, this function will use app_core_start as a default.
 *
 * \returns Zero on successful boot, non-zero (error code) on failure
 */
int start_aps_arm_start(uint8_t core_id, lpaddr_t entry)
{
    // TODO: you might want to implement this function

    // NOTE: Do not call print here.

    errval_t error = SYS_ERR_OK;

    if (core_id == 1) {
        // Set up the first register.
        // NOTE: Bits [3:2] should not be zero, otherwise the core will ignore the interrupt.
        // See OMAP TRM Page 1144
        *aux_core_boot_0 = AP_STARTING_UP;

        // Set up the entry address.
        if (entry == 0) {
            *aux_core_boot_1 = (volatile uint32_t) &app_core_start;
        } else {
            *aux_core_boot_1 = entry;
        }

        // Send the interrrupt.
        send_event ();

        // Wait until the other processor tells us to continue.
        while (*aux_core_boot_0 != AP_STARTED) {}

        // TODO: prevents some ugly race conditions in serial driver.
        // We need to check why the lock in global struct doesn't work yet.
        for (volatile int i = 0; i<40000000; i++);

    } else {
        error = SYS_ERR_CORE_NOT_FOUND;
    }
    return error;
}
