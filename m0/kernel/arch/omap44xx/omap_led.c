#include <kernel.h>
#include <paging_kernel_arch.h>
#include <omap44xx_led.h>


#define GPIO_1_BASE 0x4a310000
#define GPIO_OUTPUT_ENABLE_OFFSET 0x0134
#define GPIO_DATAOUT_OFFSET 0x013C

// #define GPIO_4_BASE  0x48059000

// The LED input is on Bit Index 8 (the 9th bit)
#define LED_BIT 0x100
// Also possible:
// #define LED_BIT (1<<8)

static volatile uint32_t* led_oe = (uint32_t *) (GPIO_1_BASE + GPIO_OUTPUT_ENABLE_OFFSET);
static volatile uint32_t* led_dataout = (uint32_t *) (GPIO_1_BASE + GPIO_DATAOUT_OFFSET);

//
// LED section
//

/*
 * You will need to implement this function for the milestone 1 extra
 * challenge.
 */
void led_map_register(void)
{
    // TODO: remap GPIO registers in newly setup address space and ensure
    // that led_flash and co use the new address locations.
}

/*
 * Enable/disable led
 * TODO: This function might be useful for the extra challenge in
 * milestone 1.
 */
void led_set_state(bool new_state)
{
}

/*
 * Wait for approximately one second.
 */
static void wait (void)
{
	for (uint32_t k = 0; k < (1 << 28); k++) {
		// Inject an assembly 'nop', otherwise the compiler
		// will optimize away the loop.
		__asm__("nop");
	}
}

/*
 * Flash the LED On the pandaboard
 */
void led_flash(void)
{	
	// Enable output by setting the LED bit to zero (see TRM page 5677).
	*led_oe = *led_oe & ~LED_BIT;

	// TODO: you'll want to change the infinite loop here for milestone 1.
    for (int i=0; i<1; ++i) {
		*led_dataout = *led_dataout | LED_BIT;
		wait ();
		*led_dataout = *led_dataout & ~LED_BIT;
		wait ();
    }
}
