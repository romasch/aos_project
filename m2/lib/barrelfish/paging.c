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

void page_in (lvaddr_t addr, bool is_revokable, size_t page_count);

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
//     debug_printf ("Hello Exception World! Got type %u, subtype %u, addr %X\n", type, subtype, addr);
    lvaddr_t vaddr = (lvaddr_t) addr;

    if (type == EXCEPT_PAGEFAULT 
        && subtype == PAGEFLT_NULL
        && current.heap_begin <= vaddr 
        && vaddr < current.heap_end )
    {
#if 0
        
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

/*            frame_alloc(&frame, frame_size, &frame_size);

            errval_t err = vnode_map(l2_table, frame, l2_index, FLAGS, 0, 256);

            if (err_is_fail(err)) {
                debug_printf ("Mapping failed: %s\n", err_getstring (err));
                abort();
            }
            //*/
            if (current.last_frame_slot < 0 || current.last_frame_slot >= 255) {
                // No more space in current frame.
                struct capref alloc_frame;
                frame_alloc(&alloc_frame, frame_size, &frame_size);
                debug_printf ("Allocated 0x%X bytes\n", frame_size); 
                current.current_frame = alloc_frame;
                current.last_frame_slot = -1;
            }
            
            struct capref frame;
            slot_alloc (&frame);
            cap_copy (frame, current.current_frame);
//             frame = current.current_frame;
            uint32_t slot = current.last_frame_slot + 1;
            current.last_frame_slot = slot;
//             debug_printf ("slot %u\n", slot);

            size_t page_size = 4096u;
            errval_t err = vnode_map(l2_table, frame, l2_index, FLAGS, slot*page_size, 1);
            if (err_is_fail(err)) {
                debug_printf ("Mapping failed: %s\n", err_getstring (err));
                abort();
            }//*/

            mapped += page_size;
        }

        current.heap_mapped = mapped;
#else
        page_in ((lvaddr_t) addr, true, 1);
#endif
    } else {
        debug_printf ("Invalid address!\n");
        abort();
    }
}

/**
 * Make sure that a page table for (addr) exists.
 * NOTE: This function might be called recursively in slab allocators.
 *
errval_t paging_allocate_ptable (lvaddr_t addr) {

}//*/

/**
 * Map the virtual address addr.
 * Must not be called if addr is already mapped.
 */
void page_in (lvaddr_t addr, bool is_revokable, size_t page_count) {
    int l1_index = ARM_L1_USER_OFFSET(addr);
    int l2_index = ARM_L2_USER_OFFSET(addr);
    
    errval_t error = 0;
    
    struct ptable_lvl2* ptable = current.ptables [l1_index];
//     debug_printf ("lvl1_index: %u, lvl2_index %u\n", l1_index, l2_index);
    if (!is_revokable) debug_printf ("page in non-revokable memory\n");
    
    // Check if we already have a second-level page table.
    if (!ptable) {
        printf ("ptable_alloc\n");
        
        // Ask the kernel for a new table.
        struct capref l1_cap = (struct capref) {
            .cnode = cnode_page,
            .slot = 0,
        };
        struct capref l2_cap;
        error = arml2_alloc(&l2_cap); //TODO errors
//         debug_printf ("%s\n", err_getstring(error));
        error = vnode_map(l1_cap, l2_cap, l1_index, FLAGS, 0, 1); //TODO errors
//         debug_printf ("%s\n", err_getstring(error));
        
        // Use our temporary table to store the result.
        // TODO: not sure if temporary table works... 
        // It might be better to store it right on the stack.
        // Also, the overall function gets too big. This part should be factored out.
        ptable = &current.temp_ptable;
        init_ptable_lvl2 (ptable);
        current.temp_ptable.lvl2_cap = l2_cap;
        current.ptables [l1_index] = ptable;

        // Allocate dynamic memory for our struct.
        // NOTE: This may call ptable_refill() which in turn invokes page_in again.
        // That's why we need to fiddle around with temporary tables.
        ptable = (struct ptable_lvl2*) slab_alloc ( &(current.ptable_mem)); //TODO errors
        // Copy the temporary table to its final destination.
        memcpy (ptable, &current.temp_ptable, sizeof (struct ptable_lvl2));
        // Don't forget to update the pointer.
        current.ptables [l1_index] = ptable;
//         debug_printf ("ptable allocated\n");
    }
    
    // Great, we have a second-level page table now.
    // Try to get a frame next.
    // TODO: Support partial use of a frame.

    size_t frame_size = page_count * 4096;

    if (is_revokable) {
        
        struct capref base_frame;
        
        if (current.flist_tail == NULL || current.flist_tail -> next_free_slot >= (FRAME_SIZE / PAGE_SIZE)) {
            
            // Ask the kernel for a frame.
            size_t actual_size = 0;
            error = frame_alloc(&base_frame, FRAME_SIZE, &actual_size); // TODO errors

            // Grab some dynamic memory for the list.
            struct frame_list* node = slab_alloc ( &(current.frame_mem));
            init_frame_list (node);
            
            // Initialize the new frame with our data structure.
            node -> frame = base_frame;
            if (current.flist_tail && current.flist_head) {
                // Extend the list.
                current.flist_tail -> next = node;
                current.flist_tail = node;
                current.flist_head = node;
            } else {
                // We're the first node.
                current.flist_head = node;
                current.flist_tail = node;
            }
            debug_printf ("Got new frame\n");
        } else {
            base_frame = current.flist_tail -> frame;
        }
        
        
        int i = current.flist_tail -> next_free_slot;
        
        // Copy a frame.
        struct capref copied_frame;
        slot_alloc (&copied_frame);
        error = cap_copy (copied_frame, base_frame); // TODO errors

        // Map the virtual address to our frame.
        error = vnode_map(ptable -> lvl2_cap, copied_frame, l2_index, FLAGS, i*PAGE_SIZE, 1);// TODO errors

        
        // Need to do some additional bookkeeping if pages can be revoked later.

        // Perform some bookkeeping for every newly mapped page.
//        for (int i=0; i<page_count; i++) {
        current.flist_tail -> next_free_slot = i+1;

            // Let the frame know which pages it contains.
            current.flist_tail -> pages [i] = addr;

            // Check if the page is paged out to persistent storage.
            if (ptable -> page_exists [l2_index]) {
                // Uh-oh, paging stuff in and out not implemented yet.
                USER_PANIC ("Paging in from persistent memory not implemented\n");
            } else {
                // Update page table entry.
                ptable -> page_exists [l2_index] = true;
            }
//         }
        
        
    } else { // TODO Split page_in into two functions, for revokable and irrevokable memory.
        
    // Ask the kernel for a frame.
        struct capref new_frame;
        debug_printf ("blub: %X\n", frame_size);
        
        error = frame_alloc(&new_frame, frame_size, &frame_size); // TODO errors
        debug_printf ("Requested pages: %u, actual pages: %u\n", page_count, frame_size/4096);
        debug_printf ("%s\n", err_getstring(error));
        
        // Map the virtual address to our frame.
        error = vnode_map(ptable -> lvl2_cap, new_frame, l2_index, FLAGS, 0, page_count);// TODO errors
        debug_printf ("%s\n", err_getstring(error));
        debug_printf ("blab\n");
    }
}

void init_ptable_lvl2 (struct ptable_lvl2* ptable) 
{
    memset (ptable, 0, sizeof(struct ptable_lvl2));
}
void init_frame_list (struct frame_list* node)
{
    memset (node, 0, sizeof(struct frame_list));
}

static errval_t memory_refill (struct slab_alloc* allocator) {
    
    debug_printf ("Refill stuff\n");
    // Reserve 32 pages (128 KiB);
    size_t pages = 256;
    size_t bytes = pages * 4096;
    
    void* buf;
    // Need to reserve virtual space.
    errval_t err = paging_alloc (&current, &buf, bytes);
    
    page_in ((lvaddr_t) buf, false, pages);
    
    if (err_is_ok (err)) {
        slab_grow (allocator, buf, bytes);
    }
    debug_printf ("End refilling\n");
    return err;
}

errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr, struct capref pdir)
{
    debug_printf("paging_init_state, start %X\n", start_vaddr);
//     debug_printf ("Size of struct capref: %u", sizeof(struct capref));
    
    // Make sure we have zeroed out memory.
    memset (st, 0, sizeof (struct paging_state));
    
    // Initialize virtual address space.
    st -> heap_begin = start_vaddr;
    st -> heap_end = start_vaddr;
    
    // Initialize dynamic memory for management.
    slab_init (&(st->ptable_mem), sizeof (struct ptable_lvl2), memory_refill);
    slab_init (&(st->frame_mem), sizeof (struct frame_list), memory_refill);

    // TODO: remove obsolte stuff
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
//     debug_printf ("Got old stack base: %X, old stack top: %X\n", oldbase, oldtop);
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
    debug_printf ("paging_region_init: %X\n", pr);
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
    debug_printf ("paging_region_map: %X, addr %X, current %X, req %u\n", pr, pr->base_addr, pr -> current_addr, req_size);
    // BUG: we may get a region that is not initialized from multi_alloc() (multi_slot_alloc.c, line 106)!
    if (pr -> base_addr == 0) {
        paging_region_init (&current, pr, FRAME_SIZE);
    }
    
    if (pr->base_addr == pr->current_addr) {
        page_in (pr->base_addr, false, (pr->region_size)/PAGE_SIZE);
        *retbuf = (void*) pr -> base_addr;
        *ret_size = pr -> region_size;
        pr -> current_addr += *ret_size;
        debug_printf ("paging_region_map_success\n");
        return SYS_ERR_OK;
    } else {
        return LIB_ERR_VSPACE_MMU_AWARE_NO_SPACE;
    }

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
    assert (st = &current);
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
