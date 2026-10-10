#ifndef LUGALOS_DRIVERS_DSI_ESP32P4_H
#define LUGALOS_DRIVERS_DSI_ESP32P4_H

/* The ESP32-P4's MIPI-DSI host and the LCD-7B's EK79007 panel, 47.4,
 * plan/phase47_esp32p4_lcd7b_ribbon.md. Built where the board file names the
 * panel's reset pin (CONFIG_DSI_LCD_RST_GPIO) on v3 silicon. */

#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_DSI_LCD_RST_GPIO)

#define DSI_LCD_H_RES 1024
#define DSI_LCD_V_RES 600

/* Power, clocks, PHY PLL, the panel's init sequence. Idempotent: the first
 * call does the work, later ones report what it found. */
bool dsi_lcd_init(void);
bool dsi_lcd_is_up(void);

/* 47.5: the frame buffer in PSRAM (NULL until `lcd fb` starts it), one byte
 * per pixel (GRAY8) or two (RGB565), stride DSI_LCD_H_RES pixels; and the
 * write-back of rows [y0, y1) that makes a drawing visible to the DMA. */
uint8_t *dsi_lcd_fb(void);
void dsi_lcd_fb_flush(unsigned y0, unsigned y1);

/* `lcd`, `lcd pattern bars|hbars|ber|off`, `lcd bl on|off`, `lcd cmd ..`,
 * `lcd id`. */
void dsi_lcd_command(const char *args);

#endif

#endif /* LUGALOS_DRIVERS_DSI_ESP32P4_H */
