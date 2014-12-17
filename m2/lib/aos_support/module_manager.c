#include <aos_support/module_manager.h>

#include <spawndomain/spawndomain.h>

// Initial length of module cache
#define MODULE_CACHE_CAPACITY 32

static struct bootinfo* bootinfo;
static struct module_info** module_cache;
static uint32_t module_cache_count;
static uint32_t module_cache_capacity;


/// Resize the module cache if necessary.
static errval_t module_cache_resize (void)
{
    errval_t error = SYS_ERR_OK;

    if (module_cache_count == module_cache_capacity) {

        struct module_info** cache_new = realloc (module_cache,
                        sizeof (2 * module_cache_capacity * sizeof (struct module_info*)));
        if (cache_new) {
            module_cache = cache_new;
            module_cache_capacity = 2 * module_cache_capacity;
        } else {
            error = LIB_ERR_MALLOC_FAIL;
        }
    }
    return error;
}

errval_t module_manager_init (struct bootinfo* bi)
{
    assert (bi != NULL);
    errval_t error = SYS_ERR_OK;

    bootinfo = bi;

    module_cache = malloc (sizeof (MODULE_CACHE_CAPACITY * sizeof (struct module_info*)));
    if (module_cache) {
        module_cache_count = 0;
        module_cache_capacity = MODULE_CACHE_CAPACITY;
    } else {
        error = LIB_ERR_MALLOC_FAIL;
    }

    return error;
}


errval_t module_manager_load (char* domain_name, struct module_info** ret_module)

{
    errval_t error = SYS_ERR_OK;
    bool found = false;

    // Try to find the module in the cache.
    for (int i=0; i<module_cache_count && !found; i++) {
        if (strcmp (module_cache [i] -> name, domain_name) == 0) {
            found = true;
            *ret_module = module_cache [i];
        }
    }

    // We may need to load the module from the boot info struct.
    if (!found) {
        // TODO: A more graceful recovery from buffer overflow attacks...
        assert (strlen (domain_name) < 128);

        // Concatenate the prefix with the domain name.
        char prefixed_name [256];
        strcpy (prefixed_name, BINARY_PREFIX);
        strcat (prefixed_name, domain_name);

        // Try to find the module in the multiboot image.
        struct mem_region* region = NULL;
        region = multiboot_find_module(bootinfo, prefixed_name);

        if (region) {

            // Allocate a new module info struct.
            error = module_cache_resize ();
            struct module_info* info = malloc (sizeof (struct module_info));
            char* copied_name = malloc (strlen (domain_name) + 1);

            if (err_is_ok (error) && info && copied_name) {

                strcpy (copied_name, domain_name);
                info -> name = copied_name;
                info -> module = region;

                // Map the module into our address space.
                error = spawn_map_module (
                        region,
                        &(info->size),
                        &(info->virtual_address),
                        &(info->physical_address));

                if (err_is_ok (error)) {

                    module_cache [module_cache_count] = info;
                    module_cache_count++;
                    *ret_module = info;
                } else {
                    error = err_push(error, SPAWN_ERR_ELF_MAP);
                }
            } else {
                if (info) {
                    free (info);
                }
                if (copied_name) {
                    free (copied_name);
                }
                error = LIB_ERR_MALLOC_FAIL;
            }
        } else {
            debug_printf("could not find module [%s] in multiboot image\n", prefixed_name);
            error = SPAWN_ERR_FIND_MODULE;
        }
    }
    return error;
}

