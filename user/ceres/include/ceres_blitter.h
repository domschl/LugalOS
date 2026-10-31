/* user/ceres/include/ceres_blitter.h — 16-Bit RGB565 Fast Rasterizer & Sprite Compositor */
#ifndef CERES_BLITTER_H
#define CERES_BLITTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ceres_config.h"

/* 16-bit RGB565 Surface Descriptor */
typedef struct {
    uint16_t *pixels;
    int       width;
    int       height;
    int       stride;   /* Stride in pixels (not bytes) */
} ceres_surface_t;

/* Clipping Rectangle (inclusive-exclusive coordinates: [x0, x1), [y0, y1)) */
typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
} ceres_rect_t;

/* Standard Glyph Font Metrics */
#define CERES_FONT_W 8
#define CERES_FONT_H 16

/* Surface Initialization */
static inline ceres_surface_t ceres_surface_make(uint16_t *pixels, int w, int h, int stride) {
    ceres_surface_t s;
    s.pixels = pixels;
    s.width = w;
    s.height = h;
    s.stride = stride;
    return s;
}

/* Color Operations */
static inline uint16_t ceres_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return RGB565(r, g, b);
}

/* 2D Geometric & Span Primitives */
void ceres_blit_clear(const ceres_surface_t *surf, uint16_t color);
void ceres_blit_pixel(const ceres_surface_t *surf, int x, int y, uint16_t color);
void ceres_blit_fill_rect(const ceres_surface_t *surf, int x, int y, int w, int h, uint16_t color);
void ceres_blit_rect_outline(const ceres_surface_t *surf, int x, int y, int w, int h, uint16_t color);
void ceres_blit_line(const ceres_surface_t *surf, int x0, int y0, int x1, int y1, uint16_t color);

/* Fast 32-bit Word Aligned Horizontal Span Fill */
void ceres_blit_span(const ceres_surface_t *surf, int x0, int x1, int y, uint16_t color);

/* Fast Vertical Column Fill (for terrain heightfields) */
void ceres_blit_vspan(const ceres_surface_t *surf, int x, int y0, int y1, uint16_t color);

/* Sprite & Bitmap Blitting with Color-Key Transparency */
void ceres_blit_sprite(const ceres_surface_t *dst, int dst_x, int dst_y,
                          const ceres_surface_t *src, uint16_t key_color);

/* Bitmap Typography */
void ceres_blit_char(const ceres_surface_t *surf, int x, int y, char c,
                        uint16_t fg, uint16_t bg, bool transparent_bg);

void ceres_blit_text(const ceres_surface_t *surf, int x, int y, const char *str,
                        uint16_t fg, uint16_t bg, bool transparent_bg);

void ceres_blit_text_fmt(const ceres_surface_t *surf, int x, int y,
                            uint16_t fg, uint16_t bg, bool transparent_bg,
                            const char *fmt, ...);

#endif /* CERES_BLITTER_H */
