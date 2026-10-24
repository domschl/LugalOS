#ifndef MOON_BLITTER_H
#define MOON_BLITTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "moon_config.h"
#include "moon_fixed.h"

/* 16-bit RGB565 Surface Descriptor */
typedef struct {
    uint16_t *pixels;
    int       width;
    int       height;
    int       stride;       /* Stride in pixels (not bytes) */
} moon_surface_t;

/* Clipping Rectangle (inclusive-exclusive coordinates) */
typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
} moon_rect_t;

/* Standard Glyph Font Metrics */
#define MOON_FONT_W 8
#define MOON_FONT_H 16

/* Surface Initialization */
static inline moon_surface_t moon_surface_make(uint16_t *pixels, int w, int h, int stride) {
    moon_surface_t s;
    s.pixels = pixels;
    s.width = w;
    s.height = h;
    s.stride = stride;
    return s;
}

/* Color Operations */
static inline uint16_t moon_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return RGB565(r, g, b);
}

/* Dim an RGB565 color by factor (0 = pitch black, 256 = original brightness) */
uint16_t moon_color_dim(uint16_t color, uint16_t factor_256);

/* Linearly interpolate between two RGB565 colors */
uint16_t moon_color_lerp(uint16_t c1, uint16_t c2, fixed32_t t);

/* 2D Geometric Primitives */
void moon_blit_clear(const moon_surface_t *surf, uint16_t color);
void moon_blit_pixel(const moon_surface_t *surf, int x, int y, uint16_t color);
void moon_blit_fill_rect(const moon_surface_t *surf, int x, int y, int w, int h, uint16_t color);
void moon_blit_rect_outline(const moon_surface_t *surf, int x, int y, int w, int h, uint16_t color);
void moon_blit_line(const moon_surface_t *surf, int x0, int y0, int x1, int y1, uint16_t color);
void moon_blit_circle(const moon_surface_t *surf, int cx, int cy, int radius, uint16_t color);
void moon_blit_circle_filled(const moon_surface_t *surf, int cx, int cy, int radius, uint16_t color);

/* Isometric Terrain and Structure Primitives */
void moon_blit_iso_tile(const moon_surface_t *surf, int sx, int sy,
                        int half_w, int half_h,
                        uint16_t fill_color, uint16_t border_color);

/* Renders a 3D isometric extruded block (top diamond, front-left wall, front-right wall) */
void moon_blit_iso_block(const moon_surface_t *surf, int sx, int sy,
                         int half_w, int half_h, int height_px,
                         uint16_t top_color, uint16_t left_color,
                         uint16_t right_color, uint16_t border_color);

/* Optional cache of the baked tops of textured tiles. The platform hands over moon_tile_cache_bytes()
 * of 4-byte aligned memory (PSRAM on the boards: about 8.7 MB, filled lazily as tiles come into
 * view); NULL detaches it. Without it every tile is sampled as it is drawn, with the same result. */
size_t moon_tile_cache_bytes(void);
void moon_tile_cache_attach(void *mem);

/* Procedural Textured Terrain & Stratified Geological Block Primitives */
void moon_blit_iso_tile_textured(const moon_surface_t *surf, int sx, int sy,
                                 int half_w, int half_h,
                                 int gx, int gy, uint8_t terrain,
                                 uint8_t flags, uint16_t border_color);

void moon_blit_iso_block_textured(const moon_surface_t *surf, int sx, int sy,
                                  int half_w, int half_h, int height_px,
                                  int gx, int gy, uint8_t terrain,
                                  uint8_t flags, uint16_t border_color);

/* World tint folded into the drawing: while set, the terrain, structure and cable routines
 * below scale every colour they write by the given per-channel factors (256 = unchanged),
 * with exactly the result of drawing untinted and then running moon_blit_tint_rect() over
 * the frame, without the extra pass over memory. Set back to 256 for the UI. */
void moon_blit_set_tint(uint16_t r_256, uint16_t g_256, uint16_t b_256);
uint16_t moon_blit_tint_color(uint16_t color);   /* a colour as the set tint would draw it */

/* Scale each colour channel of a rectangle (256 = unchanged, each <= 256) */
void moon_blit_tint_rect(const moon_surface_t *surf, int x, int y, int w, int h,
                         uint16_t r_256, uint16_t g_256, uint16_t b_256);

/* Average colour of a textured terrain top / cliff face (face_type 1 = shadowed, 2 = lit);
 * the flat renderer uses these so both modes share one palette. */
uint16_t moon_terrain_mean_color(uint8_t terrain);
uint16_t moon_cliff_mean_color(uint8_t terrain, int face_type);

/* Sprite & Bitmap Blitting with Color-Key Transparency */
void moon_blit_sprite(const moon_surface_t *dst, int dst_x, int dst_y,
                      const moon_surface_t *src, uint16_t key_color);

/* Translucent sprite: tinted and mixed over the destination (alpha 0..256) */
void moon_blit_sprite_blend(const moon_surface_t *dst, int dst_x, int dst_y,
                            const moon_surface_t *src, uint16_t key_color,
                            uint16_t tint, uint16_t alpha_256);

void moon_blit_subsprite(const moon_surface_t *dst, int dst_x, int dst_y,
                         const moon_surface_t *sheet, int src_x, int src_y,
                         int w, int h, uint16_t key_color);

/* Bitmap Typography */
void moon_blit_char(const moon_surface_t *surf, int x, int y, char c,
                    uint16_t fg, uint16_t bg, bool transparent_bg);

void moon_blit_text(const moon_surface_t *surf, int x, int y, const char *str,
                    uint16_t fg, uint16_t bg, bool transparent_bg);

void moon_blit_text_fmt(const moon_surface_t *surf, int x, int y,
                        uint16_t fg, uint16_t bg, bool transparent_bg,
                        const char *fmt, ...);

#endif /* MOON_BLITTER_H */
