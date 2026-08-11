/*
 * LugalOS Hardware Driver: ST7735 SPI TFT Canvas for RP2350 (Pico 2)
 * Vendored and adapted from ~/gith/domschl/LugalChess (firmware/st7735.c) --
 * pico-sdk hardware_spi/hardware_gpio calls replaced with the bare-metal
 * register style already established by drivers/spisd_rp2350.c. Chess-board
 * rendering (st7735_draw_board/draw_status, piece bitmaps) stays in
 * LugalChess; this is the generic canvas primitive only (H1,
 * plan/phase9_chess_computer.md), usable by any future display consumer.
 */

#ifndef LUGALOS_DRIVERS_ST7735_H
#define LUGALOS_DRIVERS_ST7735_H

#include <stdint.h>
#include <stddef.h>

#define ST7735_WIDTH  128
#define ST7735_HEIGHT 160

/* RGB565 colors */
#define ST7735_BLACK      0x0000
#define ST7735_WHITE      0xFFFF
#define ST7735_RED        0xF800
#define ST7735_BLUE       0x001F
#define ST7735_GREEN      0x07E0
#define ST7735_YELLOW     0xFFE0
#define ST7735_CYAN       0x07FF
#define ST7735_GRAY       0x8410

void st7735_init(void);
void st7735_fill_screen(uint16_t color);
void st7735_draw_pixel(int x, int y, uint16_t color);
void st7735_draw_rect(int x, int y, int w, int h, uint16_t color);
void st7735_draw_bitmap_mono(int x, int y, int w, int h, const uint16_t *bitmap, uint16_t fg_color, uint16_t bg_color);
void st7735_draw_char(int x, int y, char c, uint16_t color, int size);
void st7735_draw_string(int x, int y, const char *str, uint16_t color, int size);

#endif /* LUGALOS_DRIVERS_ST7735_H */
