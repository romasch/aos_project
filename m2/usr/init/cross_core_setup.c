#include "init.h"

#include <barrelfish_kpi/arm_core_data.h>
#include <elf/elf.h>

#include <barrelfish/paging.h>
#include <aos_support/module_manager.h>


/// Virtual address of the shared buffer.
static void* cross_core_buffer = NULL;

/**
 * Get the shared cross core buffer.
 */
void* get_cross_core_buffer (void)
{
    return cross_core_buffer;
}

/**
 * Initialize the kernel-allocated cross core buffer.
 */
errval_t init_cross_core_buffer (void)
{
    errval_t error = SYS_ERR_OK;

    // Get the shared frame from the predefined slot.
    struct capref shared_frame = cap_initep;
    shared_frame.slot = TASKCN_SLOT_MON_URPC;

    // Map it into our address space.
    error =  paging_map_frame_attr (
            get_current_paging_state(),
            &cross_core_buffer,
            BASE_PAGE_SIZE,
            shared_frame,
            VREGION_FLAGS_READ_WRITE_NOCACHE,
            NULL, NULL);

    return error;
}


/**
 * Allocate and identify a frame.
 *
 * \param size: In-Out argument for the frame size.
 * \param capability: Return parameter for the frame capability.
 * \param frameid: Return parameter for the frame identity.
 */
static errval_t frame_prepare (size_t* size, struct capref* capability, struct frame_identity* frameid)
{
    errval_t error = SYS_ERR_OK;
    error = frame_alloc (capability, *size, size);

    if (err_is_ok (error)) {
        error = invoke_frame_identify(*capability, frameid);
    }

    return error;
}


/// Dummy function for an already allocated frame.
static errval_t dummy_elfload_allocate (void *state, genvaddr_t base, size_t size, uint32_t flags, void **retbase)
{
    *retbase = state + base;
    return SYS_ERR_OK;
}

/// Load an ELF into 'target' and relocate to 'reloc_dest'.
static errval_t elf_load_and_relocate(lvaddr_t blob_start, size_t blob_size, void *target, lvaddr_t reloc_dest, uintptr_t *reloc_entry)
{
    errval_t error = SYS_ERR_OK;
    struct Elf32_Shdr* symhead = NULL;
    struct Elf32_Shdr* rel = NULL;
    struct Elf32_Shdr* symtab = NULL;

    struct Elf32_Ehdr* head = (struct Elf32_Ehdr *)blob_start;
    genvaddr_t elf_vbase = elf_virtual_base (blob_start);
    void* dummy_elfload_allocate_value = target - elf_vbase;

    genvaddr_t entry = 0;
    error = elf_load (
            head->e_machine,
            dummy_elfload_allocate,
            dummy_elfload_allocate_value,
            blob_start,
            blob_size,
            &entry);

    if (err_is_ok(error)) {

        // Relocate to new physical base address
        symhead = (struct Elf32_Shdr *)(blob_start + (uintptr_t)head->e_shoff);
        rel     = elf32_find_section_header_type(symhead, head->e_shnum, SHT_REL);
        symtab  = elf32_find_section_header_type(symhead, head->e_shnum, SHT_DYNSYM);

        if (rel && symtab) {

            elf32_relocate (
                reloc_dest,
                elf_vbase,
                (struct Elf32_Rel *) (blob_start + rel -> sh_offset),
                rel->sh_size,
                (struct Elf32_Sym *) (blob_start + symtab -> sh_offset),
                symtab->sh_size,
                elf_vbase,
                target
            );

            *reloc_entry = entry - elf_vbase + reloc_dest;

        } else {
            // Something seems to be wrong with the ELF image.
            // TODO: Check if there's a more suitable error.
            error = ELF_ERR_PROGHDR;
        }
    }

    return error;
}


/// Spawn a new kernel on core 'cid'.
errval_t spawn_core(coreid_t cid)
{
    assert(sizeof(struct arm_core_data) <= BASE_PAGE_SIZE);

    errval_t err = SYS_ERR_OK;

    // Load the kernel binary.
    struct module_info* info = NULL;
    err = module_manager_load ("cpu_omap44xx", &info);

    if (err_is_fail (err)) {
        return err;
    }

    // Allocate memory for cpu driver:
    // We allocate a page for arm_core_data and the rest for the elf image.
    size_t kernel_memory_size = elf_virtual_size(info -> virtual_address) + BASE_PAGE_SIZE;
    void* kernel_memory_buffer = NULL;
    struct capref kernel_memory_capability;
    struct frame_identity kernel_frame_identity;

    err = frame_prepare (&kernel_memory_size, &kernel_memory_capability, &kernel_frame_identity);

    if (err_is_ok (err)) {

        // Map the frame into our address space.
        err = paging_map_frame_attr (
                get_current_paging_state(),
                &kernel_memory_buffer,
                kernel_memory_size,
                kernel_memory_capability,
                VREGION_FLAGS_READ_WRITE_NOCACHE | VREGION_FLAGS_EXECUTE,
                NULL, NULL);

    }

    if (err_is_fail (err)) {
        cap_destroy (kernel_memory_capability);
        return err;
    }

    // TODO: Apparently this whole setup for the core data struct is not necessary...
/*
    // Chunk of memory for app core
    struct capref         spawn_mem_cap    ;
    struct frame_identity spawn_mem_frameid;
    size_t size = ARM_CORE_DATA_PAGES * BASE_PAGE_SIZE;


    err = frame_prepare (&size, &spawn_mem_cap, &spawn_mem_frameid);
    if (!err_is_ok(err)) {
        return err;
    }

    // Setup the core_data struct in the new kernel
    struct arm_core_data* core_data = (struct arm_core_data *) kernel_memory_buffer;
    struct Elf32_Ehdr   * head32    = (struct Elf32_Ehdr    *) info -> virtual_address;

    core_data->elf.size = sizeof(struct Elf32_Shdr)                  ;
    core_data->elf.addr = info -> physical_address + (uintptr_t)head32->e_shoff;
    core_data->elf.num  = head32->e_shnum                            ;


    core_data->module_start        = info -> physical_address;
    core_data->module_end          = info -> physical_address + info -> size;
    core_data->memory_base_start   = spawn_mem_frameid.base        ;
    core_data->memory_bits         = spawn_mem_frameid.bits        ;
    core_data->src_core_id         = my_core_id                    ;
    */
    uintptr_t reloc_entry;
    err = elf_load_and_relocate (info -> virtual_address,
                                info -> size,
                                kernel_memory_buffer + BASE_PAGE_SIZE,
                                kernel_frame_identity.base + BASE_PAGE_SIZE,
                                &reloc_entry);
    assert (err_is_ok (err));

    return sys_boot_core (cid, reloc_entry);
}