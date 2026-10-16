#!/usr/bin/env python3
"""gen_serif_font.py - Generates proportional serif font tables for LugalOS reader window.

Emits:
  - drivers/include/drivers/font_serif.h
  - drivers/font_serif.c

Generates:
  1. 1-bpp bilevel bitmaps (uint16_t rows, bit 0 = leftmost) for 1-bpp canvas.
  2. 4-bpp alpha coverage masks (16 levels: 0..15, 2 pixels/byte) for anti-aliased rendering.
  3. Proportional advance widths.
  4. Rendering functions for 1-bpp canvas, 8-bpp GRAY8, and 16-bpp RGB565.
"""

import sys
from pathlib import Path
from PIL import Image, ImageFont, ImageDraw

FONT_PATHS = [
    "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/noto/NotoSerif-Regular.ttf",
]
FIRST = 0x20
LAST = 0x7E
CELL_H = 16

def main():
    repo_root = Path(__file__).resolve().parent.parent
    font_path = None
    for p in FONT_PATHS:
        if Path(p).exists():
            font_path = p
            break
    if not font_path:
        sys.exit(f"No suitable serif font found in {FONT_PATHS}")
    font = ImageFont.truetype(font_path, 15)

    widths = []
    glyphs_1bpp = []
    glyphs_aa = []

    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        if code == 0x20:
            w = 4
            rows_1bpp = [0] * CELL_H
            rows_aa = [[0] * 8 for _ in range(CELL_H)]
        else:
            # Measure advance width
            w = int(round(font.getlength(ch)))
            if w < 1: w = 1
            if w > 16: w = 16

            # Render 1-bpp glyph
            img_1 = Image.new('1', (16, CELL_H), color=0)
            draw_1 = ImageDraw.Draw(img_1)
            draw_1.text((0, -2), ch, font=font, fill=1)

            rows_1bpp = []
            for y in range(CELL_H):
                row_val = 0
                for x in range(16):
                    if img_1.getpixel((x, y)):
                        row_val |= (1 << x)
                rows_1bpp.append(row_val)

            # Render 4-bpp anti-aliased glyph
            img_l = Image.new('L', (16, CELL_H), color=0)
            draw_l = ImageDraw.Draw(img_l)
            draw_l.text((0, -2), ch, font=font, fill=255)

            rows_aa = []
            for y in range(CELL_H):
                row_bytes = []
                for b in range(8):
                    p0 = img_l.getpixel((b * 2, y)) >> 4
                    p1 = img_l.getpixel((b * 2 + 1, y)) >> 4
                    row_bytes.append((p1 << 4) | p0)
                rows_aa.append(row_bytes)

        widths.append(w)
        glyphs_1bpp.append(rows_1bpp)
        glyphs_aa.append(rows_aa)

    # 1. Generate header
    hdr_path = repo_root / "drivers" / "include" / "drivers" / "font_serif.h"
    with open(hdr_path, "w") as f:
        f.write("""#ifndef LUGALOS_DRIVERS_FONT_SERIF_H
#define LUGALOS_DRIVERS_FONT_SERIF_H

#include <stdbool.h>
#include <stdint.h>

#define FONT_SERIF_FIRST 0x20
#define FONT_SERIF_LAST  0x7E
#define FONT_SERIF_COUNT (FONT_SERIF_LAST - FONT_SERIF_FIRST + 1)
#define FONT_SERIF_H     16

#include "drivers/canvas1.h"

extern const uint16_t font_serif_glyphs[FONT_SERIF_COUNT][FONT_SERIF_H];
extern const uint8_t  font_serif_aa_glyphs[FONT_SERIF_COUNT][FONT_SERIF_H][8];
extern const uint8_t  font_serif_widths[128];

static inline uint8_t font_serif_char_width(char c) {
    uint8_t uc = (uint8_t)c;
    return (uc < 128) ? font_serif_widths[uc] : 8;
}

static inline const uint16_t *font_serif_get_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_SERIF_FIRST || uc > FONT_SERIF_LAST) uc = '?';
    return font_serif_glyphs[uc - FONT_SERIF_FIRST];
}

static inline const uint8_t *font_serif_get_aa_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_SERIF_FIRST || uc > FONT_SERIF_LAST) uc = '?';
    return &font_serif_aa_glyphs[uc - FONT_SERIF_FIRST][0][0];
}

unsigned font_serif_text_width(const char *s, uint32_t len);

/* 1-bpp canvas rendering (classic bilevel) */
int canvas1_serif_glyph(const canvas1_t *c, int x, int y, char ch, bool bold);
int canvas1_serif_text(const canvas1_t *c, int x, int y, const char *s, uint32_t len, bool bold);

/* 8-bpp (GRAY8) anti-aliased rendering with integer alpha-blending */
int font_serif_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                              int x, int y, char ch, uint8_t fg, uint8_t bg, bool bold);
int font_serif_aa_text_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                             int x, int y, const char *s, uint32_t len,
                             uint8_t fg, uint8_t bg, bool bold);

/* 16-bpp (RGB565) anti-aliased rendering with integer alpha-blending */
int font_serif_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                               int x, int y, char ch, uint16_t fg, uint16_t bg, bool bold);
int font_serif_aa_text_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                              int x, int y, const char *s, uint32_t len,
                              uint16_t fg, uint16_t bg, bool bold);

#endif /* LUGALOS_DRIVERS_FONT_SERIF_H */
""")

    # 2. Generate C source
    c_path = repo_root / "drivers" / "font_serif.c"
    with open(c_path, "w") as f:
        f.write("""/* Generated by tools/gen_serif_font.py - do not edit.
 * Proportional Serif Font (LiberationSerif-Regular, 16-row cells).
 * 1-bpp: bit 0 = leftmost pixel (LugalOS 1-bpp canvas convention).
 * 4-bpp: low nibble = even pixel, high nibble = odd pixel (16 alpha levels).
 */

#include "drivers/font_serif.h"
#include "drivers/lcdterm_attr.h"

LCDTERM_URODATA const uint8_t font_serif_widths[128] = {
""")
        for c in range(128):
            if c < FIRST or c > LAST:
                w = 8 if c >= 32 else 0
            else:
                w = widths[c - FIRST]
            f.write(f" {w:2d},")
            if (c + 1) % 16 == 0:
                f.write("\n")
        f.write("};\n\n")

        # 1-bpp glyph table (in LCDTERM_URODATA for RP2350 U-mode domain)
        f.write(f"LCDTERM_URODATA const uint16_t font_serif_glyphs[{len(glyphs_1bpp)}][{CELL_H}] = {{\n")
        for i, rows in enumerate(glyphs_1bpp):
            ch = chr(FIRST + i)
            esc = repr(ch) if ch != '\\' and ch != "'" else f"'{ch}'"
            f.write(f"    /* 0x{FIRST + i:02X} {esc} (w={widths[i]}) */\n    {{ ")
            f.write(", ".join(f"0x{r:04x}" for r in rows))
            f.write(" },\n")
        f.write("};\n\n")

        # 4-bpp alpha coverage table (standard .rodata)
        f.write(f"const uint8_t font_serif_aa_glyphs[{len(glyphs_aa)}][{CELL_H}][8] = {{\n")
        for i, rows in enumerate(glyphs_aa):
            ch = chr(FIRST + i)
            esc = repr(ch) if ch != '\\' and ch != "'" else f"'{ch}'"
            f.write(f"    /* 0x{FIRST + i:02X} {esc} (w={widths[i]}) */\n    {{\n")
            for r_idx, r_bytes in enumerate(rows):
                f.write("        { " + ", ".join(f"0x{b:02x}" for b in r_bytes) + " },\n")
            f.write("    },\n")
        f.write("};\n\n")

        f.write("""LCDTERM_UTEXT unsigned font_serif_text_width(const char *s, uint32_t len) {
    if (!s) return 0;
    unsigned total = 0;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        total += font_serif_char_width(s[i]);
    }
    return total;
}

LCDTERM_UTEXT int canvas1_serif_glyph(const canvas1_t *c, int x, int y, char ch, bool bold) {
    uint8_t w = font_serif_char_width(ch);
    const uint16_t *g = font_serif_get_glyph(ch);
    int ax = x + (int)c->ox;
    int max_x = (int)c->stride * 8;
    bool inside = x >= 0 && x + 18 <= (int)c->w && ax >= 0 && ax + 18 <= max_x;
    for (unsigned r = 0; r < FONT_SERIF_H; r++) {
        int py = y + (int)r;
        if (py < 0 || py >= (int)c->h) continue;
        int spy = py + (int)c->oy;
        if (spy < 0) continue;
        uint32_t bits = g[r];
        if (bold) bits |= bits << 1;
        if (!bits) continue;
        uint8_t *p = c->fb + (uint32_t)spy * c->stride;
        if (inside) {
            uint32_t v = bits << (ax & 7);
            p[ax >> 3] |= (uint8_t)v;
            p[(ax >> 3) + 1] |= (uint8_t)(v >> 8);
            if (v >> 16) p[(ax >> 3) + 2] |= (uint8_t)(v >> 16);
            continue;
        }
        for (int b = 0; b < 16; b++) {
            int spx = ax + b;
            if ((bits >> b) & 1u && x + b >= 0 && x + b < (int)c->w && spx >= 0 && spx < max_x)
                p[spx >> 3] |= (uint8_t)(1u << (spx & 7));
        }
    }
    return (int)w;
}

LCDTERM_UTEXT int canvas1_serif_text(const canvas1_t *c, int x, int y, const char *s, uint32_t len, bool bold) {
    if (!s) return 0;
    int cur_x = x;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        cur_x += canvas1_serif_glyph(c, cur_x, y, s[i], bold);
    }
    return cur_x - x;
}

/* 8-bpp (GRAY8) anti-aliased glyph rendering with integer alpha-blending */
int font_serif_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                              int x, int y, char ch, uint8_t fg, uint8_t bg, bool bold) {
    uint8_t w = font_serif_char_width(ch);
    const uint8_t *g = font_serif_get_aa_glyph(ch);
    int delta = (int)fg - (int)bg;

    for (int r = 0; r < FONT_SERIF_H; r++) {
        int py = y + r;
        if (py < 0 || py >= clip_h) continue;
        uint8_t *dst_row = fb + (uint32_t)py * stride;
        const uint8_t *src_row = g + r * 8;

        uint8_t prev_a = 0;
        for (int c = 0; c < 16; c++) {
            int px = x + c;
            uint8_t a = (c & 1) ? (src_row[c >> 1] >> 4) : (src_row[c >> 1] & 0x0f);
            if (bold) {
                uint8_t cur = a;
                if (prev_a > a) a = prev_a;
                prev_a = cur;
            }
            if (!a) continue;
            if (px < 0 || px >= clip_w) continue;

            if (a == 15) {
                dst_row[px] = fg;
            } else {
                /* Integer alpha blend: bg + ((fg - bg) * a + 8) / 16 */
                dst_row[px] = (uint8_t)(bg + ((delta * (int)a + 8) >> 4));
            }
        }
    }
    return (int)w;
}

int font_serif_aa_text_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                             int x, int y, const char *s, uint32_t len,
                             uint8_t fg, uint8_t bg, bool bold) {
    if (!fb || !s) return 0;
    int cur_x = x;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        cur_x += font_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h, cur_x, y, s[i], fg, bg, bold);
    }
    return cur_x - x;
}

/* 16-bpp (RGB565) anti-aliased glyph rendering with integer alpha-blending */
int font_serif_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                               int x, int y, char ch, uint16_t fg, uint16_t bg, bool bold) {
    uint8_t w = font_serif_char_width(ch);
    const uint8_t *g = font_serif_get_aa_glyph(ch);

    int fg_r = (fg >> 11) & 0x1f;
    int fg_g = (fg >> 5) & 0x3f;
    int fg_b = fg & 0x1f;

    int bg_r = (bg >> 11) & 0x1f;
    int bg_g = (bg >> 5) & 0x3f;
    int bg_b = bg & 0x1f;

    int dr = fg_r - bg_r;
    int dg = fg_g - bg_g;
    int db = fg_b - bg_b;

    for (int r = 0; r < FONT_SERIF_H; r++) {
        int py = y + r;
        if (py < 0 || py >= clip_h) continue;
        uint16_t *dst_row = fb + (uint32_t)py * stride_pixels;
        const uint8_t *src_row = g + r * 8;

        uint8_t prev_a = 0;
        for (int c = 0; c < 16; c++) {
            int px = x + c;
            uint8_t a = (c & 1) ? (src_row[c >> 1] >> 4) : (src_row[c >> 1] & 0x0f);
            if (bold) {
                uint8_t cur = a;
                if (prev_a > a) a = prev_a;
                prev_a = cur;
            }
            if (!a) continue;
            if (px < 0 || px >= clip_w) continue;

            if (a == 15) {
                dst_row[px] = fg;
            } else {
                int r_val = bg_r + ((dr * (int)a + 8) >> 4);
                int g_val = bg_g + ((dg * (int)a + 8) >> 4);
                int b_val = bg_b + ((db * (int)a + 8) >> 4);
                dst_row[px] = (uint16_t)((r_val << 11) | (g_val << 5) | b_val);
            }
        }
    }
    return (int)w;
}

int font_serif_aa_text_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                              int x, int y, const char *s, uint32_t len,
                              uint16_t fg, uint16_t bg, bool bold) {
    if (!fb || !s) return 0;
    int cur_x = x;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        cur_x += font_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h, cur_x, y, s[i], fg, bg, bold);
    }
    return cur_x - x;
}
""")

    print(f"Generated {hdr_path} and {c_path} successfully.")

if __name__ == "__main__":
    main()
