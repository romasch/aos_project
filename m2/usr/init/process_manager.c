#include "init.h"

#include <barrelfish/dispatcher_arch.h>
#include <spawndomain/spawndomain.h>

#include <aos_support/module_manager.h>
#include <aos_support/server.h>

#define INITIAL_TABLE_LENGTH 4

static struct domain_info* domain_table;
static uint32_t domain_table_count;
static uint32_t domain_table_capacity;


errval_t init_domain_manager (void)
{
    domain_table = calloc (INITIAL_TABLE_LENGTH, sizeof (struct domain_info));
    domain_table_count = 1;
    domain_table_capacity = INITIAL_TABLE_LENGTH;

    struct domain_info* init = get_domain_info (0);
    strcpy (init -> name, "init");
    init -> state = domain_info_state_running;

    return SYS_ERR_OK;
}

/**
 * Maximum possible domain identifier.
 */
uint32_t max_domain_id (void)
{
    return domain_table_count;
}

static errval_t allocate_entry (uint32_t* result_index)
{
    assert (result_index);
    errval_t error = SYS_ERR_OK;
    bool found = false;

    // First try to find an unused spot.
    for (int i=0; i < max_domain_id() && !found; i++) {
        if (get_domain_info(i) -> state == domain_info_state_free) {
            found = true;
            *result_index = i;
        }
    }

    // Reallocate if necessary.
    if (!found && domain_table_count == domain_table_capacity) {
        void* new_buffer = realloc (domain_table, 2 * domain_table_capacity * sizeof (struct domain_info));
        if (new_buffer) {
            domain_table = new_buffer;
            domain_table_capacity *= 2;
        } else {
            error = LIB_ERR_MALLOC_FAIL;
        }
    }

    // Get an entry from the end.
    if (!found && err_is_ok (error)) {
        *result_index = domain_table_count;
        domain_table_count++;
    }
    return error;
}

/**
 * Get the domain info struct identified by domain identifier 'id'.
 */
struct domain_info* get_domain_info (domainid_t id)
{
    return (domain_table + id);
}


/// Allocate 'destination' and copy 'source' into it.
static errval_t copy_capability (struct capref* destination, struct capref source)
{
    errval_t error = slot_alloc (destination);

    if (err_is_ok (error)) {

        error = cap_copy (*destination, source);

        // Need to clean up if copy failed.
        if (err_is_fail (error)) {
            error = err_push (error, slot_free (*destination));
        }
    }
    return error;
}

/**
 * Spawn a new domain with an initial channel to init.
 *
 * \arg domain_name: The name of the new process. NOTE: Name should come
 * without path prefix, i.e. just pass "memeater" instead of "armv7/sbin/memeater".
 *
 * \arg domain_id: ID of the new domain.
 */
static errval_t spawn_with_channel (char* domain_name, uint32_t domain_id)

{
    errval_t error = SYS_ERR_OK;

    // Find the module.
    struct module_info* info;
    error = module_manager_load (domain_name, &info);

    // Early exit if module not found.
    if (err_is_fail (error)) {
        return error;
    }

    assert (info != NULL);

    // Spawninfo struct filled by spawn_arch_load().
    struct spawninfo new_domain;
    memset (&new_domain, 0, sizeof (struct spawninfo));
    new_domain.domain_id = domain_id;


    if (err_is_ok (error)) {

        // Set up default arguments.
        // TODO: Support user-provided arguments.
        char* argv [] = {domain_name, NULL};
        char* envp [] = {NULL};

        // Load the image and set up addess space, cspace etc.
        error = spawn_load_image (
            &new_domain,
            info->virtual_address,
            info->size,
            CPU_ARM,
            domain_name,
            get_core_id(),
            argv,
            envp,
            NULL_CAP,
            NULL_CAP
        );
    }

    // Set domain info data structure.
    struct domain_info* domain_data = get_domain_info (domain_id);
    if (err_is_ok (error)) {
        get_dispatcher_generic (new_domain.handle)->domain_id = domain_id;
        error = copy_capability ( &(domain_data->dispatcher_capability), new_domain.dcb);
    }
    if (err_is_ok (error)) {
        error = copy_capability ( &(domain_data->root_cnode_capability), new_domain.rootcn_cap);
    }

    // Allocate an LMP channel between init and the new domain.
    struct lmp_chan* initial_channel;
    if (err_is_ok (error)) {
        error = create_channel (&initial_channel);
    }

    // Copy the endpoint of the new channel into the new domain.
    if (err_is_ok (error)) {
        struct capref init_remote_cap;
        init_remote_cap.cnode = new_domain.taskcn;
        init_remote_cap.slot  = TASKCN_SLOT_INITEP;

        error = cap_copy(init_remote_cap, initial_channel->local_cap);

        domain_data -> channel = initial_channel;
    }

    // Make the domain runnable
    if (err_is_ok (error)) {
        error = spawn_run(&new_domain);
    }

    // Clean up structures in current domain.
    error = spawn_free(&new_domain);

    return error;
}

/**
 * Spawn a new domain.
 *
 * \param name: The name of the binary.
 * \param ret_id: Return parameter for the ID.
 */
errval_t spawn (char* name, domainid_t* ret_id)
{
    errval_t error = SYS_ERR_OK;
    uint32_t idx = -1;

    // Lookup of free process slot in process database
    error = allocate_entry (&idx);

    if (err_is_ok (error)) {
        struct domain_info* domain = get_domain_info (idx);

        error = spawn_with_channel (name, idx);

        if (err_is_ok(error)) {
            strncpy(domain -> name, name, MAX_PROCESS_NAME_LENGTH);
            domain -> name[MAX_PROCESS_NAME_LENGTH] = '\0';
            domain -> state = domain_info_state_running;
            if (ret_id) {
                *ret_id = idx;
            }
        }
    }

    return error;
}