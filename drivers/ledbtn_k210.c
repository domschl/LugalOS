/*
 * LugalOS driver: the Maix Bit RGB LED and KEY button -- 65.6.
 *
 * Wiring (schematic Maix-Bit 1.09, read against the physical board):
 * a common-anode LED (anode "LEDA" on 3V3, cathodes "LEDK" out) through
 * 4K7 series resistors, so a color lights when its GPIO is driven LOW:
 * IO_12 = green, IO_13 = red, IO_14 = blue. The single KEY button is
 * the ISP boot selector on IO_16 -- low at reset starts the BROM
 * download, and low while running is a pressed user button. There is
 * no second button on this board; nothing here writes IO16.
 *
 * Everything travels through drivers/gpio.c's K210 backend (FPIOA pad
 * config plus the SiFive-shaped GPIO block); this file only knows
 * pins, polarity and one-time configuration.
 *
 * accepted: led_state is written from shell task context only, and
 * reading it back stale costs nothing observable -- two shells
 * toggling the LED race harmlessly (last writer wins).
 */

#include "drivers/ledbtn_k210.h"
#include "drivers/gpio.h"

#define LED_GREEN_PIN 12u
#define LED_RED_PIN   13u
#define LED_BLUE_PIN  14u
#define KEY_PIN       16u

/* accepted: one-shot pad setup; every task configures the same way, so
 * a doubled init is harmless and lock-free. */
static bool g_pads_ready;

static void ensure_pads(void) {
    if (g_pads_ready) return;
    /* LEDs off means the latch HIGH first (active-low cathodes), so the
     * color never flashes during configuration. */
    (void)gpio_write_pin(LED_GREEN_PIN, 1);
    (void)gpio_write_pin(LED_RED_PIN, 1);
    (void)gpio_write_pin(LED_BLUE_PIN, 1);
    (void)gpio_set_mode(LED_GREEN_PIN, GPIO_MODE_OUT);
    (void)gpio_set_mode(LED_RED_PIN, GPIO_MODE_OUT);
    (void)gpio_set_mode(LED_BLUE_PIN, GPIO_MODE_OUT);
    (void)gpio_set_mode(KEY_PIN, GPIO_MODE_PULLUP);
    g_pads_ready = true;
}

void k210_led_set(unsigned r, unsigned g, unsigned b) {
    ensure_pads();
    (void)gpio_write_pin(LED_RED_PIN,   r ? 0 : 1);
    (void)gpio_write_pin(LED_GREEN_PIN, g ? 0 : 1);
    (void)gpio_write_pin(LED_BLUE_PIN,  b ? 0 : 1);
}

bool k210_button_pressed(void) {
    ensure_pads();
    return gpio_read_pin(KEY_PIN) == 0;   /* active low, pulled up */
}
