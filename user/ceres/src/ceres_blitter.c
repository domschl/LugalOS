/* user/ceres/src/ceres_blitter.c — 16-Bit RGB565 Fast Rasterizer & Blitter */

#include "ceres_blitter.h"
#include <string.h>
#include <stdarg.h>

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
#include <stdio.h>
extern const uint8_t g_ceres_font8x16[95][16];
#define CERES_GLYPH(idx) g_ceres_font8x16[(idx)]
#else
#include "kernel/printk.h"
#include "drivers/font8x16.h"
#define vsnprintf(buf, sz, fmt, args) kvsnprintf((buf), (sz), (fmt), (args))
#define CERES_GLYPH(idx) font8x16_glyphs[(idx)]
#endif

/* Fast 32-bit aligned 16-bit word span filler */
static inline void fill_span16(uint16_t *dst, uint16_t color, int count) {
    if (count <= 0) return;
    if (((uintptr_t)dst & 2) != 0) {
        *dst++ = color;
        count--;
    }
    uint32_t c32 = ((uint32_t)color << 16) | color;
    uint32_t *dst32 = (uint32_t *)dst;
    while (count >= 2) {
        *dst32++ = c32;
        count -= 2;
    }
    if (count > 0) {
        *(uint16_t *)dst32 = color;
    }
}

void ceres_blit_clear(const ceres_surface_t *surf, uint16_t color) {
    if (!surf || !surf->pixels) return;
    if (color == 0) {
        memset(surf->pixels, 0, (size_t)surf->stride * surf->height * sizeof(uint16_t));
        return;
    }
    for (int y = 0; y < surf->height; y++) {
        fill_span16(surf->pixels + (size_t)y * surf->stride, color, surf->width);
    }
}

void ceres_blit_pixel(const ceres_surface_t *surf, int x, int y, uint16_t color) {
    if (!surf || !surf->pixels) return;
    if (x >= 0 && x < surf->width && y >= 0 && y < surf->height) {
        surf->pixels[(size_t)y * surf->stride + x] = color;
    }
}

void ceres_blit_span(const ceres_surface_t *surf, int x0, int x1, int y, uint16_t color) {
    if (!surf || !surf->pixels || y < 0 || y >= surf->height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 > surf->width) x1 = surf->width;
    if (x0 >= x1) return;
    fill_span16(surf->pixels + (size_t)y * surf->stride + x0, color, x1 - x0);
}

void ceres_blit_vspan(const ceres_surface_t *surf, int x, int y0, int y1, uint16_t color) {
    if (!surf || !surf->pixels || x < 0 || x >= surf->width) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 > surf->height) y1 = surf->height;
    if (y0 >= y1) return;

    uint16_t *ptr = surf->pixels + (size_t)y0 * surf->stride + x;
    int stride = surf->stride;
    for (int y = y0; y < y1; y++) {
        *ptr = color;
        ptr += stride;
    }
}

void ceres_blit_fill_rect(const ceres_surface_t *surf, int x, int y, int w, int h, uint16_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;

    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > surf->width) ? surf->width : (x + w);
    int y1 = (y + h > surf->height) ? surf->height : (y + h);

    if (x0 >= x1 || y0 >= y1) return;

    int span_len = x1 - x0;
    for (int cy = y0; cy < y1; cy++) {
        fill_span16(surf->pixels + (size_t)cy * surf->stride + x0, color, span_len);
    }
}

void ceres_blit_rect_outline(const ceres_surface_t *surf, int x, int y, int w, int h, uint16_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;
    ceres_blit_line(surf, x, y, x + w - 1, y, color);
    ceres_blit_line(surf, x + w - 1, y, x + w - 1, y + h - 1, color);
    ceres_blit_line(surf, x + w - 1, y + h - 1, x, y + h - 1, color);
    ceres_blit_line(surf, x, y + h - 1, x, y, color);
}

void ceres_blit_line(const ceres_surface_t *surf, int x0, int y0, int x1, int y1, uint16_t color) {
    if (!surf || !surf->pixels) return;

    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < surf->width && y0 >= 0 && y0 < surf->height) {
            surf->pixels[(size_t)y0 * surf->stride + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ceres_blit_sprite(const ceres_surface_t *dst, int dst_x, int dst_y,
                          const ceres_surface_t *src, uint16_t key_color) {
    if (!dst || !dst->pixels || !src || !src->pixels) return;

    int x0 = (dst_x < 0) ? 0 : dst_x;
    int y0 = (dst_y < 0) ? 0 : dst_y;
    int x1 = (dst_x + src->width > dst->width) ? dst->width : (dst_x + src->width);
    int y1 = (dst_y + src->height > dst->height) ? dst->height : (dst_y + src->height);

    if (x0 >= x1 || y0 >= y1) return;

    for (int y = y0; y < y1; y++) {
        int src_y = y - dst_y;
        const uint16_t *src_row = src->pixels + (size_t)src_y * src->stride;
        uint16_t *dst_row = dst->pixels + (size_t)y * dst->stride;

        for (int x = x0; x < x1; x++) {
            int src_x = x - dst_x;
            uint16_t pixel = src_row[src_x];
            if (pixel != key_color) {
                dst_row[x] = pixel;
            }
        }
    }
}

void ceres_blit_char(const ceres_surface_t *surf, int x, int y, char c,
                        uint16_t fg, uint16_t bg, bool transparent_bg) {
    if (!surf || !surf->pixels) return;
    if (x + CERES_FONT_W <= 0 || x >= surf->width ||
        y + CERES_FONT_H <= 0 || y >= surf->height) return;

    uint8_t ascii = (uint8_t)c;
    if (ascii < 0x20 || ascii > 0x7E) ascii = '?';
    const uint8_t *glyph = CERES_GLYPH(ascii - 0x20);

    for (int row = 0; row < CERES_FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= surf->height) continue;

        uint8_t bits = glyph[row];
        uint16_t *dst_row = surf->pixels + (size_t)py * surf->stride;

        for (int col = 0; col < CERES_FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= surf->width) continue;

            if (bits & (0x01 << col)) {
                dst_row[px] = fg;
            } else if (!transparent_bg) {
                dst_row[px] = bg;
            }
        }
    }
}

void ceres_blit_text(const ceres_surface_t *surf, int x, int y, const char *str,
                        uint16_t fg, uint16_t bg, bool transparent_bg) {
    if (!surf || !surf->pixels || !str) return;

    int cur_x = x;
    int cur_y = y;

    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            cur_y += CERES_FONT_H;
        } else {
            ceres_blit_char(surf, cur_x, cur_y, *str, fg, bg, transparent_bg);
            cur_x += CERES_FONT_W;
        }
        str++;
    }
}

void ceres_blit_text_fmt(const ceres_surface_t *surf, int x, int y,
                            uint16_t fg, uint16_t bg, bool transparent_bg,
                            const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ceres_blit_text(surf, x, y, buf, fg, bg, transparent_bg);
}
