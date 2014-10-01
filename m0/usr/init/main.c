/**
 * \file
 * \brief faux-init process to help bring up userspace.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <syscalls.h>
#include <stdio.h>

int main(void)
{
    sys_nop();
	sys_print ("abcd", 5);
	sys_print ("Hello World\n", 12);
	printf ("Test some value: 0x%X\nprintf() works!\n", 1024*1024);
	
	
	printf ("Testing sys_print with illegal args: %u\n", sys_print("asdf", -1));
	printf ("Testing sys_print with illegal args: %u\n", sys_print (NULL, 1));
	printf ("Testing sys_undefined: %u\n", sys_undefined ());
	printf ("Testing sys_led: %u\n", sys_undefined ());
	printf ("%u\n", sys_led ());
    return 0;
}
