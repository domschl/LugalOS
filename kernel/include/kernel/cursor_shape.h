#ifndef LUGALOS_KERNEL_CURSOR_SHAPE_H
#define LUGALOS_KERNEL_CURSOR_SHAPE_H

#include <stdint.h>

/* The mouse cursor, a 16 x 16 arrow with its tip at the top left (the
 * pointer's point): one row a word, bit 15 the leftmost pixel. Ink is drawn
 * black, fill white, everything else is left as it is -- so it shows on
 * light and dark alike. A compositor scales it by its chrome scale. */
#define CURSOR_SHAPE_PX 16
static const uint16_t k_cursor_ink[CURSOR_SHAPE_PX]  = { 0x8000, 0xC000, 0xA000, 0x9000, 0x8800, 0x8400, 0x8200, 0x8100, 0x8080, 0x83C0, 0x9200, 0xA900, 0xC900, 0x8480, 0x0480, 0x0300 };
static const uint16_t k_cursor_fill[CURSOR_SHAPE_PX] = { 0x0000, 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 };

#endif /* LUGALOS_KERNEL_CURSOR_SHAPE_H */
