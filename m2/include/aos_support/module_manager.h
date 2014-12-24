#ifndef MODULE_MANAGER_H
#define MODULE_MANAGER_H

#include <barrelfish/barrelfish.h>
#include <barrelfish/aos_rpc.h>

// Default prefix of module names on the pandaboard.
#define BINARY_PREFIX "armv7/sbin/"

/**
 * Information about a loaded module.
 * NOTE: 'name' and 'module' are owned by this struct.
 * Whenever the struct gets freed, those have to be freed as well.
 */
struct module_info {
    char* name;
    size_t size;
    lvaddr_t virtual_address;
    genpaddr_t physical_address;
    struct mem_region* module;
};

/**
 * Initialize the module manager.
 *
 * \param bi: Pointer to the bootinfo struct.
 */
errval_t module_manager_init (struct bootinfo* bi);

/**
 * Enable ELF loading from the file system.
 *
 * \param fs_channel: The channel to the file system driver.
 */
void module_manager_enable_filesystem (struct aos_rpc* fs_channel);

/**
 * Load a module from the module cache or the bootinfo struct.
 *
 * \param domain_name: The name of the domain without prefix.
 * \param module: Return parameter for the loaded module.
 * \return: Error codes. SPAWN_ERR_FIND_MODULE if module not found.
 */
errval_t module_manager_load (char* domain_name, struct module_info** module);

#endif
