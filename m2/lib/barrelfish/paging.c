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

#include <barrelfish/aos_dbg.h>

// Flags for newly mapped pages.
#define FLAGS (KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE)

// Default page count for refill requests by slab data structures.
#define SLAB_REFILL_PAGE_COUNT 64u

// Predefined paging region for slot allocator.
// This is defined by us as a workaround to a bug.
// TODO Which size?? Experiments show that it needs about 32 pages for an 48MB array.
#define SLOT_REGION_SIZE FRAME_SIZE

// A global paging state instance.
static struct paging_state current;

// The exception stack for the first user-level thread.
static char e_stack[EXCEPTION_STACK_SIZE];
static char* e_stack_top = e_stack + EXCEPTION_STACK_SIZE;

// Forward declarations.
static errval_t paging_handle_pagefault (struct paging_state* state, lvaddr_t addr);
static errval_t paging_allocate_ptable (struct paging_state* state, uint32_t l2_index);
static errval_t arml2_alloc(struct capref *ret);
static errval_t paging_map_eagerly (struct paging_state* state, lvaddr_t base_addr, uint32_t page_count);
static errval_t memory_refill (struct slab_alloc* allocator);

/**
 * \brief Helper function that allocates a slot and
 *        creates a ARM l2 page table capability
 */
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

/**
 * Exception handler for barrelfish.
 */
static void my_exception_handler (enum exception_type type, int subtype,
                                     void *addr, arch_registers_state_t *regs,
                                     arch_registers_fpu_state_t *fpuregs) 
{
    // Print a small message, otherwise terminal fills up and computation takes ages.
    debug_print_short ("?");
//     debug_printf ("Exception type %u, subtype %u, addr %X\n", type, subtype, addr);

    lvaddr_t vaddr = (lvaddr_t) addr;

    if (type == EXCEPT_PAGEFAULT 
        && subtype == PAGEFLT_NULL
        && current.heap_begin <= vaddr 
        && vaddr < current.heap_end )
    {
        errval_t err = paging_handle_pagefault (&current, (lvaddr_t) addr);
        if (err_is_ok (err)) {
            debug_print_short ("!");
        } else {
            debug_print_short ("XXXXX");
        }
    } else {
        debug_printf ("Invalid address!\n");
        abort();
    }
}

/**
 * \brief Handle the page fault at address `addr'.
 *
 * \param state: The current paging state.
 * \param addr: The address at which the page fault occured.
 */
static errval_t paging_handle_pagefault (struct paging_state* state, lvaddr_t addr)
{
    int l1_index = ARM_L1_USER_OFFSET(addr);
    int l2_index = ARM_L2_USER_OFFSET(addr);
    errval_t error = 0;

    // Allocate a second-level page table if necessary.
    if ( ! state -> ptables [l1_index] ) {
        error = paging_allocate_ptable (state, l1_index);
    }

    if (err_is_ok (error)) {

        // Check if we need to allocate a new frame.
        if (state->flist_tail == NULL // => No frame available yet.
                || state->flist_tail->next_free_slot >= FRAME_SLOTS) // => Frame is full.
        {
            debug_print_short ("..+..");

            // Unfortunately we need to allocate a frame.
            // Get a new frame of default size FRAME_SIZE.

            struct capref new_frame;
            error = frame_alloc (&new_frame, FRAME_SIZE, NULL);

            if (err_is_ok (error)) {

                // Grab some dynamic memory for the list.
                struct frame_list* node = slab_alloc ( &(state->frame_mem));

                // The allocation should succeed as the allocator has a refill function.
                assert (node);

                // Initialize the node.
                init_frame_list (node);
                node -> frame = new_frame;

                // Add our newly allocated node to the frame list.
                if (state->flist_tail && state->flist_head) {
                    // Extend the list.
                    state->flist_tail -> next = node;
                    state->flist_tail = node;
                    state->flist_head = node;
                } else {
                    // We're the first node.
                    assert (state->flist_tail == NULL && state->flist_head == NULL);
                    state->flist_head = node;
                    state->flist_tail = node;
                }
                debug_printf_quiet ("Allocated a new frame of size 0x%X\n", FRAME_SIZE);
            } else {
                // Allocation was not successful.
                cap_destroy (new_frame);
            }
        }

        if (err_is_ok (error)) {
            // We have a guarantee now that there's a frame and a
            // second-level page table with some free capacity.
            struct capref base_frame = state -> flist_tail -> frame;
            struct capref cap_l2 = state -> ptables [l1_index] -> lvl2_cap;

            // Due to semantics we need to create a copy of the frame capability.
            struct capref copied_frame;
            slot_alloc (&copied_frame);
            error = cap_copy (copied_frame, base_frame);

            // Get the slot number.
            int slot = state -> flist_tail -> next_free_slot;

            if (err_is_ok (error)) {
                // Map the page to the free frame slot.
                error = vnode_map(cap_l2, copied_frame, l2_index, FLAGS, slot*PAGE_SIZE, 1);
            }


            if (err_is_ok (error)) {

                // Now we need to do some additional bookkeeping.
                state -> flist_tail -> next_free_slot = slot + 1;
                state -> flist_tail -> pages [slot] = (addr & (PAGE_SIZE-1)); // page aligned address.

                // At this point we would also have to check if the page
                // is currently paged out to disk.

                if (state -> ptables [l1_index] -> page_exists [l2_index]) {
                    // Uh-oh, paging in from disk not implemented yet.
                    USER_PANIC ("Paging in from persistent memory not implemented\n");
                } else {
                    state -> ptables [l1_index] -> page_exists [l2_index] = true;
                }

            } else {
                // Copy or mapping failed. Clean up any leftovers.
                cap_destroy (copied_frame);
            }
        }
    }

    if (err_is_fail(error)) {
        debug_printf ("Error while handling pagefault!\n");
    }
    return error;
}

/**
 * \brief Allocate a second-level page table at the specified index.
 * This function only works if the page table to be allocated doesn't exist yet.
 *
 * NOTE: This function might be called recursively in slab allocators.
 *
 * \param state: The current paging state.
 * \param l1_index: The index at which the second-level page table is located.
 */
static errval_t paging_allocate_ptable (struct paging_state* state, uint32_t l1_index)
{
    PRINT_ENTRY;
    debug_print_short ("..*..");
    assert (state -> ptables [l1_index] == NULL);

    errval_t error = SYS_ERR_OK;

    // Ask for a new page second-level table.
    struct capref l2_cap;
    error = arml2_alloc(&l2_cap);

    if (err_is_ok (error)) {

        // Get the predefined capability for the first-level page table.
        struct capref l1_cap = state -> ptable_lvl1_cap;
        /*struct capref l1_cap = (struct capref) {
            .cnode = cnode_page,
            .slot = 0,
        };*/

        // Now map the page table.
        error = vnode_map(l1_cap, l2_cap, l1_index, FLAGS, 0, 1);

        if (err_is_fail (error)) {
            // Mapping failed!
            // Free resources and return.
            cap_destroy (l2_cap);

        } else {
            // If everything succeeded we can add the table to our paging state.

            // Now it's getting messy... We need to allocate some dynamic memory
            // for the new page table descriptor. If the slab allocator runs
            // out of space however it will call memory_refill(), which again
            // might call paging_allocate_ptable!

            // Therefore we initialize the structure on the stack first and only copy
            // it to dynamic memory when initialization succeeds.

            struct ptable_lvl2 stack_allocated_descriptor;
            init_ptable_lvl2 (&stack_allocated_descriptor);

            stack_allocated_descriptor.lvl2_cap = l2_cap;
            state -> ptables [l1_index] = &stack_allocated_descriptor;

            // Let's do the allocation.
            struct ptable_lvl2* new_ptable = NULL;
            new_ptable = (struct ptable_lvl2*) slab_alloc ( &(state->ptable_mem) );

            // As we have a refill function for the allocator it should not run out of space.
            assert (new_ptable != NULL);

            // Copy over our stack-allocated descriptor.
            memcpy (new_ptable, &stack_allocated_descriptor, sizeof (struct ptable_lvl2));

            // Finally, we need to update the pointer.
            state -> ptables [l1_index] = new_ptable;
        }
    }
    PRINT_EXIT (error);
    return error;
}

/**
 * Map `page_count' pages to a physical frame starting from `base_addr'.
 *
 * \param state: The current paging state.
 * \param base_addr: The virtual base address. Must be page aligned.
 * \param page_count: The number of pages to map.
 */
static errval_t paging_map_eagerly (struct paging_state* state, lvaddr_t base_addr, uint32_t page_count)
{
    PRINT_ENTRY;

    // Enforce page alignment restriction.
    assert ((base_addr & (PAGE_SIZE-1)) == 0);


    errval_t error = SYS_ERR_OK;


    // First ask for a frame of correct size.
    size_t requested_size = page_count * PAGE_SIZE;
    struct capref new_frame;
    error = frame_alloc(&new_frame, requested_size, NULL);

    uint32_t remaining_pages = page_count;
    lvaddr_t mapped_addr = base_addr;

    while (remaining_pages > 0 && err_is_ok (error)) {

        // Now map all pages to the correct second-level page table.
        int l1_index = ARM_L1_USER_OFFSET (mapped_addr);
        int l2_index = ARM_L2_USER_OFFSET (mapped_addr);

        // Allocate a second-level page table if necessary.
        if ( ! state->ptables [l1_index] ) {
            error = paging_allocate_ptable (state, l1_index);
        }

        if (err_is_ok (error)) {

            // Get the capability for the second-level page table.
            struct capref cap_l2 = state -> ptables [l1_index] -> lvl2_cap;

            // Figure out how many page table entries are still free.
            uint32_t free_slots = ARM_L2_USER_ENTRIES - l2_index;

            if (free_slots <= remaining_pages) {
                debug_printf_quiet ("paging_map_eagerly: page region spans over multiple ptables\n");
                // ouch, need to allocate another page table...
                // Map at least part of it.
                struct capref copied_frame;
                slot_alloc (&copied_frame);
                error = cap_copy (copied_frame, new_frame);

                if (err_is_ok (error)) {
                    error = vnode_map (cap_l2, copied_frame, l2_index, FLAGS, 0, free_slots);
                    remaining_pages -= free_slots;
                    mapped_addr += free_slots * PAGE_SIZE;
                } else {
                    cap_destroy (copied_frame);
                }
            } else {
                // Everything fits into the current page table.
                // Map it to the specified addresses.
                error = vnode_map (cap_l2, new_frame, l2_index, FLAGS, 0, remaining_pages);
                remaining_pages = 0;
            }
        }
    }

    if (err_is_fail (error)) {
        // Mapping or allocation failed!
        // Return frame cap and report error.
        cap_destroy (new_frame);
    }

    PRINT_EXIT (error);
    return error;
}

/**
 * Refill memory for slab allocators.
 * By default always refills with SLAB_REFILL_PAGE_COUNT pages.
 */
static errval_t memory_refill (struct slab_alloc* allocator)
{
    PRINT_ENTRY;
    debug_printf_quiet ("memory_refill on state %p\n", get_current_paging_state());

    // Initialize constants.
    size_t pages = SLAB_REFILL_PAGE_COUNT;
    size_t bytes = pages * PAGE_SIZE;

    // Need to reserve virtual space.
    void* buf;
    errval_t err = paging_alloc (get_current_paging_state(), &buf, bytes);

    // Map the allocated virtual space.
    // This is needed as the memory is used for page table
    // management (i.e. by the page fault handler), and a double
    // page fault would be deadly.
    if (err_is_ok (err)) {
        err = paging_map_eagerly (get_current_paging_state(), (lvaddr_t) buf, pages);
    }

    if (err_is_ok (err)) {
        slab_grow (allocator, buf, bytes);
    }

    PRINT_EXIT (err);
    return err;
}

/**
 * Initialize a paging_state struct.
*/
errval_t paging_init_state (struct paging_state *st, lvaddr_t start_vaddr, struct capref pdir)
{
    PRINT_ENTRY;
    debug_printf_quiet ("paging_init_state: state %p, start %X\n", st, start_vaddr);

    // Make sure we have zeroed out memory.
    memset (st, 0, sizeof (struct paging_state));

    // Initialize virtual address space.
    st -> heap_begin = start_vaddr;
    st -> heap_end = start_vaddr;

    // Initialize ptable struct.
    st -> ptable_lvl1_cap = pdir;

    // Initialize dynamic memory for management.
    slab_init (&(st->ptable_mem), sizeof (struct ptable_lvl2), memory_refill);
    slab_init (&(st->frame_mem), sizeof (struct frame_list), memory_refill);

    slab_init (&(st->exception_stack_mem), EXCEPTION_STACK_SIZE, memory_refill);

    errval_t error = SYS_ERR_OK;
    PRINT_EXIT (error);
    return error;
}


errval_t paging_init(void)
{
    PRINT_ENTRY;

    // Initialize self-paging handler.
    // TIP: use thread_set_exception_handler() to setup a page fault handler
    errval_t error = thread_set_exception_handler (&my_exception_handler, NULL, e_stack, e_stack_top, NULL, NULL);

    // TIP: Think about the fact that later on, you'll have to make sure that
    // you can handle page faults in any thread of a domain.
    // TODO: Do we need mutexes? At the moment all state is accessed on a single core and only when the thread yields the CPU.

    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.

    //TODO check if VADDR_OFFSET is ok as the start of our heap.

    struct capref pdir = (struct capref) {
        .cnode = cnode_page,
        .slot = 0,
    };
    paging_init_state (&current, VADDR_OFFSET, pdir);
    assert (get_current_paging_state() == 0);

    set_current_paging_state(&current);

    PRINT_EXIT (error);
    return error;
}


/**
 * \brief Set up exception handler and exception stack for thread t.
 */
void paging_init_onthread(struct thread *t)
{
    PRINT_ENTRY;

    void* new_stack = slab_alloc ( &(current.exception_stack_mem));
    assert (new_stack != NULL);

    t -> exception_handler = my_exception_handler;
    t -> exception_stack = new_stack;
    t -> exception_stack_top = new_stack + EXCEPTION_STACK_SIZE;

    PRINT_EXIT_VOID;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_init(struct paging_state *st, struct paging_region *pr, size_t size)
{
    PRINT_ENTRY;

    // Align requested size to page boundary.
    int aligned_size = ((size-1) & ~(PAGE_SIZE-1)) + PAGE_SIZE;

    void *base;
    errval_t error = paging_alloc(st, &base, aligned_size);

    if (err_is_fail(error)) {

        error =  err_push (error, LIB_ERR_VSPACE_MMU_AWARE_INIT);
        debug_printf ("paging_region_init: paging_alloc failed\n");

    } else {

        pr->base_addr    = (lvaddr_t)base;
        pr->current_addr = pr->base_addr;
        pr->region_size  = aligned_size;
        // TODO: maybe add paging regions to paging state?
    }

    PRINT_EXIT (error);
    return error;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size)
{
    PRINT_ENTRY;

    // BUG: we may get a region that is not initialized
    // from multi_alloc() (multi_slot_alloc.c, line 106)!
    if (pr -> base_addr == 0) {
        debug_printf ("paging_region_map: Got NULL struct! struct %X, base_addr %X, current %X, req %u\n",
                      pr,
                      pr->base_addr,
                      pr -> current_addr,
                      req_size);
        paging_region_init (get_current_paging_state(), pr, SLOT_REGION_SIZE);
    }

    lvaddr_t end_addr = pr->base_addr + pr->region_size;
    size_t remaining = end_addr - pr->current_addr;

    assert ((pr->region_size & (PAGE_SIZE-1)) == 0); // region size should be page aligned.
    assert ((remaining & (PAGE_SIZE-1)) == 0); // remaining bytes should be page aligned.

    uint32_t pages_to_map = 0;

    if (req_size < remaining) {

        // Align to pages.
        pages_to_map = req_size / PAGE_SIZE + 1;

    } else if (remaining > 0) {

        pages_to_map = remaining / PAGE_SIZE;
        debug_printf("exhausted paging region, expect badness on next allocation\n");
    } else {
        return LIB_ERR_VSPACE_MMU_AWARE_NO_SPACE;
    }

    errval_t error = paging_map_eagerly (get_current_paging_state(), pr -> current_addr, pages_to_map);

    if (err_is_ok (error)) {
        *retbuf = (void*)pr->current_addr;
        *ret_size = pages_to_map * PAGE_SIZE;
        pr->current_addr += *ret_size;
    }

    PRINT_EXIT (error);
    return error;
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
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes)
{
    debug_printf_quiet ("paging_alloc: state %p, bytes %X...\n", st, bytes);
 //   assert (st == &current);
    assert ((bytes & (PAGE_SIZE-1)) == 0);

    *buf = (void*)  st -> heap_end;
    st -> heap_end += bytes;
    debug_printf_quiet ("paging_alloc: state %p, bytes %X, start %p.\n", st, bytes, *buf);
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
    PRINT_ENTRY;
    debug_printf_quiet ("paging_map_frame_attr: state %p\n", st);
    errval_t err = paging_alloc(st, buf, bytes);
    if (err_is_fail(err)) {
        return err;
    }
    err = paging_map_fixed_attr(st, (lvaddr_t)(*buf), frame, bytes, flags);

    PRINT_EXIT (err);
    return err;
}


/**
 * \brief map a user provided frame at user provided VA.
 */
errval_t paging_map_fixed_attr(struct paging_state *state, lvaddr_t vaddr,
        struct capref frame, size_t bytes, int flags)
{
    // TODO: Check if we can merge paging_map_eagerly with this function.

    // from handout: you will need this functionality in later assignments. Try to
    // keep this in mind when designing your self-paging system.

    PRINT_ENTRY;
    debug_printf_quiet ("paging_map_fixed_attr: state %p (%p -> %p), addr %X\n", state, state->heap_begin, state->heap_end, vaddr);

    // Enforce page alignment restriction.
    assert ((vaddr & (PAGE_SIZE-1)) == 0);
    assert (bytes % PAGE_SIZE == 0);

    // Enforce that frame size >= bytes.
    // TODO

    errval_t error = SYS_ERR_OK;

    uint32_t page_count = bytes / PAGE_SIZE;

    uint32_t remaining_pages = page_count;
    lvaddr_t mapped_addr = vaddr;

    while (remaining_pages > 0 && err_is_ok (error)) {

        // Now map all pages to the correct second-level page table.
        int l1_index = ARM_L1_USER_OFFSET (mapped_addr);
        int l2_index = ARM_L2_USER_OFFSET (mapped_addr);

        // Allocate a second-level page table if necessary.
        if ( ! state->ptables [l1_index] ) {
            error = paging_allocate_ptable (state, l1_index);
        }

        if (err_is_ok (error)) {

            // Get the capability for the second-level page table.
            struct capref cap_l2 = state -> ptables [l1_index] -> lvl2_cap;

            // Figure out how many page table entries are still free.
            uint32_t free_slots = ARM_L2_USER_ENTRIES - l2_index;

            if (free_slots <= remaining_pages) {
                debug_printf_quiet ("paging_map_fixed_attr: page region spans over multiple ptables\n");
                // ouch, need to allocate another page table...
                // Map at least part of it.
                struct capref copied_frame;
                slot_alloc (&copied_frame);
                error = cap_copy (copied_frame, frame);

                if (err_is_ok (error)) {
                    error = vnode_map (cap_l2, copied_frame, l2_index, flags, 0, free_slots);
                    remaining_pages -= free_slots;
                    mapped_addr += free_slots * PAGE_SIZE;
                } else {
                    cap_destroy (copied_frame);
                }
            } else {
                // Everything fits into the current page table.
                // Map it to the specified addresses.
                error = vnode_map (cap_l2, frame, l2_index, flags, 0, remaining_pages);
                remaining_pages = 0;
            }
        }
    }

//     if (err_is_fail (error)) {
//         // Mapping or allocation failed!
//         // Return frame cap and report error.
//         cap_destroy (new_frame);
//     }
//     debug_printf ("paging :: %s\n", err_getstring (error));
    PRINT_EXIT (error);
    return error;
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
