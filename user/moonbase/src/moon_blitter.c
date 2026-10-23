#include "moon_blitter.h"
#include "moon_grid.h"
#include "moon_textures.h"
#include <stdarg.h>
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
#include <stdio.h>
#else
#include "kernel/printk.h"
#define vsnprintf(buf, sz, fmt, args) kvsnprintf((buf), (sz), (fmt), (args))
#endif

extern const uint8_t g_moon_font8x16[95][16];

/*
 * Color manipulation
 */
uint16_t moon_color_dim(uint16_t color, uint16_t factor_256) {
    if (factor_256 == 0) return 0;
    if (factor_256 >= 256) return color;

    uint32_t r = (color >> 11) & 0x1F;
    uint32_t g = (color >> 5) & 0x3F;
    uint32_t b = color & 0x1F;

    r = (r * factor_256) >> 8;
    g = (g * factor_256) >> 8;
    b = (b * factor_256) >> 8;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t moon_color_lerp(uint16_t c1, uint16_t c2, fixed32_t t) {
    if (t <= 0) return c1;
    if (t >= FIXED_ONE) return c2;

    int32_t r1 = (c1 >> 11) & 0x1F;
    int32_t g1 = (c1 >> 5) & 0x3F;
    int32_t b1 = c1 & 0x1F;

    int32_t r2 = (c2 >> 11) & 0x1F;
    int32_t g2 = (c2 >> 5) & 0x3F;
    int32_t b2 = c2 & 0x1F;

    int32_t r = r1 + (int32_t)(((int64_t)(r2 - r1) * t) >> FIXED_SHIFT);
    int32_t g = g1 + (int32_t)(((int64_t)(g2 - g1) * t) >> FIXED_SHIFT);
    int32_t b = b1 + (int32_t)(((int64_t)(b2 - b1) * t) >> FIXED_SHIFT);

    if (r < 0) r = 0; else if (r > 31) r = 31;
    if (g < 0) g = 0; else if (g > 63) g = 63;
    if (b < 0) b = 0; else if (b > 31) b = 31;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/*
 * 2D Geometric Primitives
 */
void moon_blit_clear(const moon_surface_t *surf, uint16_t color) {
    if (!surf || !surf->pixels) return;
    for (int y = 0; y < surf->height; y++) {
        uint16_t *row = surf->pixels + y * surf->stride;
        for (int x = 0; x < surf->width; x++) {
            row[x] = color;
        }
    }
}

void moon_blit_pixel(const moon_surface_t *surf, int x, int y, uint16_t color) {
    if (!surf || !surf->pixels) return;
    if (x >= 0 && x < surf->width && y >= 0 && y < surf->height) {
        surf->pixels[y * surf->stride + x] = color;
    }
}

void moon_blit_fill_rect(const moon_surface_t *surf, int x, int y, int w, int h, uint16_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;

    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > surf->width) ? surf->width : (x + w);
    int y1 = (y + h > surf->height) ? surf->height : (y + h);

    if (x0 >= x1 || y0 >= y1) return;

    for (int cy = y0; cy < y1; cy++) {
        uint16_t *row = surf->pixels + cy * surf->stride;
        for (int cx = x0; cx < x1; cx++) {
            row[cx] = color;
        }
    }
}

void moon_blit_rect_outline(const moon_surface_t *surf, int x, int y, int w, int h, uint16_t color) {
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;
    moon_blit_line(surf, x, y, x + w - 1, y, color);
    moon_blit_line(surf, x + w - 1, y, x + w - 1, y + h - 1, color);
    moon_blit_line(surf, x + w - 1, y + h - 1, x, y + h - 1, color);
    moon_blit_line(surf, x, y + h - 1, x, y, color);
}

void moon_blit_line(const moon_surface_t *surf, int x0, int y0, int x1, int y1, uint16_t color) {
    if (!surf || !surf->pixels) return;

    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (x0 >= 0 && x0 < surf->width && y0 >= 0 && y0 < surf->height) {
            surf->pixels[y0 * surf->stride + x0] = color;
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

void moon_blit_circle(const moon_surface_t *surf, int cx, int cy, int radius, uint16_t color) {
    if (!surf || !surf->pixels || radius < 0) return;

    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        moon_blit_pixel(surf, cx + x, cy + y, color);
        moon_blit_pixel(surf, cx + y, cy + x, color);
        moon_blit_pixel(surf, cx - y, cy + x, color);
        moon_blit_pixel(surf, cx - x, cy + y, color);
        moon_blit_pixel(surf, cx - x, cy - y, color);
        moon_blit_pixel(surf, cx - y, cy - x, color);
        moon_blit_pixel(surf, cx + y, cy - x, color);
        moon_blit_pixel(surf, cx + x, cy - y, color);

        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

void moon_blit_circle_filled(const moon_surface_t *surf, int cx, int cy, int radius, uint16_t color) {
    if (!surf || !surf->pixels || radius < 0) return;

    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= surf->height) continue;

        int dx_max = 0;
        while ((dx_max + 1) * (dx_max + 1) + dy * dy <= r2) {
            dx_max++;
        }

        int x0 = cx - dx_max;
        int x1 = cx + dx_max;
        if (x0 < 0) x0 = 0;
        if (x1 >= surf->width) x1 = surf->width - 1;

        if (x0 <= x1) {
            uint16_t *row = surf->pixels + py * surf->stride;
            for (int px = x0; px <= x1; px++) {
                row[px] = color;
            }
        }
    }
}

/*
 * Isometric Terrain & Block Primitives
 */
void moon_blit_iso_tile(const moon_surface_t *surf, int sx, int sy,
                        int half_w, int half_h,
                        uint16_t fill_color, uint16_t border_color) {
    if (!surf || !surf->pixels) return;

    /* Fill horizontal spans across diamond */
    for (int y = -half_h; y <= half_h; y++) {
        int py = sy + y;
        if (py < 0 || py >= surf->height) continue;

        int span = (y <= 0) ? (half_h + y) : (half_h - y);
        int half_span_x = (half_w > 0) ? (span * half_w / half_h) : 0;

        int lx = sx - half_span_x;
        int rx = sx + half_span_x;
        if (lx < 0) lx = 0;
        if (rx >= surf->width) rx = surf->width - 1;

        if (lx <= rx) {
            uint16_t *row = surf->pixels + py * surf->stride;
            for (int x = lx; x <= rx; x++) {
                row[x] = fill_color;
            }
        }
    }

    /* Outlines */
    if (border_color != fill_color) {
        moon_blit_line(surf, sx, sy - half_h, sx + half_w, sy, border_color);
        moon_blit_line(surf, sx + half_w, sy, sx, sy + half_h, border_color);
        moon_blit_line(surf, sx, sy + half_h, sx - half_w, sy, border_color);
        moon_blit_line(surf, sx - half_w, sy, sx, sy - half_h, border_color);
    }
}

void moon_blit_iso_block(const moon_surface_t *surf, int sx, int sy,
                         int half_w, int half_h, int height_px,
                         uint16_t top_color, uint16_t left_color,
                         uint16_t right_color, uint16_t border_color) {
    if (!surf || !surf->pixels) return;

    int top_sy = sy - height_px;

    /* 1. Left Vertical Face */
    /* Spans x from (sx - half_w) to sx */
    for (int x = sx - half_w; x <= sx; x++) {
        if (x < 0 || x >= surf->width) continue;

        /* Top slope: from (sy - height_px) at (sx - half_w) down to (sy + half_h - height_px) at sx */
        int progress = x - (sx - half_w); /* 0 .. half_w */
        int y_top = (top_sy) + (progress * half_h / half_w);
        int y_bot = y_top + height_px;

        if (y_top < 0) y_top = 0;
        if (y_bot >= surf->height) y_bot = surf->height - 1;

        for (int y = y_top; y <= y_bot; y++) {
            surf->pixels[y * surf->stride + x] = left_color;
        }
    }

    /* 2. Right Vertical Face */
    /* Spans x from sx to (sx + half_w) */
    for (int x = sx; x <= sx + half_w; x++) {
        if (x < 0 || x >= surf->width) continue;

        int progress = x - sx; /* 0 .. half_w */
        int y_top = (top_sy + half_h) - (progress * half_h / half_w);
        int y_bot = y_top + height_px;

        if (y_top < 0) y_top = 0;
        if (y_bot >= surf->height) y_bot = surf->height - 1;

        for (int y = y_top; y <= y_bot; y++) {
            surf->pixels[y * surf->stride + x] = right_color;
        }
    }

    /* 3. Top Face Diamond */
    moon_blit_iso_tile(surf, sx, top_sy, half_w, half_h, top_color, border_color);

    /* 4. Vertical Corner Outlines */
    if (border_color != top_color) {
        moon_blit_line(surf, sx - half_w, top_sy, sx - half_w, sy, border_color);
        moon_blit_line(surf, sx, top_sy + half_h, sx, sy + half_h, border_color);
        moon_blit_line(surf, sx + half_w, top_sy, sx + half_w, sy, border_color);
        moon_blit_line(surf, sx - half_w, sy, sx, sy + half_h, border_color);
        moon_blit_line(surf, sx, sy + half_h, sx + half_w, sy, border_color);
    }
}

/*
 * Procedural Terrain Micro-Texturing & Cliff Stratification
 */
static inline uint32_t micro_noise(int32_t gx, int32_t gy, int32_t px, int32_t py) {
    uint32_t ugx = (uint32_t)gx;
    uint32_t ugy = (uint32_t)gy;
    uint32_t upx = (uint32_t)px;
    uint32_t upy = (uint32_t)py;
    uint32_t h = (ugx * 73856093u) ^ (ugy * 19349663u) ^ (upx * 83492791u) ^ (upy * 2654435761u);
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return h;
}

static uint16_t sample_terrain_pixel(int gx, int gy, int px, int py, int sx, int sy,
                                     uint8_t terrain, uint8_t flags) {
    int rel_x = px - sx;
    int rel_y = py - sy;

    /* Sintered Regolith Paver (Foundation Pad) */
    if (flags & MOON_TILE_FLAG_FOUNDATION) {
        if (((rel_x + rel_y * 2) % 14 == 0) || ((rel_x - rel_y * 2) % 14 == 0)) {
            return RGB565(42, 42, 48); /* paver seam line */
        }
        if ((rel_x == 0 || rel_x == -1) && (rel_y == 0 || rel_y == -1)) {
            return COLOR_GOLD_FOIL; /* survey anchor mark */
        }
        return RGB565(110, 110, 116);
    }

    /* World-space continuous 2:1 isometric mapping into seamless 64x64 regolith texture */
    int u = (gx * 32) + (rel_y * 2) + rel_x;
    int v = (gy * 32) + (rel_y * 2) - rel_x;
    uint32_t tu = (uint32_t)u & (TEX_REGOLITH_W - 1);
    uint32_t tv = (uint32_t)v & (TEX_REGOLITH_H - 1);
    uint16_t base = g_tex_regolith[(tv << 6) | tu];

    uint32_t n = micro_noise(gx, gy, px, py);
    uint32_t var = (n >> 28) & 0x0F; /* 0..15 */

    switch (terrain) {
        case MOON_TERRAIN_REGOLITH_FLAT:
            return base;

        case MOON_TERRAIN_REGOLITH_ROUGH: {
            if ((var % 6) == 0) return RGB565(36, 34, 40);   /* Boulder shadow */
            if ((var % 6) == 1) return RGB565(136, 134, 142); /* Boulder sunward edge */
            if (var >= 13) return moon_color_dim(base, 210);
            return base;
        }

        case MOON_TERRAIN_BASALT_MARE: {
            uint16_t dark_basalt = moon_color_dim(base, 140);
            /* Dark volcanic cooling cracks */
            if (((px ^ py) % 11) == 0 && var < 8) return RGB565(16, 17, 20);
            if (var == 3 || var == 4) return RGB565(62, 66, 74); /* Titanium reflection */
            return dark_basalt;
        }

        case MOON_TERRAIN_CRATER_RIM: {
            uint16_t rim_col = moon_color_lerp(base, COLOR_LUNAR_WHITE, FIXED_ONE / 4);
            /* Jagged ridge striations */
            if (((px - py) % 7) == 0) return RGB565(180, 180, 185);
            if (var >= 12) return RGB565(80, 80, 85); /* Ridge shadow crevice */
            return rim_col;
        }

        case MOON_TERRAIN_CRATER_FLOOR:
            return moon_color_dim(base, 70);

        case MOON_TERRAIN_ICE_DEPOSIT: {
            uint16_t ice_base = moon_color_lerp(base, RGB565(85, 170, 225), FIXED_ONE / 2);
            /* Dazzling specular ice crystal glints */
            if (var == 0 || var == 1) return COLOR_LUNAR_WHITE;
            if (var == 2) return COLOR_LASER_CYAN;
            return ice_base;
        }

        default:
            return base;
    }
}

static uint16_t sample_cliff_pixel(int gx, int gy, int rel_x, int dy, uint8_t terrain, int face_type) {
    /* World-space mapping into seamless 128x64 cliff strata texture */
    int base_coord = (face_type == 1) ? (gx << 5) : (gy << 5);
    uint32_t cu = (uint32_t)(base_coord + rel_x) & (TEX_CLIFF_W - 1);
    uint32_t cv = (uint32_t)dy & (TEX_CLIFF_H - 1);
    uint16_t col = g_tex_cliff[(cv << 7) | cu];

    /* Natural geological lighting: SW face shadowed, SE face illuminated */
    if (face_type == 1) {
        col = moon_color_dim(col, 175);
    } else {
        col = moon_color_dim(col, 235);
    }

    /* Strata and terrain tinting */
    switch (terrain) {
        case MOON_TERRAIN_CRATER_RIM:
            col = moon_color_lerp(col, COLOR_LUNAR_WHITE, FIXED_ONE / 5);
            break;
        case MOON_TERRAIN_BASALT_MARE:
            col = moon_color_dim(col, 150);
            break;
        case MOON_TERRAIN_ICE_DEPOSIT:
            col = moon_color_lerp(col, COLOR_LASER_CYAN, FIXED_ONE / 3);
            break;
        default:
            break;
    }

    return col;
}

void moon_blit_iso_tile_textured(const moon_surface_t *surf, int sx, int sy,
                                 int half_w, int half_h,
                                 int gx, int gy, uint8_t terrain,
                                 uint8_t flags, uint16_t border_color) {
    if (!surf || !surf->pixels) return;

    for (int y = -half_h; y <= half_h; y++) {
        int py = sy + y;
        if (py < 0 || py >= surf->height) continue;

        int span = (y <= 0) ? (half_h + y) : (half_h - y);
        int half_span_x = (half_w > 0) ? (span * half_w / half_h) : 0;

        int lx = sx - half_span_x;
        int rx = sx + half_span_x;
        if (lx < 0) lx = 0;
        if (rx >= surf->width) rx = surf->width - 1;

        if (lx <= rx) {
            uint16_t *row = surf->pixels + py * surf->stride;
            for (int x = lx; x <= rx; x++) {
                row[x] = sample_terrain_pixel(gx, gy, x, py, sx, sy, terrain, flags);
            }
        }
    }

    /* Outline edges */
    if (border_color != 0) {
        moon_blit_line(surf, sx, sy - half_h, sx + half_w, sy, border_color);
        moon_blit_line(surf, sx + half_w, sy, sx, sy + half_h, border_color);
        moon_blit_line(surf, sx, sy + half_h, sx - half_w, sy, border_color);
        moon_blit_line(surf, sx - half_w, sy, sx, sy - half_h, border_color);
    }
}

void moon_blit_iso_block_textured(const moon_surface_t *surf, int sx, int sy,
                                  int half_w, int half_h, int height_px,
                                  int gx, int gy, uint8_t terrain,
                                  uint8_t flags, uint16_t border_color) {
    if (!surf || !surf->pixels) return;

    int top_sy = sy - height_px;

    /* 1. Left Vertical Face with Geological Stratification */
    for (int x = sx - half_w; x <= sx; x++) {
        if (x < 0 || x >= surf->width) continue;

        int progress = x - (sx - half_w);
        int y_top = top_sy + (progress * half_h / half_w);
        int y_bot = y_top + height_px;

        if (y_top < 0) y_top = 0;
        if (y_bot >= surf->height) y_bot = surf->height - 1;

        for (int y = y_top; y <= y_bot; y++) {
            surf->pixels[y * surf->stride + x] = sample_cliff_pixel(gx, gy, x - sx, y - y_top, terrain, 1);
        }
    }

    /* 2. Right Vertical Face with Geological Stratification */
    for (int x = sx; x <= sx + half_w; x++) {
        if (x < 0 || x >= surf->width) continue;

        int progress = x - sx;
        int y_top = (top_sy + half_h) - (progress * half_h / half_w);
        int y_bot = y_top + height_px;

        if (y_top < 0) y_top = 0;
        if (y_bot >= surf->height) y_bot = surf->height - 1;

        for (int y = y_top; y <= y_bot; y++) {
            surf->pixels[y * surf->stride + x] = sample_cliff_pixel(gx, gy, x - sx, y - y_top, terrain, 2);
        }
    }

    /* 3. Top Face Diamond with Procedural Textures */
    moon_blit_iso_tile_textured(surf, sx, top_sy, half_w, half_h, gx, gy, terrain, flags, border_color);

    /* 4. Vertical Corner Outlines */
    if (border_color != 0) {
        moon_blit_line(surf, sx - half_w, top_sy, sx - half_w, sy, border_color);
        moon_blit_line(surf, sx, top_sy + half_h, sx, sy + half_h, border_color);
        moon_blit_line(surf, sx + half_w, top_sy, sx + half_w, sy, border_color);
        moon_blit_line(surf, sx - half_w, sy, sx, sy + half_h, border_color);
        moon_blit_line(surf, sx, sy + half_h, sx + half_w, sy, border_color);
    }
}

/*
 * Sprite Blitting with Color-Key Transparency
 */
void moon_blit_sprite(const moon_surface_t *dst, int dst_x, int dst_y,
                      const moon_surface_t *src, uint16_t key_color) {
    if (!dst || !src || !dst->pixels || !src->pixels) return;
    moon_blit_subsprite(dst, dst_x, dst_y, src, 0, 0, src->width, src->height, key_color);
}

void moon_blit_subsprite(const moon_surface_t *dst, int dst_x, int dst_y,
                         const moon_surface_t *sheet, int src_x, int src_y,
                         int w, int h, uint16_t key_color) {
    if (!dst || !sheet || !dst->pixels || !sheet->pixels) return;
    if (w <= 0 || h <= 0) return;

    /* Source clipping */
    if (src_x < 0) { w += src_x; dst_x -= src_x; src_x = 0; }
    if (src_y < 0) { h += src_y; dst_y -= src_y; src_y = 0; }
    if (src_x + w > sheet->width) w = sheet->width - src_x;
    if (src_y + h > sheet->height) h = sheet->height - src_y;

    /* Destination clipping */
    if (dst_x < 0) { int shift = -dst_x; w -= shift; src_x += shift; dst_x = 0; }
    if (dst_y < 0) { int shift = -dst_y; h -= shift; src_y += shift; dst_y = 0; }
    if (dst_x + w > dst->width) w = dst->width - dst_x;
    if (dst_y + h > dst->height) h = dst->height - dst_y;

    if (w <= 0 || h <= 0) return;

    for (int y = 0; y < h; y++) {
        const uint16_t *src_row = sheet->pixels + (src_y + y) * sheet->stride + src_x;
        uint16_t *dst_row = dst->pixels + (dst_y + y) * dst->stride + dst_x;

        for (int x = 0; x < w; x++) {
            uint16_t pixel = src_row[x];
            if (pixel != key_color) {
                dst_row[x] = pixel;
            }
        }
    }
}

/*
 * Bitmap Typography (8x16 Spleen ASCII)
 */
void moon_blit_char(const moon_surface_t *surf, int x, int y, char c,
                    uint16_t fg, uint16_t bg, bool transparent_bg) {
    if (!surf || !surf->pixels) return;
    if (c < 0x20 || c > 0x7E) c = '?';

    const uint8_t *glyph = g_moon_font8x16[(uint8_t)c - 0x20];

    for (int row = 0; row < MOON_FONT_H; row++) {
        int py = y + row;
        if (py < 0 || py >= surf->height) continue;

        uint8_t bits = glyph[row];
        uint16_t *dst_row = surf->pixels + py * surf->stride;

        for (int col = 0; col < MOON_FONT_W; col++) {
            int px = x + col;
            if (px < 0 || px >= surf->width) continue;

            bool on = (bits >> col) & 1;
            if (on) {
                dst_row[px] = fg;
            } else if (!transparent_bg) {
                dst_row[px] = bg;
            }
        }
    }
}

void moon_blit_text(const moon_surface_t *surf, int x, int y, const char *str,
                    uint16_t fg, uint16_t bg, bool transparent_bg) {
    if (!surf || !surf->pixels || !str) return;

    int cur_x = x;
    int cur_y = y;

    for (; *str; str++) {
        char c = *str;
        if (c == '\n') {
            cur_x = x;
            cur_y += MOON_FONT_H;
            continue;
        }
        moon_blit_char(surf, cur_x, cur_y, c, fg, bg, transparent_bg);
        cur_x += MOON_FONT_W;
    }
}

void moon_blit_text_fmt(const moon_surface_t *surf, int x, int y,
                        uint16_t fg, uint16_t bg, bool transparent_bg,
                        const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    moon_blit_text(surf, x, y, buf, fg, bg, transparent_bg);
}
