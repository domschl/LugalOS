#ifndef LUGALOS_DRIVERS_FONT_MATH_H
#define LUGALOS_DRIVERS_FONT_MATH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fs/latex_math.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t codepoint;   /* Unicode / Math Symbol Codepoint */
    uint8_t  w;           /* Width in pixels */
    uint8_t  h;           /* Height in pixels */
    int8_t   bearing_x;   /* Horizontal bearing */
    int8_t   bearing_y;   /* Vertical bearing above baseline */
    uint8_t  advance;     /* Horizontal advance */
    const uint16_t *rows; /* Bitmap rows (1 bit per pixel, MSB first) */
} math_glyph_t;

/* Extensible delimiter pieces */
typedef struct {
    uint16_t top;
    uint16_t mid;
    uint16_t bot;
    uint16_t ext;
} math_delim_pieces_t;

/* Look up math glyph descriptor in Flash .rodata */
const math_glyph_t *font_math_lookup(uint16_t codepoint, math_style_t style);

/* Look up extensible delimiter pieces */
bool font_math_get_delim_pieces(math_delim_t delim, math_delim_pieces_t *pieces);

/* Basic math font metrics */
int font_math_axis_height(math_style_t style);
int font_math_rule_thickness(math_style_t style);
int font_math_quad(math_style_t style);

#ifdef __cplusplus
}
#endif

#endif /* LUGALOS_DRIVERS_FONT_MATH_H */
