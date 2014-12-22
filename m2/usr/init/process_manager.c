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
        if (get_domain_info(i) -> name[0] == '\0') {
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


/**
 * Spawn a new domain with an initial channel to init.
 *
 * \arg domain_name: The name of the new process. NOTE: Name should come
 * without path prefix, i.e. just pass "memeater" instead of "armv7/sbin/memeater".
 *
 * \arg ret_channel: Return parameter for allocated channel. May be NULL.
 */
static errval_t spawn_with_channel (char* domain_name, uint32_t domain_id, struct capref* ret_dispatcher, struct lmp_chan** ret_channel)
{
    //DBG: Uncomment if you really need it ==> debug_printf("Spawning new domain: %s...\n", domain_name);
    errval_t error = SYS_ERR_OK;

    // Struct to keep track of new domains cspace, vspace, etc...
    struct spawninfo new_domain;
    memset (&new_domain, 0, sizeof (struct spawninfo));
    new_domain.domain_id = domain_id;

    // Find the module.
    struct module_info* info;
    error = module_manager_load (domain_name, &info);

    if (err_is_ok (error)) {

        assert (info != NULL);

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

    // Set data structures.
    if (err_is_ok(error) && ret_dispatcher) {
        // TODO; error handling. Also, it may be possible to not delete new_domain.dcb
        get_dispatcher_generic (new_domain.handle)->domain_id = domain_id;
        slot_alloc (ret_dispatcher);
        cap_copy (*ret_dispatcher, new_domain.dcb);
    } else {
        return error;
    }

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

        if (ret_channel) {
            *ret_channel = initial_channel;
        }

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

        error = spawn_with_channel (name, idx,
                    &(domain -> dispatcher_frame),
                    &(domain -> channel));
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