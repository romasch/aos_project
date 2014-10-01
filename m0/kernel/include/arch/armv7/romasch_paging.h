
#ifndef __ROMASCH_PAGING_H
#define __ROMASCH_PAGING_H

#include <kernel.h>

#define OUT(var) printf("Value of "#var": %X\n", var) 
#define TEST(expr) printf("Test mapping of "#expr": %X -> %X\n", (lvaddr_t) expr, test_translate ((lvaddr_t) expr))

/*
 * Paging init.
 */
extern void romasch_paging_init (void);

/*
 * Test translate.
 */
extern lpaddr_t test_translate (lvaddr_t address);

extern lvaddr_t uart_base (void);

#endif