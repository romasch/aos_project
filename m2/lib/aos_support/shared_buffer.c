#include <aos_support/shared_buffer.h>
#include <barrelfish/capabilities.h>
#include <barrelfish/paging.h>

//TODO: Use a dynamic structure.
#define MAX_BUFFER_COUNT 64

struct buffer_manager_entry {
    bool is_used;
    uint8_t frame_size_bits;
    void* virtual_address;
    struct capref frame_capability;
};

static struct buffer_manager_entry buffer_manager [MAX_BUFFER_COUNT];


// Map a user-provided frame into our address space.
errval_t map_shared_buffer (struct capref frame, uint32_t* memory_descriptor)
{
    uint32_t free_index = -1;
    uint8_t size_bits = 0;
    errval_t error = SYS_ERR_OK;

    // Find a free slot in the buffer table.
    for (int i=0; i < MAX_BUFFER_COUNT && free_index == -1; i++) {
        if ( !buffer_manager[i].is_used ) {
            free_index = i;
        }
    }

    if (free_index < 0) {
        error = -1; // TODO: Find a suitable error.
    }

    // Check if the user-provided frame has a correct size.
    if (err_is_ok (error)) {
        struct frame_identity frame_size;
        error = invoke_frame_identify (frame, &frame_size);
        size_bits = frame_size.bits;
    }

    // Reserve some virtual space if needed.
    if (err_is_ok (error) && buffer_manager [free_index].virtual_address == NULL) {
        void* vaddr = 0;
        error = paging_alloc (get_current_paging_state(), &vaddr, (1UL << size_bits));
        buffer_manager [free_index].virtual_address = vaddr;
    }

    // Map the frame.
    if (err_is_ok (error)) {
        lvaddr_t vaddr = (lvaddr_t) buffer_manager [free_index].virtual_address;
        error = paging_map_fixed (get_current_paging_state(), vaddr, frame, (1UL << size_bits));
    }

    // Update data structures and return values.
    if (err_is_ok (error)) {
        buffer_manager [free_index].is_used = true;
        buffer_manager [free_index].frame_size_bits = size_bits;
        buffer_manager [free_index].frame_capability = frame;
        if (memory_descriptor) {
            *memory_descriptor = free_index;
        }
    }

    return error;
}

// Return handle of the buffer.
errval_t get_shared_buffer (uint32_t memory_descriptor, void** return_buffer, uint32_t* return_size)
{
    errval_t error = SYS_ERR_OK;

    if (0 <= memory_descriptor
        && memory_descriptor < MAX_BUFFER_COUNT
        && buffer_manager [memory_descriptor].is_used)
    {
        if (return_buffer) {
            *return_buffer = buffer_manager [memory_descriptor].virtual_address;
        }
        if (return_size) {
            uint8_t size_bits = buffer_manager [memory_descriptor].frame_size_bits;
            *return_size = (1UL << size_bits);
        }
    }
    else {
        error = -1; // TODO: Find suitable error.
    }
    return error;
}


// Unmap the shared buffer from our address space.
errval_t unmap_shared_buffer (uint32_t memory_descriptor)
{
    errval_t error = -1; // TODO; Find suitable error.
    return error; // TODO: Implement this function.

    if (0 <= memory_descriptor
        && memory_descriptor < MAX_BUFFER_COUNT
        && buffer_manager [memory_descriptor].is_used)
    {
        assert ( !capref_is_null (buffer_manager [memory_descriptor].frame_capability));
        // TODO: We need to support unmap operations in the paging code...

        buffer_manager [memory_descriptor].is_used = false;
        buffer_manager [memory_descriptor].frame_size_bits = 0;
        buffer_manager [memory_descriptor].frame_capability = NULL_CAP;
    }
    return error;
}

void test_shared_buffer (void)
{
    errval_t error = SYS_ERR_OK;
    struct capref frame;

    error = frame_alloc (&frame, 4096, 0);

    debug_printf ("First ram cap done: %s\n", err_getstring (error));
    assert (err_is_ok (error));
    uint32_t md;
    error = map_shared_buffer (frame, &md);
    assert (err_is_ok (error));
    debug_printf ("First mapping done\n");
    void* buffer;
    error = get_shared_buffer (md, &buffer, NULL);
    assert (err_is_ok (error));
    ((char*) buffer) [0] = 'a';
    assert (((char*) buffer) [0] == 'a');

    debug_printf ("Write worked\n");
//     error = unmap_shared_buffer (md);
//     debug_printf ("Unmap worked\n");
//     assert (err_is_ok (error));

    struct capref frame_two;
    error = frame_alloc (&frame_two, 1024*1024, 0);
    debug_printf ("Second ram cap done\n");
    assert (err_is_ok (error));
    error = map_shared_buffer (frame_two, &md);
    assert (err_is_ok (error));
    debug_printf ("Second mapping done\n");
    void* new_buf;
    error = get_shared_buffer (md, &new_buf, NULL);
    assert (err_is_ok (error));
//    error = unmap_shared_buffer (md);
//     assert (err_is_ok (error));
}
