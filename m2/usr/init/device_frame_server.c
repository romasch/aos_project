#include "init.h"

#include <mm/mm.h>

// Predefined node count to refill slab allocator with.
#define DEVICE_NODE_REFILL 64

// Predefined child count for device nodes (in bits)
#define CHILD_COUNT_BITS 1

// Predefined IO space
#define IO_BASE 0x40000000
#define IO_SIZE_BITS 30


static struct mm device_memory_manager;
static struct slot_alloc_basecn device_memory_slot_manager;

/**
 * Refill function for a slab allocator used by device_memory_manager.
 * Internally uses malloc, because dynamic memory allocation should work.
 */
static errval_t device_memory_refill (struct slab_alloc* allocator)
{
    errval_t error = SYS_ERR_OK;

    // Allocate space for more device memory nodes.
    // NOTE: node size depends on maximum number of children.
    size_t size = DEVICE_NODE_REFILL * MM_NODE_SIZE (CHILD_COUNT_BITS);
    void* buf = malloc (size);

    if (buf == NULL) {
        error =  LIB_ERR_MALLOC_FAIL;
        debug_printf ("device_memory_refill: %s\n", err_getstring (error));
    } else {
        slab_grow (allocator, buf, size);
    }
    return error;
}

/**
 * Allocate a device frame from the server.
 *
 * \param physical_base: The start address of the requested frame. Must be page aligned.
 * \param size_bits: The size in bits of the requested frame. Must be at least page sized (i.e. >= 12).
 * \param ret_frame: A pointer to be filled in with the new capability.
 */
errval_t allocate_device_frame (lpaddr_t physical_base, uint8_t size_bits, struct capref* ret_frame)
{
    // Enforce page alignment.
    assert (size_bits >= 12);
    assert ((physical_base & ((1<<12)-1)) == 0);

    // Return variable to ensure correctness.
    uint64_t ret_base = 0;

    // This boundary selection forces the allocator to return the correct page.
    lpaddr_t physical_limit = physical_base + (1UL << size_bits);

    errval_t error = mm_alloc_range (
        &device_memory_manager,
        size_bits,          // Size bits
        physical_base,      // Minimum start address
        physical_limit,     // Maximum address.
        ret_frame,          // Capability to be returned.
        &ret_base           // Base address of allocated dev frame.
    );

    // Check if allocation was successful.
    assert (ret_base == physical_base);

    if (err_is_fail (error)) {
        debug_printf ("allocate_device_frame: %s\n", err_getstring (error));
    }
    return error;
}

/**
 * Initialize the device frame server.
 * The server takes care of the whole IO space
 * defined by IO_BASE and IO_SIZE_BITS.
 *
 * \param io_space_cap: The capability for the whole IO space.
 */
errval_t initialize_device_frame_server (struct capref io_space_cap)
{

    // Initialize the slot allocator used by the device memory manager.
    // TODO: Check if this is the right slot allocator.
    errval_t error = slot_alloc_basecn_init (&device_memory_slot_manager);

    if (err_is_ok (error)) {

        // Initialize device memory manager:
        error = mm_init (
            &device_memory_manager,
            ObjType_DevFrame,       // Memory type
            IO_BASE,                // Base address
            IO_SIZE_BITS,           // Size bits
            CHILD_COUNT_BITS,       // Maximum number of children in bits (i.e. 2 in this case)
            device_memory_refill,   // Slab refill function
            slot_alloc_basecn,      // Slot allocator function (defined in lib/barrelfish/mm/slot_alloc.c)
            &device_memory_slot_manager, // Slot allocator instance for this manager
            true                    // Delete chunked memory nodes (i.e. nodes with children)
        );

    }

    if (err_is_ok (error)) {
        // Add the global device frame cap to the memory manager.
        error = mm_add (&device_memory_manager, io_space_cap, IO_SIZE_BITS, IO_BASE);
    }

    if (err_is_fail (error)) {
        debug_printf ("init_device_memory: %s\n", err_getstring (error));
    }
    return error;
}
