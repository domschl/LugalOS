#ifndef LUGALOS_DRIVERS_TTF_H
#define LUGALOS_DRIVERS_TTF_H

#include <stdbool.h>
#include <stdint.h>

/* Freestanding integer/fixed-point TrueType (glyf) font parser and
 * anti-aliased scanline rasterizer. Zero floating-point, zero libc heap. */

#define TTF_MAX_GLYPH_W 96
#define TTF_MAX_GLYPH_H 96

typedef int (*ttf_read_fn)(void *ctx, uint32_t off, void *dst, uint32_t len);

typedef struct {
    ttf_read_fn read_fn;
    void       *read_ctx;
    uint32_t    file_size;

    uint32_t    hmtx_off;
    uint32_t    hmtx_len;
    uint32_t    loca_off;
    uint32_t    loca_len;
    uint32_t    glyf_off;
    uint32_t    glyf_len;
    uint32_t    cmap_sub_off;
    uint32_t    cmap_sub_len;
    uint16_t    cmap_fmt;

    uint32_t    gsub_off;
    uint32_t    gsub_len;
    uint32_t    gpos_off;
    uint32_t    gpos_len;
    uint32_t    kern_off;
    uint32_t    kern_len;

    uint16_t    units_per_em;
    int16_t     ascent;
    int16_t     descent;
    int16_t     line_gap;
    uint16_t    num_hmetrics;
    uint16_t    num_glyphs;
    uint16_t    loca_fmt;       /* 0: 16-bit (*2), 1: 32-bit */
} ttf_font_t;

typedef struct {
    int16_t  xoff;              /* left bearing relative to pen x, px */
    int16_t  yoff;              /* top offset relative to cell top, px */
    uint8_t  w;                 /* bitmap width, px */
    uint8_t  h;                 /* bitmap height, px */
    uint8_t  stride;            /* bytes per row: (w + 1) / 2 */
    uint8_t  advance;           /* horizontal advance, px */
} ttf_glyph_metrics_t;

/* Scratch workspace for outline decoding and analytic scanline rasterization.
 * Caller-provided (on PSRAM or host heap) so non-TTF targets pay zero RAM. */
#define TTF_MAX_POINTS   1024
#define TTF_MAX_CONTOURS 128
#define TTF_MAX_EDGES    4096

typedef struct {
    int32_t x0, y0, x1, y1;     /* 26.6 fixed-point, y in cell space */
} ttf_edge_t;

typedef struct {
    int32_t    px[TTF_MAX_POINTS];
    int32_t    py[TTF_MAX_POINTS];
    uint8_t    on[TTF_MAX_POINTS];
    uint16_t   end_pts[TTF_MAX_CONTOURS];
    ttf_edge_t edges[TTF_MAX_EDGES];
    int32_t    cover[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H];
    int32_t    area[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H];
} ttf_scratch_t;

/* Open a TrueType font from a memory buffer or custom read callback. */
bool ttf_open_mem(ttf_font_t *f, const uint8_t *data, uint32_t size);
bool ttf_open_io(ttf_font_t *f, ttf_read_fn read_fn, void *ctx, uint32_t size);

/* Map Unicode codepoint to glyph ID (0 = .notdef if absent). */
uint16_t ttf_glyph_index(const ttf_font_t *f, uint32_t codepoint);

/* Scaled cell baseline (px from top of cell_h) and advance for codepoint or gid. */
int ttf_baseline_px(const ttf_font_t *f, int cell_h);
int ttf_advance_px(const ttf_font_t *f, int cell_h, uint32_t codepoint);
int ttf_gid_advance_px(const ttf_font_t *f, int cell_h, uint16_t gid);

/* Rasterize codepoint or glyph ID at cell_h into 4-bit packed coverage (out_aa,
 * max TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H / 2 bytes). Low nibble = even x,
 * high nibble = odd x, values 0..15. Returns false only on malformed input. */
bool ttf_rasterize_glyph(const ttf_font_t *f, ttf_scratch_t *sc, int cell_h,
                         uint32_t codepoint, ttf_glyph_metrics_t *out_m,
                         uint8_t *out_aa, uint32_t out_cap);
bool ttf_rasterize_glyph_id(const ttf_font_t *f, ttf_scratch_t *sc, int cell_h,
                            uint16_t gid, ttf_glyph_metrics_t *out_m,
                            uint8_t *out_aa, uint32_t out_cap);

#endif /* LUGALOS_DRIVERS_TTF_H */
