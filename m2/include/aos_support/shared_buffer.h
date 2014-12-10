#ifndef SHARED_BUFFER_H
#define SHARED_BUFFER_H

#include <barrelfish/barrelfish.h>

/**
 * Map a user-provided shared frame.
 *
 * \param frame_capability: The frame to be mapped.
 * \param memory_descriptor: Result parameter for the descriptor of the allocated buffer.
 */
errval_t map_shared_buffer (struct capref frame_capability, uint32_t* memory_descriptor);

/**
 * Get the address and size of a shared buffer.
 *
 * \param memory_descriptor: The descriptor of the buffer.
 * \param result_buffer: Result parameter for the buffer address. May be NULL.
 * \param result_size: Result parameter for the size of the buffer. May be NULL.
 *
 */
errval_t get_shared_buffer (uint32_t memory_descriptor, void** result_buffer, uint32_t* result_size);

/**
 * Unmap an already mapped shared buffer.
 * TODO: This function is not implemented yet, as it needs some heavy changes in the paging code.
 *
 * \param memory_descriptor: The descriptor of the buffer.
 */
errval_t unmap_shared_buffer (uint32_t memory_descriptor);


/// Small test routine
void test_shared_buffer (void);
#endif