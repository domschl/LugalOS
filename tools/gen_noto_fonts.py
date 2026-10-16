#!/usr/bin/env python3
"""gen_noto_fonts.py - Generates multi-density Noto Sans Mono and Noto Serif font tables for LugalOS.

Generates:
  1. drivers/include/drivers/font_noto_mono.h & drivers/font_noto_mono.c
     - 16x32 monospace font (bilevel 1-bpp uint16_t rows and 4-bpp anti-aliased coverage masks)
     - Full LugalOS internal glyph coverage (0x20..0xFF):
       - 0x20..0x7E: ASCII printable
       - 0x7F: Replacement glyph (inverted question mark)
       - 0x80..0x9F: Box drawing, 12 Staunton chess figurines, euro, quotes, bullets, shade blocks
       - 0xA0..0xFF: Latin-1
  2. drivers/include/drivers/font_noto_serif.h & drivers/font_noto_serif.c
     - 32-pixel proportional serif font for HiDPI reader
     - Proportional advance widths, 1-bpp bilevel uint32_t rows, and 4-bpp anti-aliased coverage masks
     - Fast integer blending routines for 8-bpp (GRAY8) and 16-bpp (RGB565)
"""

import sys
from pathlib import Path
from PIL import Image, ImageFont, ImageDraw

MONO_PATHS = [
    "/usr/share/fonts/noto/NotoSansMono-Medium.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Medium.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-CondensedMedium.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansMono-CondensedMedium.ttf",
]

SERIF_PATHS = [
    "/usr/share/fonts/TTF/GentiumBook-Regular.ttf",
    "/usr/share/fonts/gentium/GentiumBook-Regular.ttf",
    "/usr/share/fonts/noto/NotoSerif-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
]

FIRST = 0x20
LAST = 0xFF
REPLACEMENT = 0x7F
CELL_W = 16
CELL_H = 32

EXTRA = [0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
         0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
         0x2654, 0x2655, 0x2656, 0x2657, 0x2658, 0x2659,
         0x265A, 0x265B, 0x265C, 0x265D, 0x265E, 0x265F,
         0x20AC, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
         0x2022, 0x2588, 0x2592]

FIGURINES_WHITE = {
    "king":   ["..###..", "...#...", ".#.#.#.", "#.....#", "#..#..#", "#..#..#",
               ".#...#.", "..#.#..", ".#...#.", "#.....#", "#######"],
    "queen":  ["#..#..#", ".#.#.#.", "#..#..#", "#.....#", ".#...#.", "..#.#..",
               "..#.#..", ".#...#.", ".#...#.", "#.....#", "#######"],
    "rook":   ["#.#.#.#", "#.#.#.#", "#######", ".#...#.", ".#...#.", ".#...#.",
               ".#...#.", ".#...#.", ".#...#.", "#.....#", "#######"],
    "bishop": ["...#...", "..#.#..", "..#.#..", ".#...#.", ".#.#.#.", "..#.#..",
               "...#...", "..#.#..", ".#...#.", "#.....#", "#######"],
    "knight": ["..####.", ".#....#", "#.##..#", "#.#...#", ".#...#.", "...##..",
               "..#....", ".#.....", ".#...#.", "#.....#", "#######"],
    "pawn":   [".......", "..###..", ".#...#.", "..###..", "...#...", "..###..",
               "..#.#..", "..#.#..", ".#...#.", "#.....#", "#######"],
}

FIGURINES_BLACK = {
    "king":   ["..###..", "...#...", ".#####.", "#######", "##.#.##", "##.#.##",
               ".#####.", "..###..", ".#####.", "#######", "#######"],
    "queen":  ["#..#..#", ".#####.", "#######", "#######", ".#####.", "..###..",
               "..###..", ".#####.", ".#####.", "#######", "#######"],
    "rook":   ["#.#.#.#", "#.#.#.#", "#######", ".#####.", ".#####.", ".#####.",
               ".#####.", ".#####.", ".#####.", "#######", "#######"],
    "bishop": ["...#...", "..###..", "..###..", ".#####.", ".##.##.", "..###..",
               "...#...", "..###..", ".#####.", "#######", "#######"],
    "knight": ["..####.", ".######", "#.#####", "#.#####", ".######", "...####",
               "..#####", ".######", ".######", "#######", "#######"],
    "pawn":   [".......", "..###..", ".#####.", "..###..", "...#...", "..###..",
               ".#####.", ".#####.", ".#####.", "#######", "#######"],
}

FIGURINE_ORDER = ["king", "queen", "rook", "bishop", "knight", "pawn"]


def find_font(paths):
    for p in paths:
        if Path(p).exists():
            return p
    return None


def scale_figurine(art):
    """Scale 7x11 ASCII art by 2x horizontally and vertically into a 16x32 grid."""
    grid = []
    # 6 rows top padding
    for _ in range(6):
        grid.append("." * 16)
    for row in art:
        # 1 px margin left + 14 px doubled art + 1 px margin right = 16 px
        scaled_row = "." + "".join(c * 2 for c in row) + "."
        grid.append(scaled_row)
        grid.append(scaled_row)
    # 4 rows bottom padding
    for _ in range(4):
        grid.append("." * 16)
    assert len(grid) == 32
    assert all(len(r) == 16 for r in grid)
    return grid


def grid_to_rows_1bpp(grid):
    rows = []
    for y in range(CELL_H):
        val = 0
        for x in range(CELL_W):
            if grid[y][x] == "#":
                val |= (1 << x)
        rows.append(val)
    return rows


def grid_to_rows_aa(grid):
    rows = []
    for y in range(CELL_H):
        row_bytes = []
        for b in range(CELL_W // 2):
            p0 = 15 if grid[y][b * 2] == "#" else 0
            p1 = 15 if grid[y][b * 2 + 1] == "#" else 0
            row_bytes.append((p1 << 4) | p0)
        rows.append(row_bytes)
    return rows


def draw_box(ch_code):
    """Pixel-perfect 2-pixel-thick box drawing characters on 16x32 grid."""
    grid = [["." for _ in range(16)] for _ in range(32)]
    # Horizontal bar on rows 15..16
    # Vertical bar on cols 7..8
    h = (ch_code in (0x2500, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C))
    v = (ch_code in (0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C))

    left = ch_code in (0x2500, 0x2510, 0x2518, 0x2524, 0x252C, 0x2534, 0x253C)
    right = ch_code in (0x2500, 0x250C, 0x2514, 0x251C, 0x252C, 0x2534, 0x253C)
    top = ch_code in (0x2502, 0x2514, 0x2518, 0x251C, 0x2524, 0x2534, 0x253C)
    bottom = ch_code in (0x2502, 0x250C, 0x2510, 0x251C, 0x2524, 0x252C, 0x253C)

    if left:
        for x in range(0, 9):
            grid[15][x] = "#"
            grid[16][x] = "#"
    if right:
        for x in range(7, 16):
            grid[15][x] = "#"
            grid[16][x] = "#"
    if top:
        for y in range(0, 17):
            grid[y][7] = "#"
            grid[y][8] = "#"
    if bottom:
        for y in range(15, 32):
            grid[y][7] = "#"
            grid[y][8] = "#"

    return ["".join(r) for r in grid]


def generate_mono():
    mono_path = find_font(MONO_PATHS)
    if not mono_path:
        sys.exit(f"No Noto Sans Mono font found in {MONO_PATHS}")
    font = ImageFont.truetype(mono_path, 26)

    # 1. Prepare synthesized EXTRA glyphs
    extra_grids = {}
    # Box drawing
    for cp in [0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C, 0x2534, 0x253C]:
        extra_grids[cp] = draw_box(cp)

    # Chess figurines
    for i, name in enumerate(FIGURINE_ORDER):
        extra_grids[0x2654 + i] = scale_figurine(FIGURINES_WHITE[name])
        extra_grids[0x265A + i] = scale_figurine(FIGURINES_BLACK[name])

    # Blocks
    extra_grids[0x2588] = ["#" * 16 for _ in range(32)]
    shade = []
    for y in range(32):
        row = "".join("#." if (y % 2 == 0) else ".#" for _ in range(8))
        shade.append(row)
    extra_grids[0x2592] = shade

    # 2. Render all 224 glyphs
    glyphs_1bpp = []
    glyphs_aa = []
    qmark_1bpp = None

    for code in range(FIRST, LAST + 1):
        if code == 0x20:
            # Space
            g_1 = [0] * CELL_H
            g_aa = [[0] * 8 for _ in range(CELL_H)]
        elif code == REPLACEMENT:
            # Filled block with inverted '?'
            assert qmark_1bpp is not None
            g_1 = [r ^ 0xFFFF for r in qmark_1bpp]
            g_aa = []
            for y in range(CELL_H):
                row_bytes = []
                for b in range(8):
                    p0 = 0 if (g_1[y] & (1 << (b * 2))) == 0 else 15
                    p1 = 0 if (g_1[y] & (1 << (b * 2 + 1))) == 0 else 15
                    row_bytes.append((p1 << 4) | p0)
                g_aa.append(row_bytes)
        elif 0x80 <= code <= 0x9F:
            cp = EXTRA[code - 0x80]
            if cp in extra_grids:
                g_1 = grid_to_rows_1bpp(extra_grids[cp])
                g_aa = grid_to_rows_aa(extra_grids[cp])
            else:
                # Typographic characters from font (euro, quotes, bullet, ellipsis)
                img_l = Image.new('L', (16, CELL_H), color=0)
                draw_l = ImageDraw.Draw(img_l)
                draw_l.text((0, -4), chr(cp), font=font, fill=255)

                g_1 = []
                for y in range(CELL_H):
                    r_val = 0
                    for x in range(16):
                        if img_l.getpixel((x, y)) >= 96:
                            r_val |= (1 << x)
                    g_1.append(r_val)

                g_aa = []
                for y in range(CELL_H):
                    r_bytes = []
                    for b in range(8):
                        p0 = img_l.getpixel((b * 2, y)) >> 4
                        p1 = img_l.getpixel((b * 2 + 1, y)) >> 4
                        r_bytes.append((p1 << 4) | p0)
                    g_aa.append(r_bytes)
        else:
            # ASCII or Latin-1
            ch = chr(code)
            img_l = Image.new('L', (16, CELL_H), color=0)
            draw_l = ImageDraw.Draw(img_l)
            draw_l.text((0, -4), ch, font=font, fill=255)

            g_1 = []
            for y in range(CELL_H):
                r_val = 0
                for x in range(16):
                    if img_l.getpixel((x, y)) >= 96:
                        r_val |= (1 << x)
                g_1.append(r_val)

            g_aa = []
            for y in range(CELL_H):
                r_bytes = []
                for b in range(8):
                    p0 = img_l.getpixel((b * 2, y)) >> 4
                    p1 = img_l.getpixel((b * 2 + 1, y)) >> 4
                    r_bytes.append((p1 << 4) | p0)
                g_aa.append(r_bytes)

            if code == ord('?'):
                qmark_1bpp = list(g_1)

        glyphs_1bpp.append(g_1)
        glyphs_aa.append(g_aa)

    return glyphs_1bpp, glyphs_aa


def generate_serif():
    serif_path = find_font(SERIF_PATHS)
    if not serif_path:
        sys.exit(f"No Noto Serif font found in {SERIF_PATHS}")
    font = ImageFont.truetype(serif_path, 26)

    widths = []
    glyphs_1bpp = []
    glyphs_aa = []

    for code in range(FIRST, 0x7F):
        ch = chr(code)
        if code == 0x20:
            w = 8
            g_1 = [0] * CELL_H
            g_aa = [[0] * 16 for _ in range(CELL_H)]
        else:
            w = int(round(font.getlength(ch)))
            if w < 2: w = 2
            if w > 32: w = 32

            img_l = Image.new('L', (32, CELL_H), color=0)
            draw_l = ImageDraw.Draw(img_l)
            draw_l.text((0, -1), ch, font=font, fill=255)

            g_1 = []
            for y in range(CELL_H):
                r_val = 0
                for x in range(32):
                    if img_l.getpixel((x, y)) >= 96:
                        r_val |= (1 << x)
                g_1.append(r_val)

            g_aa = []
            for y in range(CELL_H):
                r_bytes = []
                for b in range(16):
                    p0 = img_l.getpixel((b * 2, y)) >> 4
                    p1 = img_l.getpixel((b * 2 + 1, y)) >> 4
                    r_bytes.append((p1 << 4) | p0)
                g_aa.append(r_bytes)

        widths.append(w)
        glyphs_1bpp.append(g_1)
        glyphs_aa.append(g_aa)

    return widths, glyphs_1bpp, glyphs_aa


def emit_mono_files(repo_root, glyphs_1bpp, glyphs_aa):
    hdr_path = repo_root / "drivers" / "include" / "drivers" / "font_noto_mono.h"
    with open(hdr_path, "w") as f:
        f.write("""#ifndef LUGALOS_DRIVERS_FONT_NOTO_MONO_H
#define LUGALOS_DRIVERS_FONT_NOTO_MONO_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/lcdterm_attr.h"

#define FONT_NOTO_MONO_FIRST        0x20
#define FONT_NOTO_MONO_LAST         0xff
#define FONT_NOTO_MONO_REPLACEMENT  0x7f
#define FONT_NOTO_MONO_COUNT        (FONT_NOTO_MONO_LAST - FONT_NOTO_MONO_FIRST + 1)
#define FONT_NOTO_MONO_W            16
#define FONT_NOTO_MONO_H            32

/* 1-bpp bilevel glyphs: uint16_t rows, bit 0 = leftmost pixel */
extern const uint16_t font_noto_mono_glyphs[FONT_NOTO_MONO_COUNT][FONT_NOTO_MONO_H];

/* 4-bpp anti-aliased coverage masks: 16 levels (0..15), 2 pixels/byte (8 bytes/row) */
extern const uint8_t font_noto_mono_aa_glyphs[FONT_NOTO_MONO_COUNT][FONT_NOTO_MONO_H][8];

/* The code points at 0x80..0x9F, sorted, with their glyph codes */
#define FONT_NOTO_MONO_MAP_COUNT 32
extern const uint16_t font_noto_mono_map_cp[FONT_NOTO_MONO_MAP_COUNT];
extern const uint8_t font_noto_mono_map_code[FONT_NOTO_MONO_MAP_COUNT];

static inline const uint16_t *font_noto_mono_get_glyph(uint8_t code) {
    if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
    return font_noto_mono_glyphs[code - FONT_NOTO_MONO_FIRST];
}

static inline const uint8_t *font_noto_mono_get_aa_glyph(uint8_t code) {
    if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
    return &font_noto_mono_aa_glyphs[code - FONT_NOTO_MONO_FIRST][0][0];
}

/* Fast integer blending onto 8-bpp (GRAY8) */
int font_noto_mono_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                  int x, int y, uint8_t code, uint8_t fg, uint8_t bg);

/* Fast integer blending onto 16-bpp (RGB565) */
int font_noto_mono_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                   int x, int y, uint8_t code, uint16_t fg, uint16_t bg);

#endif /* LUGALOS_DRIVERS_FONT_NOTO_MONO_H */
""")

    c_path = repo_root / "drivers" / "font_noto_mono.c"
    with open(c_path, "w") as f:
        f.write("""/* Generated by tools/gen_noto_fonts.py - do not edit.
 * Noto Sans Mono 16x32 font tables.
 * 1-bpp: bit 0 = leftmost pixel (uint16_t rows).
 * 4-bpp: low nibble = even pixel, high nibble = odd pixel (16 alpha levels).
 */

#include "drivers/font_noto_mono.h"
#include "drivers/lcdterm_attr.h"

""")
        # 1-bpp glyph table
        f.write(f"LCDTERM_URODATA const uint16_t font_noto_mono_glyphs[{len(glyphs_1bpp)}][{CELL_H}] = {{\n")
        for i, rows in enumerate(glyphs_1bpp):
            code = FIRST + i
            f.write(f"    /* 0x{code:02X} */\n    {{ ")
            f.write(", ".join(f"0x{r:04x}" for r in rows))
            f.write(" },\n")
        f.write("};\n\n")

        # 4-bpp coverage table
        f.write(f"const uint8_t font_noto_mono_aa_glyphs[{len(glyphs_aa)}][{CELL_H}][8] = {{\n")
        for i, rows in enumerate(glyphs_aa):
            code = FIRST + i
            f.write(f"    /* 0x{code:02X} */\n    {{\n")
            for r_bytes in rows:
                f.write("        { " + ", ".join(f"0x{b:02x}" for b in r_bytes) + " },\n")
            f.write("    },\n")
        f.write("};\n\n")

        # Code point map sorted
        pairs = sorted((cp, 0x80 + idx) for idx, cp in enumerate(EXTRA))
        f.write("LCDTERM_URODATA const uint16_t font_noto_mono_map_cp[FONT_NOTO_MONO_MAP_COUNT] = {\n    ")
        f.write(", ".join(f"0x{cp:04x}" for cp, _ in pairs))
        f.write("\n};\n\n")

        f.write("LCDTERM_URODATA const uint8_t font_noto_mono_map_code[FONT_NOTO_MONO_MAP_COUNT] = {\n    ")
        f.write(", ".join(f"0x{code:02x}" for _, code in pairs))
        f.write("\n};\n\n")

        # Blitting functions
        f.write("""int font_noto_mono_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                  int x, int y, uint8_t code, uint8_t fg, uint8_t bg) {
    if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
    const uint8_t *g = &font_noto_mono_aa_glyphs[code - FONT_NOTO_MONO_FIRST][0][0];

    for (int r = 0; r < FONT_NOTO_MONO_H; r++) {
        int py = y + r;
        if (py < 0 || py >= clip_h) continue;
        uint8_t *dst = fb + (uint32_t)py * stride;

        for (int c = 0; c < FONT_NOTO_MONO_W; c++) {
            int px = x + c;
            if (px < 0 || px >= clip_w) continue;

            uint8_t pair = g[(r * 8) + (c >> 1)];
            uint8_t a = (c & 1) ? (pair >> 4) : (pair & 0x0Fu);
            if (a == 0) {
                dst[px] = bg;
            } else if (a == 15) {
                dst[px] = fg;
            } else {
                uint32_t mixed = ((uint32_t)fg * a + (uint32_t)bg * (15u - a) + 7u) / 15u;
                dst[px] = (uint8_t)mixed;
            }
        }
    }
    return FONT_NOTO_MONO_W;
}

int font_noto_mono_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                   int x, int y, uint8_t code, uint16_t fg, uint16_t bg) {
    if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
    const uint8_t *g = &font_noto_mono_aa_glyphs[code - FONT_NOTO_MONO_FIRST][0][0];

    uint32_t fg_r = (fg >> 11) & 0x1Fu;
    uint32_t fg_g = (fg >> 5)  & 0x3Fu;
    uint32_t fg_b =  fg        & 0x1Fu;

    uint32_t bg_r = (bg >> 11) & 0x1Fu;
    uint32_t bg_g = (bg >> 5)  & 0x3Fu;
    uint32_t bg_b =  bg        & 0x1Fu;

    for (int r = 0; r < FONT_NOTO_MONO_H; r++) {
        int py = y + r;
        if (py < 0 || py >= clip_h) continue;
        uint16_t *dst = fb + (uint32_t)py * stride_pixels;

        for (int c = 0; c < FONT_NOTO_MONO_W; c++) {
            int px = x + c;
            if (px < 0 || px >= clip_w) continue;

            uint8_t pair = g[(r * 8) + (c >> 1)];
            uint8_t a = (c & 1) ? (pair >> 4) : (pair & 0x0Fu);
            if (a == 0) {
                dst[px] = bg;
            } else if (a == 15) {
                dst[px] = fg;
            } else {
                uint32_t inv = 15u - a;
                uint32_t out_r = (fg_r * a + bg_r * inv + 7u) / 15u;
                uint32_t out_g = (fg_g * a + bg_g * inv + 7u) / 15u;
                uint32_t out_b = (fg_b * a + bg_b * inv + 7u) / 15u;
                dst[px] = (uint16_t)((out_r << 11) | (out_g << 5) | out_b);
            }
        }
    }
    return FONT_NOTO_MONO_W;
}
""")


def emit_serif_files(repo_root, widths, glyphs_1bpp, glyphs_aa):
    hdr_path = repo_root / "drivers" / "include" / "drivers" / "font_noto_serif.h"
    with open(hdr_path, "w") as f:
        f.write("""#ifndef LUGALOS_DRIVERS_FONT_NOTO_SERIF_H
#define LUGALOS_DRIVERS_FONT_NOTO_SERIF_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/lcdterm_attr.h"
#include "drivers/canvas1.h"

#define FONT_NOTO_SERIF_FIRST  0x20
#define FONT_NOTO_SERIF_LAST   0x7e
#define FONT_NOTO_SERIF_COUNT  (FONT_NOTO_SERIF_LAST - FONT_NOTO_SERIF_FIRST + 1)
#define FONT_NOTO_SERIF_H      32
#define FONT_NOTO_SERIF_MAX_W  32

extern const uint8_t  font_noto_serif_widths[128];
extern const uint8_t  font_noto_serif_aa_glyphs[FONT_NOTO_SERIF_COUNT][FONT_NOTO_SERIF_H][16];
extern const uint32_t font_noto_serif_glyphs[FONT_NOTO_SERIF_COUNT][FONT_NOTO_SERIF_H];

static inline uint8_t font_noto_serif_char_width(char c) {
    uint8_t uc = (uint8_t)c;
    return (uc < 128) ? font_noto_serif_widths[uc] : 16;
}

static inline const uint8_t *font_noto_serif_get_aa_glyph(char c) {
    uint8_t uc = (uint8_t)c;
    if (uc < FONT_NOTO_SERIF_FIRST || uc > FONT_NOTO_SERIF_LAST) uc = '?';
    return &font_noto_serif_aa_glyphs[uc - FONT_NOTO_SERIF_FIRST][0][0];
}

unsigned font_noto_serif_text_width(const char *s, uint32_t len);

/* 1-bpp canvas rendering (classic bilevel) */
int canvas1_noto_serif_glyph(const canvas1_t *c, int x, int y, char ch, bool bold);
int canvas1_noto_serif_text(const canvas1_t *c, int x, int y, const char *s, uint32_t len, bool bold);

/* Fast integer blending onto 8-bpp (GRAY8) */
int font_noto_serif_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                   int x, int y, char ch, uint8_t fg, uint8_t bg, bool bold);
int font_noto_serif_aa_text_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                  int x, int y, const char *s, uint32_t len,
                                  uint8_t fg, uint8_t bg, bool bold);

/* Fast integer blending onto 16-bpp (RGB565) */
int font_noto_serif_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                    int x, int y, char ch, uint16_t fg, uint16_t bg, bool bold);
int font_noto_serif_aa_text_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                   int x, int y, const char *s, uint32_t len,
                                   uint16_t fg, uint16_t bg, bool bold);

#endif /* LUGALOS_DRIVERS_FONT_NOTO_SERIF_H */
""")

    c_path = repo_root / "drivers" / "font_noto_serif.c"
    with open(c_path, "w") as f:
        f.write("""/* Generated by tools/gen_noto_fonts.py - do not edit.
 * Proportional Noto Serif font (32-row cells).
 * 1-bpp: bit 0 = leftmost pixel (uint32_t rows).
 * 4-bpp: low nibble = even pixel, high nibble = odd pixel (16 alpha levels).
 */

#include "drivers/font_noto_serif.h"
#include "drivers/lcdterm_attr.h"

LCDTERM_URODATA const uint8_t font_noto_serif_widths[128] = {
""")
        for c in range(128):
            if c < FIRST or c > 0x7E:
                w = 16 if c >= 32 else 0
            else:
                w = widths[c - FIRST]
            f.write(f" {w:2d},")
            if (c + 1) % 16 == 0:
                f.write("\n")
        f.write("};\n\n")

        # 1-bpp glyph table
        f.write(f"LCDTERM_URODATA const uint32_t font_noto_serif_glyphs[{len(glyphs_1bpp)}][{CELL_H}] = {{\n")
        for i, rows in enumerate(glyphs_1bpp):
            ch = chr(FIRST + i)
            esc = repr(ch) if ch != '\\' and ch != "'" else f"'{ch}'"
            f.write(f"    /* 0x{FIRST + i:02X} {esc} (w={widths[i]}) */\n    {{ ")
            f.write(", ".join(f"0x{r:08x}" for r in rows))
            f.write(" },\n")
        f.write("};\n\n")

        # 4-bpp coverage table
        f.write(f"const uint8_t font_noto_serif_aa_glyphs[{len(glyphs_aa)}][{CELL_H}][16] = {{\n")
        for i, rows in enumerate(glyphs_aa):
            ch = chr(FIRST + i)
            esc = repr(ch) if ch != '\\' and ch != "'" else f"'{ch}'"
            f.write(f"    /* 0x{FIRST + i:02X} {esc} (w={widths[i]}) */\n    {{\n")
            for r_bytes in rows:
                f.write("        { " + ", ".join(f"0x{b:02x}" for b in r_bytes) + " },\n")
            f.write("    },\n")
        f.write("};\n\n")

        f.write("""unsigned font_noto_serif_text_width(const char *s, uint32_t len) {
    if (!s) return 0;
    unsigned total = 0;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        total += font_noto_serif_char_width(s[i]);
    }
    return total;
}

int font_noto_serif_aa_glyph_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                   int x, int y, char ch, uint8_t fg, uint8_t bg, bool bold) {
    (void)bg;
    uint8_t w = font_noto_serif_char_width(ch);
    const uint8_t *g = font_noto_serif_get_aa_glyph(ch);

    for (int r = 0; r < FONT_NOTO_SERIF_H; r++) {
        int py = y + r;
        if (py < 0 || py >= clip_h) continue;
        uint8_t *dst = fb + (uint32_t)py * stride;

        for (int c = 0; c < (int)w; c++) {
            int px = x + c;
            if (px < 0 || px >= clip_w) continue;

            uint8_t pair = g[(r * 16) + (c >> 1)];
            uint8_t a = (c & 1) ? (pair >> 4) : (pair & 0x0Fu);

            if (bold && c + 1 < (int)w) {
                uint8_t next_pair = g[(r * 16) + ((c + 1) >> 1)];
                uint8_t next_a = ((c + 1) & 1) ? (next_pair >> 4) : (next_pair & 0x0Fu);
                if (next_a > a) a = next_a;
            }

            if (a == 0) continue;
            if (a == 15) {
                dst[px] = fg;
            } else {
                uint8_t cur = dst[px];
                uint32_t mixed = ((uint32_t)fg * a + (uint32_t)cur * (15u - a) + 7u) / 15u;
                dst[px] = (uint8_t)mixed;
            }
        }
    }
    return (int)w;
}

int font_noto_serif_aa_text_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                  int x, int y, const char *s, uint32_t len,
                                  uint8_t fg, uint8_t bg, bool bold) {
    if (!fb || !s) return 0;
    int cur_x = x;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        cur_x += font_noto_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h, cur_x, y, s[i], fg, bg, bold);
    }
    return cur_x - x;
}

int font_noto_serif_aa_glyph_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                    int x, int y, char ch, uint16_t fg, uint16_t bg, bool bold) {
    (void)bg;
    uint8_t w = font_noto_serif_char_width(ch);
    const uint8_t *g = font_noto_serif_get_aa_glyph(ch);

    uint32_t fg_r = (fg >> 11) & 0x1Fu;
    uint32_t fg_g = (fg >> 5)  & 0x3Fu;
    uint32_t fg_b =  fg        & 0x1Fu;

    for (int r = 0; r < FONT_NOTO_SERIF_H; r++) {
        int py = y + r;
        if (py < 0 || py >= clip_h) continue;
        uint16_t *dst = fb + (uint32_t)py * stride_pixels;

        for (int c = 0; c < (int)w; c++) {
            int px = x + c;
            if (px < 0 || px >= clip_w) continue;

            uint8_t pair = g[(r * 16) + (c >> 1)];
            uint8_t a = (c & 1) ? (pair >> 4) : (pair & 0x0Fu);

            if (bold && c + 1 < (int)w) {
                uint8_t next_pair = g[(r * 16) + ((c + 1) >> 1)];
                uint8_t next_a = ((c + 1) & 1) ? (next_pair >> 4) : (next_pair & 0x0Fu);
                if (next_a > a) a = next_a;
            }

            if (a == 0) continue;
            if (a == 15) {
                dst[px] = fg;
            } else {
                uint16_t cur = dst[px];
                uint32_t bg_r = (cur >> 11) & 0x1Fu;
                uint32_t bg_g = (cur >> 5)  & 0x3Fu;
                uint32_t bg_b =  cur        & 0x1Fu;

                uint32_t inv = 15u - a;
                uint32_t out_r = (fg_r * a + bg_r * inv + 7u) / 15u;
                uint32_t out_g = (fg_g * a + bg_g * inv + 7u) / 15u;
                uint32_t out_b = (fg_b * a + bg_b * inv + 7u) / 15u;
                dst[px] = (uint16_t)((out_r << 11) | (out_g << 5) | out_b);
            }
        }
    }
    return (int)w;
}

int font_noto_serif_aa_text_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                   int x, int y, const char *s, uint32_t len,
                                   uint16_t fg, uint16_t bg, bool bold) {
    if (!fb || !s) return 0;
    int cur_x = x;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        cur_x += font_noto_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h, cur_x, y, s[i], fg, bg, bold);
    }
    return cur_x - x;
}

LCDTERM_UTEXT int canvas1_noto_serif_glyph(const canvas1_t *c, int x, int y, char ch, bool bold) {
    if (!c || !c->fb) return 0;
    uint8_t uc = (uint8_t)ch;
    if (uc < FONT_NOTO_SERIF_FIRST || uc > FONT_NOTO_SERIF_LAST) uc = '?';
    uint8_t w = font_noto_serif_char_width((char)uc);
    const uint32_t *g = font_noto_serif_glyphs[uc - FONT_NOTO_SERIF_FIRST];

    int ax = x + (int)c->ox;
    int max_x = (int)c->stride * 8;
    bool inside = (x >= 0 && x + (int)w <= (int)c->w && ax >= 0 && ax + 32 <= max_x);

    for (int r = 0; r < FONT_NOTO_SERIF_H; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)c->h) continue;
        int spy = py + (int)c->oy;
        if (spy < 0) continue;
        uint32_t bits = g[r];
        if (bold) bits |= bits << 1;
        if (!bits) continue;
        uint8_t *p = c->fb + (uint32_t)spy * c->stride;
        if (inside) {
            uint64_t v = (uint64_t)bits << (ax & 7);
            int byte_idx = ax >> 3;
            p[byte_idx] |= (uint8_t)v;
            p[byte_idx + 1] |= (uint8_t)(v >> 8);
            p[byte_idx + 2] |= (uint8_t)(v >> 16);
            p[byte_idx + 3] |= (uint8_t)(v >> 24);
            if (v >> 32) p[byte_idx + 4] |= (uint8_t)(v >> 32);
            continue;
        }
        for (int b = 0; b < (int)w; b++) {
            int spx = ax + b;
            if (((bits >> b) & 1u) && x + b >= 0 && x + b < (int)c->w && spx >= 0 && spx < max_x) {
                p[spx >> 3] |= (uint8_t)(1u << (spx & 7));
            }
        }
    }
    return (int)w;
}

LCDTERM_UTEXT int canvas1_noto_serif_text(const canvas1_t *c, int x, int y, const char *s, uint32_t len, bool bold) {
    if (!s) return 0;
    int cur_x = x;
    for (uint32_t i = 0; i < len && s[i]; i++) {
        cur_x += canvas1_noto_serif_glyph(c, cur_x, y, s[i], bold);
    }
    return cur_x - x;
}
""")


def main():
    repo_root = Path(__file__).resolve().parent.parent
    print("Generating Noto Sans Mono 16x32...")
    mono_1bpp, mono_aa = generate_mono()
    emit_mono_files(repo_root, mono_1bpp, mono_aa)
    print(f"Emitted drivers/font_noto_mono.c ({len(mono_1bpp)} glyphs)")

    print("Generating Noto Serif 32px...")
    serif_widths, serif_1bpp, serif_aa = generate_serif()
    emit_serif_files(repo_root, serif_widths, serif_1bpp, serif_aa)
    print(f"Emitted drivers/font_noto_serif.c ({len(serif_1bpp)} glyphs)")
    print("Done!")


if __name__ == "__main__":
    main()
