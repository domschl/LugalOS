#ifndef LUGALOS_DRIVERS_LCD7_H
#define LUGALOS_DRIVERS_LCD7_H

#include <stdint.h>

/* The RP2350-LCD-7's 800x480 RGB panel (ST7262), plan/phase36 36.3 onward.
 * Stubs everywhere else; the real body is built where the board file
 * declares CONFIG_LCD_PCLK_GPIO. */

/* Power, reset, backlight, and the PIO/DMA scan-out. Returns 0 when the
 * panel is being driven. Needs palloc; takes ~220 ms (the panel's reset). */
int lcd7_init(void);

/* 36.3: the whole screen one RGB565 colour (the data pins held from SIO). */
void lcd7_set_colour(uint16_t rgb565);

/* 0..100; 0 also switches the backlight converter off. */
void lcd7_set_backlight(unsigned percent);

/* `lcd` in the shell: what the scan-out is doing. */
void lcd7_report(void);

#endif /* LUGALOS_DRIVERS_LCD7_H */
