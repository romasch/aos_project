/**
 * \file
 * \brief Barrelfish paging helpers.
 */

/*
 * Copyright (c) 2012, 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <barrelfish/paging.h>
#include <barrelfish/except.h>
#include <barrelfish/slab.h>
#include "threads_priv.h"

#include <stdio.h>
#include <string.h>

static struct paging_state current;

/**
 * \brief Helper function that allocates a slot and
 *        creates a ARM l2 page table capability
 */
__attribute__((unused))
static errval_t arml2_alloc(struct capref *ret)
{
    errval_t err;
    err = slot_alloc(ret);
    if (err_is_fail(err)) {
        debug_printf("slot_alloc failed: %s\n", err_getstring(err));
        return err;
    }
    err = vnode_create(*ret, ObjType_VNode_ARM_l2);
    if (err_is_fail(err)) {
        debug_printf("vnode_create failed: %s\n", err_getstring(err));
        return err;
    }
    return SYS_ERR_OK;
}

// TODO: implement page fault handler that installs frames when a page fault
// occurs and keeps track of the virtual address space.

#define S_SIZE 2*8192
#define FLAGS (KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE)
static char e_stack[S_SIZE];
static char* e_stack_top = e_stack + S_SIZE;
static void my_exception_handler (enum exception_type type, int subtype,
                                     void *addr, arch_registers_state_t *regs,
                                     arch_registers_fpu_state_t *fpuregs) 
{
    debug_printf ("Hello Exception World!\nGot type %u, subtype %u, addr %X\n", type, subtype, addr);
    lvaddr_t vaddr = (lvaddr_t) addr;

    if (type == EXCEPT_PAGEFAULT 
        && subtype == PAGEFLT_NULL
        && current.heap_begin <= vaddr 
        && vaddr < current.heap_end )
    {
        lvaddr_t mapped = current.heap_mapped;
        
        while (mapped <= vaddr) {
            
            int l1_index = ARM_L1_USER_OFFSET(mapped);
            int l2_index = ARM_L2_USER_OFFSET(mapped);
            struct capref l1_table = (struct capref) {
            .cnode = cnode_page,
            .slot = 0,
            };
            // would need checks if ptables exist already
            struct capref l2_table;
            if (current.last_l1_index != l1_index) {
                arml2_alloc(&l2_table);
                vnode_map(l1_table, l2_table, l1_index, FLAGS, 0, 1);
                
                current.last_l1_index = l1_index;
                current.last_l2_table = l2_table;
            } else {
                l2_table = current.last_l2_table;
            }
            
            // TODO: Split frames to perform mappings of less than 1 MB.

            size_t frame_size = 1024 * 1024u; // 1 MiB

            struct capref frame;
            frame_alloc(&frame, frame_size, &frame_size);

            errval_t err = vnode_map(l2_table, frame, l2_index, FLAGS, 0, 256);

            if (err_is_fail(err)) {
                debug_printf ("Mapping failed: %s\n", err_getstring (err));
                abort();
            }
            /*
            if (current.last_frame_slot < 0 || current.last_frame_slot >= 256) {
                // No more space in current frame.
                frame_alloc(&frame, frame_size, &frame_size);
                debug_printf ("Allocated 0x%X bytes\n", frame_size); 
                current.current_frame = frame;
                current.last_frame_slot = -1;
            }
            frame = current.current_frame;
            uint32_t slot = current.last_frame_slot + 1;
            current.last_frame_slot = slot;

            size_t page_size = 4096u;
            errval_t err = vnode_map(l2_table, frame, l2_index, FLAGS, slot*page_size, 256);
            if (err_is_fail(err)) {
                debug_printf ("Mapping failed: %s\n", err_getstring (err));
                abort();
            }//*/

            mapped += frame_size;
        }

        current.heap_mapped = mapped;
    } else {
        debug_printf ("Invalid address!\n");
        abort();
    }
}


errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr, struct capref pdir)
{
    debug_printf("paging_init_state\n");
    debug_printf ("Size of struct capref: %u", sizeof(struct capref));
    // TODO: implement state struct initialization
    st -> heap_begin = start_vaddr;
    st -> heap_end = start_vaddr;
    st ->  heap_mapped = start_vaddr;
    st -> last_l1_index = -1;
    st -> last_frame_slot = -1;
    
    return SYS_ERR_OK;
}


errval_t paging_init(void)
{
    debug_printf("paging_init\n");
    // TODO: initialize self-paging handler
    // TIP: use thread_set_exception_handler() to setup a page fault handler
    void* oldbase = 0;
    void* oldtop = 0;
    thread_set_exception_handler (&my_exception_handler, NULL, e_stack, e_stack_top, &oldbase, &oldtop);
    debug_printf ("Got old stack base: %X, old stack top: %X\n", oldbase, oldtop);
    // TIP: Think about the fact that later on, you'll have to make sure that
    // you can handle page faults in any thread of a domain.
    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.
    
    //TODO WTF is: struct capref pdir
    struct capref pdir;
    //TODO check if VADDR_OFFSET is ok as the start of our heap.
    paging_init_state (&current, VADDR_OFFSET, pdir);
    set_current_paging_state(&current);
    return SYS_ERR_OK;
}

void paging_init_onthread(struct thread *t)
{
    // TODO: setup exception handler for thread `t'.
    debug_printf ("called\n");
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_init(struct paging_state *st, struct paging_region *pr, size_t size)
{
    void *base;
    errval_t err = paging_alloc(st, &base, size);
    if (err_is_fail(err)) {
        debug_printf("paging_region_init: paging_alloc failed\n");
        return err_push(err, LIB_ERR_VSPACE_MMU_AWARE_INIT);
    }
    pr->base_addr    = (lvaddr_t)base;
    pr->current_addr = pr->base_addr;
    pr->region_size  = size;
    // TODO: maybe add paging regions to paging state?
    return SYS_ERR_OK;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size)
{
    lvaddr_t end_addr = pr->base_addr + pr->region_size;
    size_t rem = end_addr - pr->current_addr;
    if (rem > req_size) {
        // ok
        *retbuf = (void*)pr->current_addr;
        *ret_size = req_size;
        pr->current_addr += req_size;
    } else if (rem > 0) {
        *retbuf = (void*)pr->current_addr;
        *ret_size = rem;
        pr->current_addr += rem;
        debug_printf("exhausted paging region, "
                "expect badness on next allocation\n");
    } else {
        return LIB_ERR_VSPACE_MMU_AWARE_NO_SPACE;
    }
    return SYS_ERR_OK;
}

/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * We ignore unmap requests right now.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, size_t bytes)
{
    // XXX: should free up some space in paging region, however need to track
    //      holes for non-trivial case
    return SYS_ERR_OK;
}

/**
 * \brief Find a bit of free virtual address space that is large enough to
 *        accomodate a buffer of size `bytes`.
 * TODO: you need to implement this function using the knowledge of your
 * self-paging implementation about where you have already mapped frames.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes)
{
    
    printf ("Paging alloc with bytes %X\n", bytes);
    if (bytes << 20 != 0) {
        debug_printf ("Error in paging_alloc: bytes not page aligned.\n");
        abort();
    }
    
    *buf = (void*)  st -> heap_end;
    st -> heap_end += bytes;
    return SYS_ERR_OK;
}

/**
 * \brief map a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 */
errval_t paging_map_frame_attr(struct paging_state *st, void **buf,
                               size_t bytes, struct capref frame,
                               int flags, void *arg1, void *arg2)
{
    errval_t err = paging_alloc(st, buf, bytes);
    if (err_is_fail(err)) {
        return err;
    }
    return paging_map_fixed_attr(st, (lvaddr_t)(*buf), frame, bytes, flags);
}


/**
 * \brief map a user provided frame at user provided VA.
 */
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
        struct capref frame, size_t bytes, int flags)
{
    // TODO: you will need this functionality in later assignments. Try to
    // keep this in mind when designing your self-paging system.
    return SYS_ERR_OK;
}

/**
 * \brief unmap a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 * NOTE: this function is currently here to make libbarrelfish compile. As
 * noted on paging_region_unmap we ignore unmap requests right now.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
    return SYS_ERR_OK;
}
