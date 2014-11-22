#ifndef AOS_DBG_H
    #define AOS_DBG_H

    #include <stdarg.h>

    #ifndef AOS_DEBUG
        #define AOS_DEBUG 0
    #endif
    
    #if AOS_DEBUG == 1 && !defined(VERBOSE)
        #define debug_printf_quiet debug_printf
    #else
        #define debug_printf_quiet(x, ...)
    #endif

    #if AOS_DEBUG == 1 && defined(DEBUG_PRINT_SHORT)
        static inline void debug_print_short (char* msg)    
        {
            char buf [5] = {0,0,0,0,0};
            for (int i=0; i<5 || msg[i] == '\0'; i++) {
                buf [i] = msg [i];
            }
            sys_print (buf, 5);
        }
    #else
        static inline void debug_print_short (char* msg) {}
    #endif

    #if AOS_DEBUG == 1 && defined(PRINT_STACK)
        #define PRINT_ENTRY debug_printf("%s...\n", __func__)
        #define PRINT_EXIT(error) debug_printf ("%s: %s\n", __func__, err_getstring (error))
        #define PRINT_EXIT_VOID debug_printf ("%s terminated\n", __func__)
    #else
        #define PRINT_ENTRY
        #define PRINT_EXIT(error)
        #define PRINT_EXIT_VOID
    #endif
#endif
