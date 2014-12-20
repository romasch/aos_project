#include "init.h"

#include <barrelfish/dispatcher_arch.h>

#include <aos_support/module_manager.h>
#include <aos_support/server.h>

#define DDB_FIXED_LENGTH 32

static struct domain_info ddb[DDB_FIXED_LENGTH];


errval_t init_domain_manager (void)
{
    for (int i=0; i<DDB_FIXED_LENGTH; i++) {
        ddb[i].name[0] = '\0';
    }
    strncpy ((ddb[0].name), "init", MAX_PROCESS_NAME_LENGTH);
    return SYS_ERR_OK;
}

/**
 * Maximum possible domain identifier.
 */
uint32_t max_domain_id (void)
{
    return DDB_FIXED_LENGTH;
}

/**
 * Get the domain info struct identified by domain identifier 'id'.
 */
struct domain_info* get_domain_info (domainid_t id)
{
    return &(ddb[id]);
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
    for (int i = 0; (i < max_domain_id()) && (idx == -1); i++) {
        if (ddb[i].name[0] == '\0') {
            idx = i;
        }
    }

    if (idx != -1) {
        error = spawn_with_channel (name, idx, &ddb[idx].dispatcher_frame, &ddb[idx].channel);
        if (err_is_ok(error)) {
            strncpy(ddb[idx].name, name, MAX_PROCESS_NAME_LENGTH);
            ddb[idx].name[MAX_PROCESS_NAME_LENGTH] = '\0';
            if (ret_id) {
                *ret_id = idx;
            }
        }
    } else {
        // TODO: Find suitable error value.
        error = LIB_ERR_MALLOC_FAIL;
    }

    return error;

}