/**
 * \file
 * \brief Barrelfish paging helpers.
 */

/*
 * Copyright (c) 2012, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */


#ifndef LIBBARRELFISH_PAGING_H
#define LIBBARRELFISH_PAGING_H

#include <errors/errno.h>
#include <barrelfish/capabilities.h>

typedef int paging_flags_t;

#define VADDR_OFFSET ((lvaddr_t)1UL*1024*1024*1024) // 1GB

#define SLAB_BUFSIZE 16

// why on earth doesn't this match the actual ARM mmu engine?!
#define ARM_L1_USER_OFFSET(addr) ((addr)>>22 & 0x3ff)
#define ARM_L1_USER_ENTRIES 1024u
#define ARM_L2_USER_OFFSET(addr) ((addr)>>12 & 0x3ff)
#define ARM_L2_USER_ENTRIES 1024u

#define VREGION_FLAGS_READ     0x01 // Reading allowed
#define VREGION_FLAGS_WRITE    0x02 // Writing allowed
#define VREGION_FLAGS_EXECUTE  0x04 // Execute allowed
#define VREGION_FLAGS_NOCACHE  0x08 // Caching disabled
#define VREGION_FLAGS_MPB      0x10 // Message passing buffer
#define VREGION_FLAGS_GUARD    0x20 // Guard page
#define VREGION_FLAGS_MASK     0x2f // Mask of all individual VREGION_FLAGS

#define VREGION_FLAGS_READ_WRITE \
    (VREGION_FLAGS_READ | VREGION_FLAGS_WRITE)
#define VREGION_FLAGS_READ_EXECUTE \
    (VREGION_FLAGS_READ | VREGION_FLAGS_EXECUTE)
#define VREGION_FLAGS_READ_WRITE_NOCACHE \
    (VREGION_FLAGS_READ | VREGION_FLAGS_WRITE | VREGION_FLAGS_NOCACHE)
#define VREGION_FLAGS_READ_WRITE_MPB \
    (VREGION_FLAGS_READ | VREGION_FLAGS_WRITE | VREGION_FLAGS_MPB)

    
#define FRAME_SIZE (1024u*1024u)
#define PAGE_SIZE (4*1024)
#define FRAME_SLOTS 256u
struct frame_list;

// We only need to store if the page has been mapped at some point.
// (Not mapped + Pagefault) -> allocate new, empty frame.
// (Mapped + Pagefault) -> Paged out. Allocate frame & retrieve from disk.
struct ptable_lvl2 {
    struct capref lvl2_cap;
    bool page_exists [ARM_L2_USER_ENTRIES];
};

// struct to store the paging status of a process
struct paging_state {

    // Virtual address space management:

    // Keep track of reserved (valid) heap addresses.
    // Valid addresses are in the range:
    //      heap_begin <= x < heap_end
    // NOTE: We don't support recycling of virtual addresses.
    // Malloc usually doesn't return them anyway.
    lvaddr_t heap_begin;
    lvaddr_t heap_end;

    // Page table management:

    // Keep track of allocated second-level page tables.
    // A first-level page table always has ARM_L1_USER_ENTRIES entries.
    struct ptable_lvl2* ptables [ARM_L1_USER_ENTRIES];
    // ptable_mem is a simple memory manager for second-level page tables
    struct slab_alloc ptable_mem;
    // Temporary space for a lvl2 page table to avoid some endless recursion.
    struct ptable_lvl2 temp_ptable;

    // Frame management:

    // Allocated frames are stored in a singly linked list.
    // That way pages can be unmapped in a FIFO way
    // if physical memory gets scarce.
    // NOTE: Remove from head, insert at end.
    struct frame_list* flist_head;
    struct frame_list* flist_tail;
    // Simple memory manager for frame list.
    struct slab_alloc frame_mem;

    // The rest is obsolete:
    lvaddr_t heap_mapped;
    uint32_t last_l1_index;
    struct capref last_l2_table;
    // Store the current frame to be filled.
    // Frames are always 1 MiB big and have [0..255] 4KiB slots.
    uint32_t last_frame_slot;
    struct capref current_frame;
};


void init_ptable_lvl2 (struct ptable_lvl2* ptable);

// For each frame, store frame cap & pages residing on this frame.
// Need to call cap_revoke() when unmapping the frame, as copies are not stored.
struct frame_list {
    struct capref frame;
    uint32_t next_free_slot;
    lvaddr_t pages [FRAME_SIZE / PAGE_SIZE]; // address is enough to identify a page.
    struct frame_list* next;
};
void init_frame_list (struct frame_list* node);



struct thread;
/// Initialize paging_state struct
errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir);
/// initialize self-paging module
errval_t paging_init(void);
/// setup paging on new thread (used for user-level threads)
void paging_init_onthread(struct thread *t);

struct paging_region {
    lvaddr_t base_addr;
    lvaddr_t current_addr;
    size_t region_size;
};

errval_t paging_region_init(struct paging_state *st,
                            struct paging_region *pr, size_t size);

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size);
/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * We ignore unmap requests right now.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, size_t bytes);

/**
 * \brief Find a bit of free virtual address space that is large enough to
 *        accomodate a buffer of size `bytes`.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes);

/**
 * Functions to map a user provided frame.
 */
/// Map user provided frame with given flags while allocating VA space for it
errval_t paging_map_frame_attr(struct paging_state *st, void **buf,
                    size_t bytes, struct capref frame,
                    int flags, void *arg1, void *arg2);
/// Map user provided frame at user provided VA with given flags.
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
                struct capref frame, size_t bytes, int flags);

/**
 * \brief unmap a user provided frame
 * NOTE: this function is currently here to make libbarrelfish compile. As
 * noted on paging_region_unmap we ignore unmap requests right now.
 */
errval_t paging_unmap(struct paging_state *st, const void *region);


/// Map user provided frame while allocating VA space for it
static inline errval_t paging_map_frame(struct paging_state *st, void **buf,
                size_t bytes, struct capref frame, void *arg1, void *arg2)
{
    return paging_map_frame_attr(st, buf, bytes, frame,
            VREGION_FLAGS_READ_WRITE, arg1, arg2);
}

/// Map user provided frame at user provided VA.
static inline errval_t paging_map_fixed(struct paging_state *st, lvaddr_t vaddr,
                struct capref frame, size_t bytes)
{
    return paging_map_fixed_attr(st, vaddr, frame, bytes,
            VREGION_FLAGS_READ_WRITE);
}

static inline lvaddr_t paging_genvaddr_to_lvaddr(genvaddr_t genvaddr) {
    return (lvaddr_t) genvaddr;
}

#endif // LIBBARRELFISH_PAGING_H
