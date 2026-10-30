/* A button as a trigger for setup mode and the owner reset (plan/phase52 1.4): hold it
 * SETUP_BUTTON_SETUP_MS to open a setup window, SETUP_BUTTON_RESET_MS to wipe the owner part.
 * Compiled only for a board that names a button (CONFIG_SETUP_BUTTON_GPIO or _BOOTSEL).
 *
 * A strapping pin held at reset keeps its ROM meaning (download mode); this only reads the pin
 * once running, and a button already down when the task starts must be released first, so a
 * board powered up with it held does not reset its owner. */

#include "lugalos_config.h"

#if defined(CONFIG_SETUP_BUTTON_GPIO) || defined(CONFIG_SETUP_BUTTON_BOOTSEL)

#include "kernel/setup.h"
#include "kernel/status_led.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/hart.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETUP_BUTTON_POLL_MS    100u

#if defined(CONFIG_SETUP_BUTTON_GPIO)
#include "drivers/gpio.h"

static void button_init(void) { (void)gpio_set_mode(CONFIG_SETUP_BUTTON_GPIO, GPIO_MODE_PULLUP); }
static bool button_down(void) { return gpio_read_pin(CONFIG_SETUP_BUTTON_GPIO) == 0; }

#else   /* the RP2350's BOOTSEL */

#define IO_QSPI_SS_CTRL     (*(volatile uint32_t *)0x4003001cUL)
#define SS_CTRL_OEOVER      (3u << 14)
#define SS_CTRL_OE_DISABLE  (2u << 14)
#define SIO_GPIO_HI_IN      (*(volatile uint32_t *)0xd0000008UL)
#define HI_IN_QSPI_CSN      (1u << 27)

/* The button pulls the flash's chip-select low, and that line is only readable while the
 * flash does not drive it -- during which every fetch from flash fails. So: the other core
 * parked in RAM (the flash-write handshake, kernel/smp.c), interrupts off, and this routine in
 * RAM. Pressed = low. */
__attribute__((section(".ramfunc"), noinline))
static bool bootsel_sample(void) {
    uintptr_t saved;
    __asm__ __volatile__("csrrci %0, mstatus, 0x8" : "=r"(saved));
    uint32_t ctrl = IO_QSPI_SS_CTRL;
    IO_QSPI_SS_CTRL = (ctrl & ~SS_CTRL_OEOVER) | SS_CTRL_OE_DISABLE;
    for (volatile int i = 0; i < 1000; i++) { }
    bool down = (SIO_GPIO_HI_IN & HI_IN_QSPI_CSN) == 0;
    IO_QSPI_SS_CTRL = ctrl;
    if (saved & 0x8) __asm__ __volatile__("csrsi mstatus, 0x8");
    return down;
}

static void button_init(void) { }
static bool button_down(void) {
    if (!smp_flash_park_request()) return false;
    bool down = bootsel_sample();
    smp_flash_park_release();
    return down;
}
#endif

static void setup_button_task(void *arg) {
    (void)arg;
    button_init();
    while (button_down()) task_sleep_ms(SETUP_BUTTON_POLL_MS);

    uint64_t since = 0;
    bool setup_done = false, reset_done = false;
    for (;;) {
        task_sleep_ms(SETUP_BUTTON_POLL_MS);
        if (!button_down()) { since = 0; setup_done = reset_done = false; status_led_button(0); continue; }
        uint64_t now = time_get_ms();
        if (!since) since = now;
        uint64_t held = now - since;
        status_led_button((uint32_t)held);
        if (!setup_done && held >= SETUP_BUTTON_SETUP_MS) {
            setup_done = true;
            printk(setup_start() ? "[Setup] button: setup window open\n"
                                 : "[Setup] button: no setup window (no setup secret, or a build without network claiming)\n");
        }
        if (!reset_done && held >= SETUP_BUTTON_RESET_MS) {
            reset_done = true;
            bool ok = setup_reset_owner() == NODE_ID_OK;
            printk("[Setup] button: owner reset %s\n", ok ? "done" : "failed");
            if (ok) {
                status_led_reset_done();
                (void)setup_start();
            }
        }
    }
}

void setup_button_start(void) {
    if (task_create_sized("setupbtn", setup_button_task, NULL, 4) < 0) printk("[Setup] could not start the button task\n");
}

#else
void setup_button_start(void) { }
#endif
