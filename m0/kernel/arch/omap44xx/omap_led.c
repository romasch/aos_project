#include <kernel.h>
#include <paging_kernel_arch.h>
#include <omap44xx_led.h>


#define GPIO_1_BASE 0x4a310000
#define GPIO_OUTPUT_ENABLE_OFFSET 0x0134
#define GPIO_DATAOUT_OFFSET 0x013C

// For the second LED.
// #define GPIO_4_BASE  0x48059000

// The LED input is on Bit Index 8 (the 9th bit)
#define LED_BIT 0x100

static volatile uint32_t* led_oe = (uint32_t *) (GPIO_1_BASE + GPIO_OUTPUT_ENABLE_OFFSET);
static volatile uint32_t* led_dataout = (uint32_t *) (GPIO_1_BASE + GPIO_DATAOUT_OFFSET);

//
// LED section
//

/**
 * Reserve a virtual address for LED registers
 * and map them accordingly.
 */
void led_map_register(void)
{
    // Remap GPIO registers in newly setup address space and ensure
    // that led_flash and co use the new address locations.

    lvaddr_t base = paging_map_device(GPIO_1_BASE, GPIO_DATAOUT_OFFSET + 4);
    uint32_t offset = (GPIO_1_BASE & ARM_L1_SECTION_MASK);

    led_oe = (uint32_t*) (base + offset + GPIO_OUTPUT_ENABLE_OFFSET);
    led_dataout = (uint32_t*) (base + offset + GPIO_DATAOUT_OFFSET);
}

/**
 * Enable/disable LED.
 */
void led_set_state (bool new_state)
{
    if (new_state) {
        // Enable LED.
        *led_dataout = *led_dataout | LED_BIT;
    } else {
        // Disable LED.
        *led_dataout = *led_dataout & ~LED_BIT;
    }
}

/**
 * Wait for approximately one second.
 */
static void wait (void)
{
    for (uint32_t k = 0; k < (1 << 27); k++) {
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

    // Just flash the LED once.
    led_set_state (true);
    wait ();
    led_set_state (false);
    wait ();
}
