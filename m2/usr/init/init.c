/**
 * \file
 * \brief init process.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include "init.h"
#include <stdlib.h>
#include <string.h>
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>

#include <barrelfish/aos_rpc.h>

#include <barrelfish/cpu_arch.h>
#include <elf/elf.h>
#include <barrelfish_kpi/arm_core_data.h>

#include <mm/mm.h>

#include <aos_support/server.h>
#include <barrelfish/aos_dbg.h>
#include <aos_support/shared_buffer.h>

#include <aos_support/module_manager.h>

#define DDB_FIXED_LENGTH 32

// Entry of the process database maintained by init.
struct ddb_entry
{
    char name[MAX_PROCESS_NAME_LENGTH + 1];
    struct capref dispatcher_frame;
    bool exists; // TODO: Use this instead of the name.
    // Initial connection to domain.
    // NOTE: Malloced and owned by this struct, needs to be freed.
    struct lmp_chan* channel;
    // Possible observer for termination events.
    // NOTE: Not owned, do not free!
    struct lmp_chan* termination_observer;
};

static struct ddb_entry ddb[DDB_FIXED_LENGTH];

struct bootinfo *bi;
static coreid_t my_core_id;

//Keeps track of registered services.
struct lmp_chan* services [aos_service_guard];

// Keeps track of FIND_SERVICE requests.
// TODO: use a system that supports removal as well.
static struct lmp_chan* find_request [100];
static int find_request_index = 0;

/**
 * Initialize some data structures.
 */
static void init_data_structures (void) 
{
    for (int i=0; i<DDB_FIXED_LENGTH; i++) {
        ddb[i].name[0] = '\0';
    }
    ddb[0].name[0] = 'i'; // TODO: write init...
    ddb[0].name[1] = '\0';

    for (int i=0; i<aos_service_guard; i++) {
        services [i] = NULL;
    }
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
            my_core_id,
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

/*static bool str_to_args(const char* string, uint32_t* args, size_t args_length, int* indx, bool finished)
{
    finished = false;

    for (int i = 0; (i < args_length) && (finished == false); i++) {
        for (int j = 0; (j < 4) && (finished == false); j++, (*indx)++) {
            args[i] |= (uint32_t)(string[*indx]) << (8 * j);

            if ((string[*indx] == '\0') && (finished == false)) {
                finished = true;
            }
        }
    }

    return finished;
}*/

static const uintptr_t CHUNK_SIZE    = 64U                          ; // 2 cache lines
static const uintptr_t IKC_LAST_WORD = 64U / sizeof(uintptr_t) -  1U; // index of last word in ikc message

static void* shared_buffer = NULL;

static uintptr_t ikc_read_ptr  = 0;
static uintptr_t ikc_write_ptr = 0;

static bool free_prev_message = false;

static char* ikc_get_in_channel_base(void)
{
    char* channel_base;

    if (my_core_id == 0) {
        channel_base = (char*)(((uintptr_t)shared_buffer) + (BASE_PAGE_SIZE / 2));
    } else {
        channel_base = (char*)             shared_buffer                         ;
    }

    return channel_base;
}

static char* ikc_get_out_channel_base(void)
{
    char* channel_base;

    if (my_core_id == 0) {
        channel_base = (char*)             shared_buffer                         ;
    } else {
        channel_base = (char*)(((uintptr_t)shared_buffer) + (BASE_PAGE_SIZE / 2));
    }

    return channel_base;
}

static inline uintptr_t ikc_index_to_offset(const uintptr_t idx)
{
    return (idx % (BASE_PAGE_SIZE / (2 * CHUNK_SIZE))) * CHUNK_SIZE;
}

static void ikc_cleanup(char* channel_base)
{
    if (free_prev_message != false) {
        ((uintptr_t*)&channel_base[ikc_index_to_offset(ikc_read_ptr - 1)])[IKC_LAST_WORD] = 0;        

        free_prev_message = false;
    }
}

static void* peek_ikc_message(void)
{
    char* channel_base = ikc_get_in_channel_base();
    char* message_slot;

    ikc_cleanup(channel_base);

    message_slot = &channel_base[ikc_index_to_offset(ikc_read_ptr)];
    if(((uintptr_t*)message_slot)[IKC_LAST_WORD] == 0) {
        message_slot = NULL;
    } else {
        ikc_read_ptr++;
    }
    
    return message_slot;
}

static void* pop_ikc_message(void)
{
    debug_printf ("pop_ikc_message +\n");
    char* channel_base = ikc_get_in_channel_base();
    char* message_slot;

    ikc_cleanup(channel_base);

    message_slot = &channel_base[ikc_index_to_offset(ikc_read_ptr)];

    for (; ((volatile uintptr_t*)message_slot)[IKC_LAST_WORD] == 0 ;);
    ikc_read_ptr++;
    
    debug_printf ("pop_ikc_message -\n");
    return message_slot;
}

static void push_ikc_message(void* message, int size)
{
    debug_printf ("push_ikc_message +\n");
    char* channel_base = ikc_get_out_channel_base();
    char* message_slot;

    assert(size <= CHUNK_SIZE - sizeof(uintptr_t));

    message_slot = &channel_base[ikc_index_to_offset(ikc_write_ptr)];

    for (; ((volatile uintptr_t*)message_slot)[IKC_LAST_WORD] != 0 ;);
    memcpy(message_slot, message, size);
    ((uintptr_t*)message_slot)[IKC_LAST_WORD] = 1;
    ikc_write_ptr++;
    debug_printf ("push_ikc_message -\n");
}

static void* ikc_rpc_call(void* message, int size)
{
    push_ikc_message(message, size);

    return pop_ikc_message();
}

#define IKC_MSG_REMOTE_SPAWN 0x0FFFFFFFU

struct remote_spawn_message {
    uintptr_t message_id                              ;
    char      name      [64U - 2U * sizeof(uintptr_t)];
};

/**
 * The main receive handler for init.
 */
static void my_handler (struct lmp_chan* channel, struct lmp_recv_msg* message, struct capref cap, uint32_t type)
{
    // TODO: search-and-replace these uses.
    struct lmp_chan* lc = channel;
    struct lmp_recv_msg msg = *message;

    errval_t err = SYS_ERR_OK;

        // NOTE: In most cases we shouldn't use LMP_SEND_FLAGS_DEFAULT,
        // otherwise control will be transfered back to sender immediately...
    switch (type)
    {
        case AOS_RPC_GET_RAM_CAP:;
            size_t bits = msg.words [1];
            struct capref ram;
            errval_t error = ram_alloc (&ram, bits);

            lmp_chan_send2 (lc, 0, ram, error, bits);

            //TODO: do we need to destroy ram capability here?
//          error = cap_destroy (ram);

            debug_printf_quiet ("Handled AOS_RPC_GET_RAM_CAP: %s\n", err_getstring (error));
            break;
        case AOS_ROUTE_REGISTER_SERVICE:;
            debug_printf_quiet ("Got AOS_ROUTE_REGISTER_SERVICE 0x%x\n", msg.words [1]);
            assert (capref_is_null (cap));
            services [msg.words [1]] = lc;
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_GET_DEVICE_FRAME:;
            uint32_t device_addr = msg.words [1];
            uint8_t device_bits = msg.words [2];
            struct capref device_frame;
            error = allocate_device_frame (device_addr, device_bits, &device_frame);

            lmp_chan_send1 (lc, 0, device_frame, error);

            //TODO: do we need to destroy frame capability here?
//          error = cap_destroy (device_frame);

            debug_printf_quiet ("Handled AOS_RPC_GET_DEVICE_FRAME: %s\n", err_getstring (error));
            break;
        case AOS_ROUTE_FIND_SERVICE:;
            debug_printf_quiet ("Got AOS_ROUTE_FIND_SERVICE 0x%x\n", msg.words [1]);
            // generate new ID
            uint32_t id = find_request_index;
            find_request_index++;
            // store current channel at ID
            find_request [id] = lc;
            // find correct server channel
            uint32_t requested_service = msg.words [1];
            struct lmp_chan* serv = services [requested_service];
            // generate AOS_ROUTE_REQUEST_EP request with ID.
            if (serv != NULL) {
                lmp_chan_send2 (serv, 0, NULL_CAP, AOS_ROUTE_REQUEST_EP, id);
            } else {
                // Service is unknown. Send error back.
                lmp_chan_send1 (lc, 0, NULL_CAP, -1); // TODO: proper error value
            }
            break;
        case AOS_ROUTE_DELIVER_EP:;
            debug_printf_quiet ("Got AOS_ROUTE_DELIVER_EP\n");
            // get error value and ID from message
            errval_t error_ret = msg.words [1];
            id = msg.words [2];
            // lookup receiver channel at ID
            struct lmp_chan* recv = find_request [id]; // TODO: delete id
            // generate response with cap and error value
            lmp_chan_send1 (recv, 0, cap, error_ret);
            // Delete cap and reuse slot
            err = cap_delete (cap);
            lmp_chan_set_recv_slot (lc, cap);
            lmp_chan_alloc_recv_slot (lc);
            break;
        case AOS_RPC_SPAWN_PROCESS:;
            int idx = -1;
            char* name = (char*) &(msg.words [1]);

            //DBG: debug_printf ("Got AOS_RPC_SPAWN_PROCESS <- %s\n", name);

            // Lookup of free process slot in process database
            for (int i = 0; (i < DDB_FIXED_LENGTH) && (idx == -1); i++) {
                if (ddb[i].name[0] == '\0') {
                    idx = i;
                }
            }

            if (idx != -1) {
                err = spawn_with_channel (name, idx, &ddb[idx].dispatcher_frame, &ddb[idx].channel);
                if (err_is_ok(err)) {
                    strcpy(ddb[idx].name, name);
                }
            }

            // Send reply back to the client
            lmp_chan_send2(lc, 0, NULL_CAP, err, idx);
            break;
        case AOS_RPC_SPAWN_PROCESS_REMOTELY:;
            struct remote_spawn_message rsm = { .message_id = IKC_MSG_REMOTE_SPAWN };

            name = (char*) &(msg.words [1]);
            
            strcpy(rsm.name, name);
            
            void* reply = ikc_rpc_call(&rsm, sizeof(rsm));

            debug_printf ("AOS_RPC_SPAWN_PROCESS_REMOTELY\n");

            lmp_chan_send1(lc, 0, NULL_CAP, *(errval_t*)reply);
            break;
        case AOS_RPC_GET_PROCESS_NAME:;
            domainid_t pid    = msg.words [1];
            debug_printf_quiet ("Got AOS_RPC_GET_PROCESS_NAME  for process %u\n", pid);

            if ((pid < DDB_FIXED_LENGTH) && (ddb[pid].name[0] != '\0')) {
                uint32_t args[8];

                strcpy((char*)args, ddb[pid].name);

                lmp_chan_send9 (lc, 0, NULL_CAP, SYS_ERR_OK, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
            } else {
                lmp_chan_send1 (lc, 0, NULL_CAP, -1);
            }
            break;
        case AOS_RPC_GET_PROCESS_LIST:;
            debug_printf_quiet ("Got AOS_RPC_GET_PROCESS_LIST\n");
            uint32_t args[7] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
            int args_index = 0;

            idx = 0xffffffff;

            for (int i = msg.words [1]; (i < DDB_FIXED_LENGTH) && (args_index < 7); i++) {
                if (ddb[i].name[0] != '\0') {
                    args[args_index] = i;
                    args_index++;
                    idx  = i + 1;
                }
            }
            lmp_chan_send9 (lc, 0, NULL_CAP, SYS_ERR_OK, idx, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
            break;
        case AOS_RPC_SET_LED:;
            bool state = msg.words [1];
            led_set_state (state);
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            break;
        case AOS_RPC_KILL:;
            //DBG: Uncomment if you really need it ==> debug_printf ("Got AOS_RPC_KILL\n");
            // TODO: error handling;
            uint32_t pid_to_kill = msg.words [1];
            // TODO: Check if this is a self-kill. If yes sending a message is unnecessary.
            lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            ddb [pid_to_kill].name[0] = '\0';
            error = cap_revoke (ddb [pid_to_kill].dispatcher_frame);
            error = cap_destroy (ddb [pid_to_kill].dispatcher_frame);

            // Notify the observer about termination.
            if (ddb [pid_to_kill].termination_observer) {
                lmp_chan_send1 (ddb [pid_to_kill].termination_observer, 0, NULL_CAP, SYS_ERR_OK);
                ddb [pid_to_kill].termination_observer = NULL;
            }

            // TODO: properly clean up processor, i.e. its full cspace.
            break;

        case AOS_RPC_WAIT_FOR_TERMINATION:;
            uint32_t pid_to_wait = msg.words [1];
            if (ddb [pid_to_wait].name[0] == '\0') {
                // Domain is already dead!
                lmp_chan_send1 (lc, 0, NULL_CAP, SYS_ERR_OK);
            } else {
                ddb [pid_to_wait].termination_observer = lc;
            }
            break;
        default:
            handle_unknown_message (channel, cap);
            debug_printf ("ERROR: Got unknown message\n");
    }

}

/**
 * Set up the user-level data structures for the
 * kernel-created endpoint for init.
 */
__attribute__((unused))
static errval_t initep_setup (void)
{
    errval_t err = SYS_ERR_OK;

    // Allocate an LMP channel and do basic initializaton.
    // TODO; maybe make the allocated channel structure available somewhere.
    struct lmp_chan* channel = malloc (sizeof (struct lmp_chan));

    if (channel != NULL) {
        lmp_chan_init (channel);

        // make local endpoint available -- this was minted in the kernel in a way
        // such that the buffer is directly after the dispatcher struct and the
        // buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel
        // sentinel word).

        // NOTE: lmp_endpoint_setup automatically adds the dispatcher offset.
        // Thus the offset of the first endpoint structure is zero.

        struct lmp_endpoint* endpoint; // Structure to be filled in.
        err = lmp_endpoint_setup (0, DEFAULT_LMP_BUF_WORDS, &endpoint);

        if (err_is_fail(err)) {
            debug_printf ("ERROR: On endpoint setup.\n");

        } else {
            // Update the channel with the newly created endpoint.
            channel->endpoint  = endpoint;
            // The channel needs to know about the (kernel-created) capability to receive objects.
            channel->local_cap = cap_initep;

            // Allocate a slot for incoming capabilities.
            err = lmp_chan_alloc_recv_slot(channel);
            if (err_is_fail(err)) {
                debug_printf ("ERROR: On allocation of recv slot.\n");

            } else {
                // Register a receive handler.
                err = lmp_chan_register_recv(channel, get_default_waitset(), MKCLOSURE(get_default_handler(), channel));
                if (err_is_fail(err))
                {
                    debug_printf ("ERROR: On channel registration.\n");
                }
            }
        }
    } else {
        // malloc returned NULL...
        err = LIB_ERR_MALLOC_FAIL;
    }
    return err;
} //*/

static errval_t dummy_elfload_allocate (void *state, genvaddr_t base, size_t size, uint32_t flags, void **retbase)
{
    *retbase = state + base;
    return SYS_ERR_OK;
}

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

__attribute__((unused))
static errval_t spawn_core(coreid_t cid)
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


__attribute__((unused))
static struct thread* ikcsrv;

__attribute__((unused))
static int ikc_server(void* data)
{
    while (true) {
        errval_t  err    ;
        void    * message;

        message = peek_ikc_message();
        if (message == NULL) {
            message = pop_ikc_message();
        }

        switch(*((uintptr_t*)message))
        {
        case IKC_MSG_REMOTE_SPAWN:;
            err = SYS_ERR_OK;

            int   idx  = -1;
            char* name = ((char*)message) + sizeof(uintptr_t);


            for (int i = 0; (i < DDB_FIXED_LENGTH) && (idx == -1); i++) {
                if (ddb[i].name[0] == '\0') {
                    idx = i;
                }
            }

            // Fix the name by removing whitespaces and newlines.
            for (int i = 0; i < DDB_FIXED_LENGTH; i++) {
                char c = name [i];
                if (c == ' ' || c=='\n' || c == '\r') {
                    name [i] = '\0';
                }
            }


            for (volatile int wait=0; wait<1000000;wait++);
            debug_printf (name);
            for (volatile int wait=0; wait<1000000;wait++);


            if (idx != -1) {
                err = spawn_with_channel (name, idx, &ddb[idx].dispatcher_frame, &ddb[idx].channel);
                if (err_is_ok(err)) {
                    strcpy(ddb[idx].name, name);
                }
            }
            push_ikc_message(&err, sizeof(err));
            break;
        default:;
            uintptr_t reply = -1;

            push_ikc_message(&reply, sizeof(reply));
            break;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    set_external_handler (my_handler);

    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);

    debug_printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

    debug_printf("FIRSTEP_OFFSET should be %zu + %zu\n",
            get_dispatcher_size(), offsetof(struct lmp_endpoint, k));

    // First argument contains the bootinfo location
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);

    // setup memory serving
    err = initialize_ram_alloc();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init local ram allocator");
        abort();
    }
    debug_printf("initialized local ram alloc\n");

    // setup memory serving
    err = initialize_mem_serv();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init memory server module");
        abort();
    }

    // Set up some data structures for RPC services.
    init_data_structures ();

    // Create our endpoint to self
    err = cap_retype(cap_selfep, cap_dispatcher, ObjType_EndPoint, 0);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to create our endpoint to self");
        // bail out, there isn't much we can do without a self endpoint.
        abort();
    }

    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    err = initialize_device_frame_server (cap_io);

    // Initialize the LED driver.
    led_init ();
    led_set_state (true);

    // Initialize the module manager.
    err = module_manager_init (bi);
    assert (err_is_ok (err));


    // Get the capability to the shared frame.
    struct capref shared_frame = cap_initep;
    shared_frame.slot = TASKCN_SLOT_MON_URPC;

    // Map it.
    err = paging_map_frame_attr (
            get_current_paging_state(),
            &shared_buffer,
            BASE_PAGE_SIZE,
            shared_frame,
            VREGION_FLAGS_READ_WRITE_NOCACHE,
            NULL,
            NULL);

    assert (err_is_ok (err));


    // TODO: need special initialization for second init.
    if (my_core_id == 0) {
    // Initialize the serial driver.
        struct lmp_chan* serial_chan;
        if (err_is_ok (err)) {
    //         init_uart_driver ();
            strcpy(ddb[2].name, "serial_driver");
            err = spawn_with_channel ("serial_driver", 2, &(ddb[2].dispatcher_frame), &serial_chan);
            debug_printf ("Spawning serial driver: %s\n", err_getstring (err));
            while (services [aos_service_serial] == NULL) {
                event_dispatch (get_default_waitset()); 
            }
        }

        struct lmp_chan* filesystem_chan;
        if (err_is_ok (err)) {
            strcpy(ddb[3].name, "mmchs");
            err = spawn_with_channel ("mmchs", 3, &(ddb[3].dispatcher_frame), &filesystem_chan);
            debug_printf ("Spawning filesystem driver: %s\n", err_getstring (err));
            while (services [aos_service_filesystem] == NULL) {
                event_dispatch (get_default_waitset());
            }
        }



        if (err_is_fail (err)) {
            debug_printf ("Failed to initialize: %s\n", err_getstring (err));
        }
        debug_printf_quiet ("initialized core services\n");



        if (my_core_id == 0) {
            // Zero out the shared memory.
            memset (shared_buffer, 0, BASE_PAGE_SIZE);

            volatile uint32_t* as_int_array = shared_buffer;
            assert (as_int_array [0] == 0);

            err = spawn_core(1);
            debug_printf ("spawn_core: %s\n", err_getstring (err));

            /* struct remote_spawn_message rsm = { .message_id = IKC_MSG_REMOTE_SPAWN };
            
            strcpy(rsm.name, "hello_world");
            ikc_rpc_call(&rsm, sizeof(rsm));
            
            debug_printf ("Got a test message from init.\n");*/
        }

        // Spawn the shell.
        struct lmp_chan* memeater_chan;
        strcpy(ddb[1].name, "memeater");
        spawn_with_channel ("memeater",  1, &(ddb[1].dispatcher_frame), &memeater_chan);
    } else {
        ikcsrv = thread_create(ikc_server, NULL);
    }

    // Go into messaging main loop.
    while (true) {
        err = event_dispatch (get_default_waitset());
        if (err_is_fail (err)) {
            debug_printf ("Handling LMP message: %s\n", err_getstring (err));
        }
    }

    debug_printf ("init returned.\n");
    return EXIT_SUCCESS;
}
