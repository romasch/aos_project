#include "init.h"

static const uintptr_t CHUNK_SIZE    = 64U                          ; // 2 cache lines
static const uintptr_t IKC_LAST_WORD = 64U / sizeof(uintptr_t) -  1U; // index of last word in ikc message

static uintptr_t ikc_read_ptr  = 0;
static uintptr_t ikc_write_ptr = 0;

static bool free_prev_message = false;

static char* ikc_get_in_channel_base(void)
{
    char* channel_base;

    if (get_core_id () == 0) {
        channel_base = (char*)(((uintptr_t)get_cross_core_buffer()) + (BASE_PAGE_SIZE / 2));
    } else {
        channel_base = (char*)             get_cross_core_buffer()                         ;
    }

    return channel_base;
}

static char* ikc_get_out_channel_base(void)
{
    char* channel_base;

    if (get_core_id () == 0) {
        channel_base = (char*)             get_cross_core_buffer()                         ;
    } else {
        channel_base = (char*)(((uintptr_t)get_cross_core_buffer()) + (BASE_PAGE_SIZE / 2));
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

void* ikc_rpc_call(void* message, int size)
{
    push_ikc_message(message, size);
    return pop_ikc_message();
}

int ikc_server(void* data)
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

            char* name = ((char*)message) + sizeof(uintptr_t);




            // Fix the name by removing whitespaces and newlines.
            for (int i = 0; i < MAX_PROCESS_NAME_LENGTH; i++) {
                char c = name [i];
                if (c == ' ' || c=='\n' || c == '\r') {
                    name [i] = '\0';
                }
            }

            for (volatile int wait=0; wait<1000000;wait++);
            debug_printf (name);
            for (volatile int wait=0; wait<1000000;wait++);

            err = spawn (name, NULL);

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
