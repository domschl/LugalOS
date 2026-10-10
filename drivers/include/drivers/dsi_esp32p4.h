#ifndef LUGALOS_DRIVERS_DSI_ESP32P4_H
#define LUGALOS_DRIVERS_DSI_ESP32P4_H

/* The ESP32-P4's MIPI-DSI host and the LCD-7B's EK79007 panel, 47.4,
 * plan/phase47_esp32p4_lcd7b_ribbon.md. Built where the board file names the
 * panel's reset pin (CONFIG_DSI_LCD_RST_GPIO) on v3 silicon. */

#include "lugalos_config.h"

#include <stdbool.h>

#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_DSI_LCD_RST_GPIO)

#define DSI_LCD_H_RES 1024
#define DSI_LCD_V_RES 600

/* Power, clocks, PHY PLL, the panel's init sequence. Idempotent: the first
 * call does the work, later ones report what it found. */
bool dsi_lcd_init(void);
bool dsi_lcd_is_up(void);

/* `lcd`, `lcd pattern bars|hbars|ber|off`, `lcd bl on|off`, `lcd cmd ..`,
 * `lcd id`. */
void dsi_lcd_command(const char *args);

#endif

#endif /* LUGALOS_DRIVERS_DSI_ESP32P4_H */
