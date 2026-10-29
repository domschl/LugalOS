#ifndef DRIVERS_LEDBTN_K210_H
#define DRIVERS_LEDBTN_K210_H

#include <stdbool.h>
#include <stdint.h>

/* The Maix Bit's RGB LED and USR/BOOT key (drivers/ledbtn_k210.c, 65.6).
 *
 * The schematic (Maix-Bit 1.09) drives a common-anode tri-color LED —
 * anode to 3V3 ("LEDA"), cathodes out through 4K7 — so every LED is
 * ACTIVE LOW: a GPIO driven 0 lights its color. The three cathodes are
 * IO_12 green, IO_13 red, IO_14 blue (schematic columns D11/D12/C11,
 * resistors R17 LED_B / R18 LED_G / R19 LED_R).
 *
 * There is one key, labeled KEY, on IO_16 — but IO_16 is also the BOOT
 * strap, and the CH552's auto-programming transistors sit on that net.
 * Measured on the board: the line reads LOW in every FPIOA pull
 * configuration (no pull; the bit12 pull that made MISO readable; the
 * bit14 the arduino-k210 header calls PU), and toggling DTR/RTS resets
 * the chip through the same transistor pair. While the USB link is up,
 * pressed and released are electrically indistinguishable — it is a
 * strap line shared with the download circuit, not a usable user
 * button. k210_button_pressed() still reports the raw level, and the
 * shell help says what that level is actually worth.
 *
 * Brightness is on/off only (no PWM armed; IO12..14 do have PWM
 * functions if a later milestone wants fades). Any nonzero level is
 * on. Safe to call before sched runs; first call configures the pads.
 * The LED state is the whole "hardware state" there is, written only
 * from the shell task; accepted: last-writer-wins across two consoles. */
void k210_led_set(unsigned r, unsigned g, unsigned b); /* 0/off, else on */
bool k210_button_pressed(void);                        /* true = KEY held */

#endif /* DRIVERS_LEDBTN_K210_H */
