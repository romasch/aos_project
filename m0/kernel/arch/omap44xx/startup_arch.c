/*
 * Copyright (c) 2009, 2010, 2012 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <dispatch.h>
#include <string.h>
#include <stdio.h>

#include <barrelfish_kpi/init.h>
#include <barrelfish_kpi/syscalls.h>

#include <arm_hal.h>
#include <paging_kernel_arch.h>
#include <exceptions.h>
#include <cp15.h>
#include <cpiobin.h>
#include <init.h>
#include <barrelfish_kpi/paging_arm_v7.h>
#include <arm_core_data.h>
#include <kernel_multiboot.h>
#include <offsets.h>
#include <startup_arch.h>
#include <startup_helpers.h>
#include <global.h>

#define UNUSED(x)               (x) = (x)

#define STARTUP_PROGRESS()      debug(SUBSYS_STARTUP, "%s:%d\n",          \
                                      __FUNCTION__, __LINE__);

#define INIT_MODULE_NAME    "armv7/sbin/init"

static inline uintptr_t round_up(uintptr_t value, size_t unit)
{
    assert(0 == (unit & (unit - 1)));
    size_t m = unit - 1;
    return (value + m) & ~m;
}

//static phys_mmap_t* g_phys_mmap;        // Physical memory map
static union arm_l1_entry * init_l1;              // L1 page table for init
static union arm_l2_entry * init_l2;              // L2 page tables for init

/**
 * Each kernel has a local copy of global and locks. However, during booting and
 * kernel relocation, these are set to point to global of the pristine kernel,
 * so that all the kernels can share it.
 */
//static  struct global myglobal;
struct global *global = (struct global *)GLOBAL_VBASE;


// NOTE: internal functionality expects l2 ptables back-to-back in memory
/*
 * \brief Initialize page tables
 *
 * This includes setting up page tables for the init process.
 */
__attribute__((unused))
static void build_init_page_tables(void)
{
    // Create page tables for init
    init_l1 =  (union arm_l1_entry *)local_phys_to_mem(alloc_phys_aligned(INIT_L1_BYTES, ARM_L1_ALIGN));
    memset(init_l1, 0, INIT_L1_BYTES);

    init_l2 = (union arm_l2_entry *)local_phys_to_mem(alloc_phys_aligned(INIT_L2_BYTES, ARM_L2_ALIGN));
    memset(init_l2, 0, INIT_L2_BYTES);

    /*
     * Initialize init page tables - this just wires the L1
     * entries through to the corresponding L2 entries.
     */
    STATIC_ASSERT(0 == (INIT_VBASE % ARM_L1_SECTION_BYTES), "");
    for (lvaddr_t vaddr = INIT_VBASE;
         vaddr < INIT_SPACE_LIMIT;
         vaddr += ARM_L1_SECTION_BYTES) {
        uintptr_t section = (vaddr - INIT_VBASE) / ARM_L1_SECTION_BYTES;
        uintptr_t l2_off = section * ARM_L2_TABLE_BYTES;
        lpaddr_t paddr = mem_to_local_phys((lvaddr_t)init_l2) + l2_off;
        paging_map_user_pages_l1((lvaddr_t)init_l1, vaddr, paddr);
    }
    printf("build_init_page_tables done: init_l1=%p init_l2=%p\n",
            init_l1, init_l2);

    printf("Calling paging_context_switch with address = %"PRIxLVADDR"\n",
           mem_to_local_phys((lvaddr_t) init_l1));
    paging_context_switch(mem_to_local_phys((lvaddr_t)init_l1));
}

struct dcb *spawn_init(const char *name)
{
    printf("spawn_init\n");

    // allocate & prime init control block
    // the argument gets filled in with the address of the parameter page
    struct dcb *init_dcb = allocate_init_dcb(name);

    // Set up page tables for Init, set the provided variables
    // init_l1 and init_l2 to the address of the L1 and L2 page table respectively.
    // NOTE: internal functionality expects l2 ptables back-to-back in memory

    build_init_page_tables();

    // save address of user L1 page table in init_dcb->vspace
    init_dcb -> vspace = (uint32_t) init_l1;

    // Map & Load init structures and ELF image
    // returns the entry point address and the global offset table base address
    genvaddr_t init_ep, got_base;
    map_and_load_init(name, init_dcb, init_l2, &init_ep, &got_base);

    // get dispatcher structs from dispatcher handle
    struct dispatcher_shared_arm *disp_arm
        = get_dispatcher_shared_arm(init_dcb->disp);

    // set Thread local storage register in register save area
    disp_arm->save_area.named.rtls = INIT_DISPATCHER_VBASE;

    // set global offset table base in dispatcher
    disp_arm->got_base = got_base;

    // set processor mode
    disp_arm->save_area.named.cpsr = ARM_MODE_USR | CPSR_F_MASK;

    // set pc and r10(got base) in register save area (disp_arm->save_area)

    disp_arm -> save_area.named.pc = init_ep;
    disp_arm -> save_area.named.r10 = got_base;

    return init_dcb;
}

void arm_kernel_startup(void)
{
    printf("arm_kernel_startup entered \n");

    struct dcb *init_dcb;

    printf("Doing BSP related bootup \n");

    // Initialize the location to allocate phys memory from
    init_alloc_addr = glbl_core_data->start_free_ram;

    // Create & initialize init control block
    init_dcb = spawn_init(INIT_MODULE_NAME);

    // Should not return
    printf("Calling dispatch from arm_kernel_startup, start address is=%"PRIxLVADDR"\n",
           get_dispatcher_shared_arm(init_dcb->disp)->save_area.named.pc);
    dispatch(init_dcb);
    panic("Error spawning init!");
}
