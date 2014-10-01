#include <romasch_paging.h>
#include <cp15.h>
#include <string.h>

static lvaddr_t device_section_base = 0;

lvaddr_t uart_base (void)
{
	return device_section_base;
}

/**
 * NOTE: This function was implemented before I realized
 * that most functionality is already provided in paging.c!
 * The translation actually works and the UART can be correctly
 * initialized to print "MMU enabled".
 * 
 * This function sets up initial page tables for the kernel's 
 * own memory, a small device section, and the page tables.
 * The physical and virtual addresses of all kernel memory 
 * and page tables are the same.
 * 
 * The virtual memory layout imposed by this function is as follows:
 * 
 * ---------- 0x 8204 0000 (or &kernel_first_byte)
 * 
 * KERNEL (incl. stack)
 * 
 * ---------- &kernel_final_byte
 * 
 * padding
 * 
 * ---------- 0x 8206 0000 (next 16KiB aligned address)
 * 
 * Reserved for UART
 * 
 * ---------- 0x 8206 4000
 * 
 * First-Level Page Table
 * 
 * ---------- 0x 8206 8000
 * 
 * Second-Level Page Tables
 * 
 * 		...
 * 
 */
void romasch_paging_init (void)
{
	/**
	 * Do not relocate the kernel's memory!
	 * 
	 * This means the virtual address should be exactly
	 * the same as the physical address for the kernel's text
	 * and data sections, the initial stack, and the
	 * initial translation tables.
	 */
	
	// These two addresses encapsulate the kernel's 
	// text, data and the initial stack.
	uint8_t* kernel_begin = &kernel_first_byte;
	uint8_t* kernel_end = &kernel_final_byte;
	
	uint32_t kb_16 = 16*1024;
	
	// Get the next 16 KiB aligned address:
	uint8_t* ttbr1 = (kernel_end + kb_16);
	ttbr1 = (uint8_t*) ((uint32_t) ttbr1 & (~ (kb_16 - 1)));
	
	// Use this 16 KiB section for device mappings.
	device_section_base = (lvaddr_t) ttbr1;
	OUT(device_section_base);
	
	// Reserve the next 16 KiB section for the first-level page table.
	ttbr1 = ttbr1 + kb_16;
	OUT(ttbr1);
	
	// Make sure we write to zeroed memory.
	memset (ttbr1, 0, kb_16);
	
	// Now we have an empty first-level page table.
	// Let's initialize it.
	
	// Luckily the kernel start address is already 4 KiB aligned...
	uint8_t* page_addr = kernel_begin;
	uint8_t* max_memory = ttbr1 + kb_16;
	
	while (page_addr < max_memory) {
		OUT (page_addr);
		OUT (max_memory);
		
		
		// First we need to create an empty 2nd level table.
		uint8_t* page_table = max_memory;
		memset (page_table, 0, 1024);
		
		// The page table increases the section that needs to be mapped.
		max_memory += 1024;
		
		// Put the page table address to the correct location in the first-level page table.
		
		// Bits [31:20] a virtual address is used as an index for the first-level table.
		// They should replace Bits [14:2] of the TTBR1 value.
		// The address of the second-level page table should be 
		// stored a the resulting position.
		
		uint32_t index = (uint32_t) page_addr >> 20;
		index = index << 2; // Last two bits must be zero.
		
		uint32_t* firstlevel_descriptor_address = (uint32_t*) ((uint32_t) ttbr1 | index);
		
		OUT (firstlevel_descriptor_address);
		
		// Generate the 1st level descriptor.
		// Bits [9:2] should already be zero as the table is 1 KiB aligned.
		// Bits [1:0] should be 01.
		uint32_t firstlevel_descriptor = ( (uint32_t) page_table | 1);
		
		OUT (firstlevel_descriptor);
		
		// Initialize the table entry.
		*firstlevel_descriptor_address = firstlevel_descriptor;
		
		printf ("Survived dereference\n");
		
		// We can now fill all 256 entries of the 2nd level page 
		// table with consecutive 4 KiB blocks.
		for (int i=0; i<256 && page_addr < max_memory; ++i) {
			
			// Bits [19:12] of a virtual address are used as index for the second-level table.
			// They should replace bits [11:2] of the 2nd level table base address.
			
			// Delete upper bits.
			uint32_t secondlevel_index = (uint32_t) page_addr << 12;
			// Delete lower bits.
			secondlevel_index = secondlevel_index >> 24;
			// Move to correct position.
			secondlevel_index = secondlevel_index << 2;
			
			uint32_t* secondlevel_descriptor_address = (uint32_t*) ((uint32_t) page_table | secondlevel_index);
			
			OUT (secondlevel_descriptor_address);
			
			// Generate the 2nd-level descriptor.
			// Bits [31:12] should correspond to the page address.
			// Bits [11:2] should be zero.
			// Bits [1:0] should be 10.
			
			uint32_t secondlevel_descriptor = (uint32_t) page_addr;
			// Bits [11:0] are already zero due to alignment.
			secondlevel_descriptor = secondlevel_descriptor | 2;
			
			// ap10 access bits
			secondlevel_descriptor = secondlevel_descriptor | 16;
			
			OUT(secondlevel_descriptor);
			
			// Initialize the page table entry.
			*secondlevel_descriptor_address = secondlevel_descriptor;
			
			// Now another page of 4 KiB is mapped.
			// increase page address.
			page_addr = page_addr + 4*1024;
		}
	}
	
	printf ("Finished basic initialization.\n");
	
	
	// Map UART device.
	
	// The 16 KiB reserved for device mappings earlier are already 
	// mapped to the corresponding physical memory area.
	// Now we need to adapt the page table mappings.
	
	// Let's just map the UART device to the beginning, i.e.
	//		device_section_base -> 0x48020000
	
	// Do a "manual" lookup to get the page table entry.
	
	uint32_t* firstlvl_desc_addr = (uint32_t*) ((uint32_t) ttbr1 | ((uint32_t) device_section_base >> 20 << 2));
	OUT (ttbr1);
	OUT(firstlvl_desc_addr);
	
	uint32_t firstlvl_desc = *firstlvl_desc_addr;
	
	OUT (firstlvl_desc);
	printf ("Survived dereference\n");
	
	uint32_t* secondlvl_desc_addr = (uint32_t*) ((firstlvl_desc-1) | ((uint32_t) device_section_base << 12 >> 24 << 2));
	
	OUT(secondlvl_desc_addr);
	
	// Check correctness.
	printf ("Virtual address: %X, physical address: %X\n", device_section_base, *secondlvl_desc_addr);
	
	*secondlvl_desc_addr = (0x48020002 | 48); // ap10 Access bits.
	printf ("Virtual address: %X, physical address: %X\n", device_section_base, *secondlvl_desc_addr);
	
	cp15_write_ttbr1 ((uint32_t) ttbr1);
	cp15_write_ttbcr (1);
//*/
}
 
/**
 * 
 * Simulate a translation of a virtual address.
 * 
 * Only works for small pages or small sections translated by TTBR1.
 * The page table base address is taken directly from the TTBR1 register.
 * 
 */
lpaddr_t test_translate (lvaddr_t address)
{
	uint32_t ttbr1 = cp15_read_ttbr1();
	
	uint32_t descriptor_lvl1_addr = ttbr1 | (address >> 20 << 2);
	
	uint32_t descriptor_lvl1 = *((uint32_t*) descriptor_lvl1_addr);
	
	// Only deal with small section or small page addresses.
	if (descriptor_lvl1 & 1) { // small page
		
		uint32_t descriptor_lvl2_base = descriptor_lvl1 >> 10 << 10;
		uint32_t descriptor_lvl2_index = address << 12 >> 24 << 2;
		
		uint32_t* descriptor_lvl2_address = (uint32_t*) (descriptor_lvl2_base | descriptor_lvl2_index);
		
		uint32_t descriptor_lvl2 = *descriptor_lvl2_address;
		
		uint32_t page_base = descriptor_lvl2 >> 12 << 12;
		uint32_t page_index = address << 20 >> 20;
		
		return page_base | page_index;
		
	} else { // small section
		
			// Make last 20 bits zero.
		uint32_t section_base = descriptor_lvl1 >> 20 << 20;
		
		uint32_t section_index = address << 12 >> 12;
		
		return section_base | section_index;
	}
}