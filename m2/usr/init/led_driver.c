#include "init.h"

// Predefined physical addresses of GPIO with LED attached.
#define GPIO_BASE 0x4a310000u
#define GPIO_SIZE 0x1000u
#define GPIO_SIZE_BITS 12

// Offset to LED control registers.
#define GPIO_OUTPUT_ENABLE_OFFSET 0x0134
#define GPIO_DATAOUT_OFFSET 0x013C

// The LED input is on Bit Index 8 (the 9th bit)
#define LED_BIT 0x100

static uint32_t virtual_device_base;

/**
 * Map the GPIO device into our virtual address space
 * and initialize LED.
 *
 * \arg frame: The (page-sized) device frame of the GPIO.
 */
static errval_t led_map_device (struct capref frame)
{
    void* buf = 0;
    errval_t error = SYS_ERR_OK;
    int flags = KPI_PAGING_FLAGS_READ | KPI_PAGING_FLAGS_WRITE | KPI_PAGING_FLAGS_NOCACHE;

    // Map the device into virtual address space.
    error = paging_map_frame_attr (get_current_paging_state(), &buf, GPIO_SIZE, frame, flags, NULL, NULL);

    // Store the virtual base pointer.
    virtual_device_base = (uint32_t) buf;

    if (err_is_ok (error)) {
        // Enable output by setting the LED bit to zero (see TRM page 5677).
        volatile uint32_t* led_oe = (volatile uint32_t*) (virtual_device_base + GPIO_OUTPUT_ENABLE_OFFSET);
        *led_oe = *led_oe & ~LED_BIT;
    }

    return error;
}

/**
 * Enable/disable LED.
 * Has no effect if LED device is not correctly initialized.
 *
 * \arg new_state: true for on, false for off.
 */
void led_set_state (bool new_state)
{
    if (virtual_device_base) {
        // Get the address of the register that controls the LED.
        volatile uint32_t* led_reg = (volatile uint32_t*) (virtual_device_base + GPIO_DATAOUT_OFFSET);

        if (new_state) {
            // Enable LED.
            *led_reg = *led_reg | LED_BIT;
        } else {
            // Disable LED.
            *led_reg = *led_reg & ~LED_BIT;
        }
    }
}

/**
 * \brief Initialize the LED driver.
 *
 * Allocate a device frame for LED, map it to virtual
 * address space, and initialize the device itself.
 */
errval_t led_init (void)
{
    struct capref led_cap;
    errval_t error = SYS_ERR_OK;

    error = allocate_device_frame (GPIO_BASE, GPIO_SIZE_BITS, &led_cap);

    if (err_is_ok (error)) {
        error = led_map_device (led_cap);
    }

    return error;
}