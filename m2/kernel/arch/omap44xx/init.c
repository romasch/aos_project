/*
 * Copyright (c) 2009-2013, ETH Zurich. All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
 * Attn: Systems Group.
 */

/**
 * \file
 * \brief CPU driver init code for the OMAP44xx series SoCs. 
 * interface found in /kernel/include/serial.h
 */

#include <kernel.h>
#include <string.h>
#include <init.h>
#include <exceptions.h>
#include <exec.h>
#include <offsets.h>
#include <paging_kernel_arch.h>
#include <phys_mmap.h>
#include <serial.h>
#include <stdio.h>
#include <arm_hal.h>
#include <getopt/getopt.h>
#include <cp15.h>
#include <elf/elf.h>
#include <arm_core_data.h>
#include <startup_arch.h>
#include <kernel_multiboot.h>
#include <global.h>

#include <omap44xx_map.h>
#include <dev/omap/omap44xx_id_dev.h>
#include <dev/omap/omap44xx_emif_dev.h>
#include <dev/omap/omap44xx_gpio_dev.h>
#include <arch/armv7/start_aps.h>


/// Round up n to the next multiple of size
#define ROUND_UP(n, size)           ((((n) + (size) - 1)) & (~((size) - 1)))

/**
 * Used to store the address of global struct passed during boot across kernel
 * relocations.
 */
//static uint32_t addr_global;

/**
 * \brief Kernel stack.
 *
 * This is the one and only kernel stack for a kernel instance.
 */
uintptr_t kernel_stack[KERNEL_STACK_SIZE/sizeof(uintptr_t)]
__attribute__ ((aligned(8)));

/**
 * \brief application core stack
 *
 * This stack will be used by the application core
 */
uintptr_t app_core_stack[KERNEL_STACK_SIZE/sizeof(uintptr_t)]
__attribute__ ((aligned(8)));


/**
 * Boot-up L1 page table for addresses up to 2GB (translated by TTBR0)
 */
//XXX: We reserve double the space needed to be able to align the pagetable
//	   to 16K after relocation
static union arm_l1_entry boot_l1_low[2*ARM_L1_MAX_ENTRIES]
__attribute__ ((aligned(ARM_L1_ALIGN)));
static union arm_l1_entry * aligned_boot_l1_low;
/**
 * Boot-up L1 page table for addresses >=2GB (translated by TTBR1)
 */
//XXX: We reserve double the space needed to be able to align the pagetable
//	   to 16K after relocation
static union arm_l1_entry boot_l1_high[2*ARM_L1_MAX_ENTRIES]
__attribute__ ((aligned(ARM_L1_ALIGN)));
static union arm_l1_entry * aligned_boot_l1_high;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define CONSTRAIN(x, a, b) MIN(MAX(x, a), b)

lvaddr_t aux_core_boot_section = 0;
volatile uint32_t *shared_var = 0;

//
// Kernel command line variables and binding options
//

static int timeslice  = 5; //interval in ms in which the scheduler gets called

static struct cmdarg cmdargs[] = {
    { "consolePort",    ArgType_UInt, { .uinteger = &serial_console_port}},
    { "debugPort",      ArgType_UInt, { .uinteger = &serial_debug_port}},
    { "loglevel",       ArgType_Int, { .integer = &kernel_loglevel }},
    { "logmask",        ArgType_Int, { .integer = &kernel_log_subsystem_mask }},
    { "timeslice",      ArgType_Int, { .integer = &timeslice }},
    {NULL, 0, {NULL}}
};

static inline void __attribute__ ((always_inline))
relocate_stack(lvaddr_t offset)
{
    __asm volatile (
		    "add	sp, sp, %[offset]\n\t" ::[offset] "r" (offset)
		    );
}

static inline void __attribute__ ((always_inline))
relocate_got_base(lvaddr_t offset)
{
    __asm volatile (
		    "add	r10, r10, %[offset]\n\t" ::[offset] "r" (offset)
		    );
}

#ifndef __GEM5__
static void enable_cycle_counter_user_access(void)
{
    /* enable user-mode access to the performance counter*/
    __asm volatile ("mcr p15, 0, %0, C9, C14, 0\n\t" :: "r"(1));

    /* disable counter overflow interrupts (just in case)*/
    __asm volatile ("mcr p15, 0, %0, C9, C14, 2\n\t" :: "r"(0x8000000f));
}
#endif


void led_flash(int counter);
void led_set_state(bool new_state);
static omap44xx_gpio_t g1;
#define GPIO_SIZE 0x1000    // 4k


static void led_remap_register(void)
{
    lvaddr_t base = paging_map_device(OMAP44XX_MAP_L4_WKUP_GPIO1, GPIO_SIZE);
    // paging_map_device returns an address pointing to the beginning of
    // a section, need to add the offset for within the section again
    uint32_t offset = (OMAP44XX_MAP_L4_WKUP_GPIO1 & ARM_L1_SECTION_MASK);
    printk(LOG_NOTE, "led_map_register: base = 0x%"PRIxLVADDR"\n",
            base);
    printk(LOG_NOTE, "led_map_register: offset = 0x%"PRIxLVADDR"\n", offset);

    lvaddr_t  dev_addr = base + offset;
    omap44xx_gpio_initialize(&g1, (mackerel_addr_t)dev_addr);
    return;
}

/*
 * Doesn't work yet on the second LED for some reason...
 */
static void set_leds(void)
{
    omap44xx_gpio_t g2;
    //char buf[8001];
    uint32_t r, nr;

    omap44xx_gpio_initialize(&g1, (mackerel_addr_t)OMAP44XX_MAP_L4_WKUP_GPIO1);
    // Output enable
    r = omap44xx_gpio_oe_rd(&g1) & (~(1<<8));
    omap44xx_gpio_oe_wr(&g1, r);
    // Write data out
    r = omap44xx_gpio_dataout_rd(&g1);
    nr = r  |(1<<8);
    for(int i = 0; i < 5; i++) {
        omap44xx_gpio_dataout_wr(&g1,r);
        for(int j = 0; j < 20000; j++);
        omap44xx_gpio_dataout_wr(&g1,nr);
        for(int j = 0; j < 20000; j++);
    }
    return;

    omap44xx_gpio_initialize(&g2, (mackerel_addr_t)OMAP44XX_MAP_L4_PER_GPIO4);

    // Output enable
    r = omap44xx_gpio_oe_rd(&g2) & (~(1<<14));
    omap44xx_gpio_oe_wr(&g2,r);
    // Write data out
    r = omap44xx_gpio_dataout_rd(&g2);
    nr = r  |(1<<14);
    for(int i = 0; i < 100; i++) {
        omap44xx_gpio_dataout_wr(&g2,r);
        for(int j = 0; j < 2000; j++) {
            printk(LOG_NOTE, ".");
        }
        omap44xx_gpio_dataout_wr(&g2,nr);
        for(int j = 0; j < 2000; j++) {
            printk(LOG_NOTE, ".");
        }
    }
}



void led_set_state(bool new_state)
{
    uint32_t r;
    r = omap44xx_gpio_dataout_rd(&g1);

    if (new_state) {
        r = r | (1 << 8);
    } else {
        r = r & (~(1<<8));
    }
    omap44xx_gpio_dataout_wr(&g1,r);
}

/*
 * Doesn't work yet on the second LED for some reason...
 */
void led_flash(int counter)
{
    bool state = true;
    for (int i = 0; i < counter; i++) {
        led_set_state(state);
	for(volatile int j = 0; j < 1000000; j++);
        state = !state;
    }
}



extern void paging_map_device_section(uintptr_t ttbase, lvaddr_t va, lpaddr_t pa);

static void paging_init(void)
{
    // configure system to use TTBR1 for VAs >= 2GB
    uint32_t ttbcr;
    ttbcr = cp15_read_ttbcr();
    ttbcr |= 1;
    cp15_write_ttbcr(ttbcr);

    // make sure pagetables are aligned to 16K
    aligned_boot_l1_low = 
	(union arm_l1_entry *)ROUND_UP((uintptr_t)boot_l1_low, ARM_L1_ALIGN);
    aligned_boot_l1_high = 
	(union arm_l1_entry *)ROUND_UP((uintptr_t)boot_l1_high, ARM_L1_ALIGN);

    lvaddr_t vbase = MEMORY_OFFSET, base =  0;

    for(size_t i=0; i < ARM_L1_MAX_ENTRIES/2; i++,
	    base += ARM_L1_SECTION_BYTES, vbase += ARM_L1_SECTION_BYTES) {
	// create 1:1 mapping
	//		paging_map_kernel_section((uintptr_t)aligned_boot_l1_low, base, base);
	paging_map_device_section((uintptr_t)aligned_boot_l1_low, base, base);

	// Alias the same region at MEMORY_OFFSET (gem5 code)
	// create 1:1 mapping for pandaboard
	//		paging_map_device_section((uintptr_t)boot_l1_high, vbase, vbase);
	/* if(vbase < 0xc0000000) */
	paging_map_device_section((uintptr_t)aligned_boot_l1_high, vbase, vbase);
    }

    // Activate new page tables
    cp15_write_ttbr1((lpaddr_t)aligned_boot_l1_high);
    cp15_write_ttbr0((lpaddr_t)aligned_boot_l1_low);
}

void kernel_startup_early(void)
{
    const char *cmdline;
    assert(glbl_core_data != NULL);
    cmdline = MBADDR_ASSTRING(glbl_core_data->cmdline);
    parse_commandline(cmdline, cmdargs);
    timeslice = CONSTRAIN(timeslice, 1, 20);
}

/**
 * \brief Continue kernel initialization in kernel address space.
 *
 * This function resets paging to map out low memory and map in physical
 * address space, relocating all remaining data structures. It sets up exception handling,
 * initializes devices and enables interrupts. After that it
 * calls arm_kernel_startup(), which should not return (if it does, this function
 * halts the kernel).
 */
static void  __attribute__ ((noinline,noreturn)) text_init(void)
{
    char          buf[200];
    errval_t      errval  ;
    omap44xx_id_t id      ;

    if(glbl_core_data->multiboot_flags & MULTIBOOT_INFO_FLAG_HAS_MMAP) {
        struct arm_coredata_mmap* mmap = (struct arm_coredata_mmap*)local_phys_to_mem(glbl_core_data->mmap_addr);

        paging_arm_reset(mmap->base_addr, mmap->length);
    } else {
        paging_arm_reset(PHYS_MEMORY_START, 0x40000000);
    }

    exceptions_init();

    cp15_invalidate_i_and_d_caches_fast();
    cp15_invalidate_tlb                ();

    kernel_startup_early(                   );
    serial_init         (serial_console_port);
    
    printk(LOG_NOTE, "Barrelfish CPU driver starting on ARMv7 OMAP44xx Board id 0x%08"PRIx32"\n", hal_get_board_id()       );
    printk(LOG_NOTE, "The address of paging_map_kernel_section is %p\n"                         , paging_map_kernel_section);
    
    errval = serial_debug_init();
    if (err_is_fail(errval)) {
        printk(LOG_NOTE, "Failed to initialize debug port: %d", serial_debug_port);
    }

    if (my_core_id != hal_get_cpu_id()) {
        my_core_id = hal_get_cpu_id();
    }

    // Test MMU by remapping the device identifier and reading it using a
    // virtual address
    lpaddr_t id_code_section  = OMAP44XX_MAP_L4_CFG_SYSCTRL_GENERAL_CORE & ~ARM_L1_SECTION_MASK;
    lvaddr_t id_code_remapped = paging_map_device(id_code_section, ARM_L1_SECTION_BYTES);

    omap44xx_id_initialize(&id, (mackerel_addr_t)(id_code_remapped + (OMAP44XX_MAP_L4_CFG_SYSCTRL_GENERAL_CORE & ARM_L1_SECTION_MASK)));
    
    omap44xx_id_code_pr(buf, 200, &id);
    printk(LOG_NOTE, "Using MMU, %s", buf);

    gic_init();
    printk(LOG_NOTE, "gic_init done\n");

    led_remap_register(  );
    led_flash         (10);

    if (hal_cpu_is_bsp()) {
        uint32_t omap_num_cores;

        scu_initialize();

        omap_num_cores = scu_get_core_count();
        printk(LOG_NOTE, "Number of cores in system: %"PRIu32"\n", omap_num_cores);

        // ARM Cortex A9 TRM section 2.1
        if (omap_num_cores > 4) {
            panic("ARM SCU doesn't support more than 4 cores!");
        }

        // init SCU if more than one core present
        if (omap_num_cores > 1) {
            scu_enable();
        }
    }

    tsc_init();
    printf("tsc_init done --\n");
    
    #ifndef __gem5__
        enable_cycle_counter_user_access();
        reset_cycle_counter             ();
    #endif

    // map aux_core_boot_section if necessary
    if (!aux_core_boot_section) {
        lpaddr_t aux_core_boot_section_phys = AUX_CORE_BOOT_0 & ~ARM_L1_SECTION_MASK;

        aux_core_boot_section =          paging_map_device(aux_core_boot_section_phys, ARM_L1_SECTION_BYTES );
        shared_var            = (uint32_t*)(aux_core_boot_section + (AUX_CORE_BOOT_0 & ARM_L1_SECTION_MASK ));

        // Remap device section.
        start_aps_remap(aux_core_boot_section);
    }

        cp15_disable_cache(); // TODO: Find a less drastic way to make spinlocks work.
    arm_kernel_startup();
}

/**
 * Use Mackerel to print the identification from the system
 * configuration block.
 */
static void print_system_identification(void)
{
    char buf[800];
    omap44xx_id_t id;
    omap44xx_id_initialize(&id, (mackerel_addr_t)OMAP44XX_MAP_L4_CFG_SYSCTRL_GENERAL_CORE);
    omap44xx_id_pr(buf, 799, &id);
    printk(LOG_NOTE, "%s", buf);
    omap44xx_id_codevals_prtval(buf, 799, omap44xx_id_code_rawrd(&id));
    printk(LOG_NOTE, "Device is a %s\n", buf);
}


static size_t bank_size(int bank, lpaddr_t base)
{
    int rowbits;
    int colbits;
    int rowsize;
    omap44xx_emif_t emif;
    omap44xx_emif_initialize(&emif, (mackerel_addr_t)base);
    if (omap44xx_emif_status_phy_dll_ready_rdf(&emif)) {
	    rowbits = omap44xx_emif_sdram_config_rowsize_rdf (&emif) + 9;
	    colbits = omap44xx_emif_sdram_config_pagesize_rdf(&emif) + 9;
	    rowsize = omap44xx_emif_sdram_config2_rdbsize_rdf(&emif) + 5;
	    
        printk(LOG_NOTE, "EMIF%d: ready, %d-bit rows, %d-bit cols, %d-byte row buffer\n", bank, rowbits, colbits, 1<<rowsize);
	
        return (1 << (rowbits + colbits + rowsize));
    } else {
	    printk(LOG_NOTE, "EMIF%d doesn't seem to have any DDRAM attached.\n", bank);
	
        return 0;
    }
}

size_t ram_size = 0;

static void size_ram(void)
{
    size_t sz = 0;
    sz = bank_size(1, OMAP44XX_MAP_EMIF1) + bank_size(2, OMAP44XX_MAP_EMIF2);
    printk(LOG_NOTE, "We seem to have 0x%08lx bytes of DDRAM: that's %s.\n", sz, sz == 0x40000000 ? "about right" : "unexpected" );
    ram_size = sz;
}

__attribute__((unused))
static void raw_serial_putchar(char c)
{
    volatile uint32_t *l_uart_thr = (uint32_t *)(0x48020000 + 0x0000);
    volatile uint32_t *l_uart_lsr = (uint32_t *)(0x48020000 + 0x0014);

    // Wait until FIFO can hold more characters (i.e. TX_FIFO_E == 1)
    while ( ((*l_uart_lsr) & 0x20) == 0 ) {
        // Do nothing
    }
    // Write character
    *l_uart_thr = c;
}

void app_core_init(void *pointer);
void  __attribute__ ((noinline,noreturn))
app_core_init(void *pointer)
{
    // NOTE: This is basically the only safe thing to do
    // if started within the ELF image of the first kernel.
    // NOTE that the "real" second kernel doesn't use this function.
    raw_serial_putchar ('*');
    raw_serial_putchar ('\r');
    raw_serial_putchar ('\n');

    arch_init (pointer);
}

extern uint8_t core_id;
/**
 * Entry point called from boot.S for bootstrap processor.
 * if is_bsp == true, then pointer points to multiboot_info
 * else pointer points to a global struct
 */
void arch_init(void *pointer)
{
    serial_early_init(serial_console_port);
    
    if (hal_cpu_is_bsp()) {
        struct multiboot_info *mb = pointer;

        size_t max_addr = max(multiboot_end_addr(mb), (uintptr_t)&kernel_final_byte);

        memset(glbl_core_data, 0, sizeof(struct arm_core_data));
        memset(&global->locks, 0, sizeof(global->locks       ));
        
        glbl_core_data->cmdline         = mb->cmdline                       ;
        glbl_core_data->mods_addr       = mb->mods_addr                     ;
        glbl_core_data->mods_count      = mb->mods_count                    ;        
        glbl_core_data->mmap_addr       = mb->mmap_addr                     ;
        glbl_core_data->mmap_length     = mb->mmap_length                   ;
        glbl_core_data->multiboot_flags = mb->flags                         ;


        // Allocate a free page-sized memory chunk for init to use.
        uint32_t start_free_ram = ROUND_UP(max_addr, BASE_PAGE_SIZE);

        global -> urpc_channel_physical_address = start_free_ram;
        global -> urpc_channel_size_bits = BASE_PAGE_BITS;

        assert ((1<<BASE_PAGE_BITS) == BASE_PAGE_SIZE);

        // The allocated frame changes the region that can be used to map init and devices.
        glbl_core_data->start_free_ram  = start_free_ram + BASE_PAGE_SIZE;


        print_system_identification();
        size_ram                   ();
        set_leds                   (); 

        // Publish the mutliboot info to the second core.
        global->mb_info = mb;
    } else {
        glbl_core_data = (struct arm_core_data*)((lpaddr_t)&kernel_first_byte - BASE_PAGE_SIZE);
        global         = (struct global       *)GLOBAL_VBASE                                   ;

        // TODO: do we get any relevant data here, from init maybe??
        memset(glbl_core_data, 0, sizeof(struct arm_core_data));

        glbl_core_data->cmdline = (lpaddr_t)&glbl_core_data->kernel_cmdline;
//         my_core_id              = glbl_core_data->dst_core_id              ; // huh?
        my_core_id = hal_get_cpu_id();

        struct multiboot_info *mb = global -> mb_info;
        glbl_core_data->mods_addr = mb->mods_addr;
        glbl_core_data->mods_count = mb->mods_count;

        //TODO: See what these are good for. They get used at some point, the values
        // may be different for the APP core.
        glbl_core_data->mmap_addr = mb->mmap_addr;
        glbl_core_data->mmap_length = mb->mmap_length;


        //TODO: This signal has to happen later, when APP is almost ready to boot.
        *((volatile lvaddr_t*)AUX_CORE_BOOT_0) = AP_STARTED;

        size_ram ();
    }

    printk(LOG_NOTE, "Barrelfish OMAP44xx CPU driver starting at addr 0x%"PRIxLVADDR"\n", local_phys_to_mem((uint32_t)&kernel_first_byte));

    paging_init    ();
    cp15_enable_mmu();
    text_init      ();
}

