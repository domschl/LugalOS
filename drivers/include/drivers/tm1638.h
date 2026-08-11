/*
 * LugalOS Hardware Driver: QYF-TM1638 8-Digit 7-Segment Display + 4x4 Keypad
 * Vendored and adapted from ~/gith/domschl/LugalChess (firmware/tm1638.c, H2,
 * plan/phase9_chess_computer.md) -- pico-sdk gpio_* calls replaced with the
 * bare-metal SIO register style already established by
 * drivers/uart_rp2350.c's LED handling.
 */

#ifndef LUGALOS_DRIVERS_TM1638_H
#define LUGALOS_DRIVERS_TM1638_H

#include <stdint.h>

/* Initialize GPIO pins and the TM1638 chip */
void tm1638_init(void);

/* Display up to 8 characters on the 7-segment digits ('.' after a character
 * sets that digit's decimal point instead of consuming its own digit slot) */
void tm1638_display_string(const char *str);

/* Set the 8 individual LEDs (bitmask, bit i = LED i) */
void tm1638_set_leds(uint8_t mask);

/* Scan the keypad; returns the pressed key index (0-15) or -1 if none/ambiguous */
int tm1638_get_key(void);

#endif /* LUGALOS_DRIVERS_TM1638_H */
