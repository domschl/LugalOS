#!/usr/bin/env python3
"""gen_font_bdf.py - a fixed-cell BDF font as a C table for the 1-bpp screen.

    python3 tools/gen_font_bdf.py tools/fonts/spleen-8x16.bdf drivers/font8x16.c

36.5, plan/phase36_rp2350_lcd7_terminal.md §4.2; widened by 37.1,
plan/phase37_screen_layouts_and_apps.md §3.1. Emits `font8x16_glyphs[224][16]`,
one byte per glyph row, indexed by the terminal's **internal glyph code**
minus 0x20 -- the byte the cell shadow stores for each cell:

    0x20..0x7E   printable ASCII
    0x7F         the replacement glyph (a reversed '?'): anything the font
                 cannot draw, and malformed UTF-8
    0x80..0x9F   EXTRA below: box drawing, the chess figurines, the euro,
                 typographic quotes and a few block characters
    0xA0..0xFF   Latin-1, at the same numbers as their code points

and `font8x16_map_cp[]` / `font8x16_map_code[]`, the EXTRA code points
sorted with their codes, for the bisection in drivers/fbtext.c.

**The bit order is decided here, once, and nowhere else.** The RP2350-LCD-7's
pixel state machine shifts its framebuffer words out to the right, so the
leftmost pixel of a byte is its *least* significant bit (drivers/lcd7.h). BDF
stores the leftmost pixel in the *most* significant bit. So every row is
bit-reversed on the way out, and a glyph row can then be stored into the
framebuffer as a single byte with no work at runtime.

The font's own copyright notice and licence are carried into the generated
file's header, as the BSD licence requires of a source redistribution.
"""

import os
import sys

FIRST, LAST = 0x20, 0x7E
CELL_W, CELL_H = 8, 16
LATIN1 = range(0xA0, 0x100)
REPLACEMENT = 0x7F

# Beyond ASCII and Latin-1, only what this tree prints, in code order from
# 0x80: the single-line box set (the `e` editor's frame uses two of them, and
# a frame drawn with half the set looks broken), the twelve chess figurines,
# the euro, the four typographic quotes and the ellipsis, a bullet, and the
# full and medium shade blocks. Exactly 32, which is what 0x80..0x9F holds.
EXTRA = [0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518,
         0x251C, 0x2524, 0x252C, 0x2534, 0x253C,
         0x2654, 0x2655, 0x2656, 0x2657, 0x2658, 0x2659,
         0x265A, 0x265B, 0x265C, 0x265D, 0x265E, 0x265F,
         0x20AC, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026,
         0x2022, 0x2588, 0x2592]
assert len(EXTRA) == 32

# Spleen has no chess figurines, so they are drawn here, in BDF orientation
# (leftmost pixel first) on Spleen's capital-letter grid: glyph rows 1..11,
# columns 0..6, the base on the baseline (row 11). These are the black
# pieces; a white piece is its black one with the interior cleared (every
# pixel whose four neighbours are all set), so the thin parts stay solid and
# the bodies open up, the usual look of chess figurines at text size.
FIGURINES = {
    "king":   ["...#...", "..###..", "...#...", ".#####.", "#######", "#######",
               ".#####.", "..###..", ".#####.", "#######", "#######"],
    "queen":  ["#..#..#", "#.###.#", "#######", "#######", ".#####.", "..###..",
               "..###..", "..###..", ".#####.", "#######", "#######"],
    "rook":   ["#.#.#.#", "#######", "#######", ".#####.", "..###..", "..###..",
               "..###..", "..###..", ".#####.", "#######", "#######"],
    "bishop": ["...#...", "..###..", ".##.##.", ".#####.", ".#####.", "..###..",
               "...#...", "..###..", ".#####.", "#######", "#######"],
    "knight": ["..##.#.", ".######", "###.###", "#######", "##..###", "...####",
               "..#####", ".######", ".######", "#######", "#######"],
    "pawn":   [".......", ".......", "..###..", "..###..", "...#...", "..###..",
               "..###..", ".#####.", ".#####.", "#######", "#######"],
}
FIGURINE_ORDER = ["king", "queen", "rook", "bishop", "knight", "pawn"]


def figurine(name: str, white: bool) -> list[int]:
    art = FIGURINES[name]
    grid = [[c == "#" for c in row.ljust(CELL_W, ".")] for row in art]
    if white:
        def on(r: int, c: int) -> bool:
            return 0 <= r < len(grid) and 0 <= c < CELL_W and grid[r][c]
        grid = [[grid[r][c] and not (on(r - 1, c) and on(r + 1, c) and on(r, c - 1) and on(r, c + 1))
                 for c in range(CELL_W)] for r in range(len(grid))]
    rows = [0] * CELL_H
    for i, row in enumerate(grid):
        rows[1 + i] = sum(1 << (CELL_W - 1 - c) for c in range(CELL_W) if row[c])
    return rows


def drawn_glyphs() -> dict[int, list[int]]:
    out = {}
    for i, name in enumerate(FIGURINE_ORDER):
        out[0x2654 + i] = figurine(name, white=True)
        out[0x265A + i] = figurine(name, white=False)
    return out


def reverse8(b: int) -> int:
    return int(f"{b:08b}"[::-1], 2)


def parse(path: str) -> tuple[list[str], dict[int, list[int]]]:
    notice: list[str] = []
    glyphs: dict[int, list[int]] = {}
    enc = None
    bbx = None
    rows: list[int] | None = None
    ascent = descent = None
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("COMMENT"):
                notice.append(line[len("COMMENT"):].strip())
            elif line.startswith("FONT_ASCENT"):
                ascent = int(line.split()[1])
            elif line.startswith("FONT_DESCENT"):
                descent = int(line.split()[1])
            elif line.startswith("ENCODING"):
                enc = int(line.split()[1])
            elif line.startswith("BBX"):
                bbx = tuple(int(v) for v in line.split()[1:])
            elif line == "BITMAP":
                rows = []
            elif line == "ENDCHAR":
                if enc is not None and (FIRST <= enc <= LAST or enc in LATIN1 or enc in EXTRA):
                    if bbx != (CELL_W, CELL_H, 0, -(descent or 0)):
                        sys.exit(f"glyph {enc:#x}: BBX {bbx} is not the full {CELL_W}x{CELL_H} cell; "
                                 "this generator only handles fixed-cell fonts")
                    glyphs[enc] = rows or []
                enc, bbx, rows = None, None, None
            elif rows is not None:
                rows.append(int(line, 16))
    if ascent is None or descent is None or ascent + descent != CELL_H:
        sys.exit(f"FONT_ASCENT + FONT_DESCENT must be {CELL_H}")
    glyphs.update(drawn_glyphs())
    missing = [c for c in list(range(FIRST, LAST + 1)) + list(LATIN1) + EXTRA if c not in glyphs]
    if missing:
        sys.exit(f"missing glyphs: {', '.join(hex(c) for c in missing)}")
    return notice, glyphs


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    notice, glyphs = parse(src)
    with open(out, "w", encoding="ascii") as f:
        f.write("/* Generated by tools/gen_font_bdf.py -- do not edit.\n")
        f.write(f" *\n *   python3 tools/gen_font_bdf.py {src} {out}\n *\n")
        f.write(" * The terminal's internal glyph codes 0x20..0xFF (see the generator):\n")
        f.write(" * ASCII, a replacement glyph at 0x7F, box drawing, chess figurines and\n")
        f.write(" * typography at 0x80..0x9F, and Latin-1 at its own numbers. 8x16 cells,\n")
        f.write(" * one byte per row, **leftmost pixel in bit 0** (bit-reversed from BDF;\n")
        f.write(" * see the generator for why). The chess figurines are drawn in the\n")
        f.write(" * generator, not taken from Spleen, which has none.\n")
        f.write(" *\n * The font's own notice, from the BDF, followed by its licence (BSD\n")
        f.write(" * 2-Clause, tools/fonts/LICENSE.spleen):\n *\n")
        for line in notice:
            if line in ("/*", "*/"):
                continue
            text = line.lstrip("*").strip()
            f.write(f" *   {text}\n" if text else " *\n")
        # The licence's full text, not only the notice: a BSD source
        # redistribution must retain the conditions and the disclaimer too.
        lic = os.path.join(os.path.dirname(src), "LICENSE.spleen")
        if os.path.exists(lic):
            f.write(" *\n *   ---- tools/fonts/LICENSE.spleen ----\n")
            with open(lic, encoding="utf-8") as lf:
                for text in lf.read().splitlines():
                    f.write(f" *   {text}".rstrip() + "\n")
        f.write(" */\n\n#include \"drivers/font8x16.h\"\n\n")
        qmark = glyphs[ord("?")]
        by_code = {c: glyphs[c] for c in range(FIRST, LAST + 1)}
        by_code[REPLACEMENT] = [r ^ 0xFF for r in qmark]
        for i, cp in enumerate(EXTRA):
            by_code[0x80 + i] = glyphs[cp]
        for cp in LATIN1:
            by_code[cp] = glyphs[cp]
        f.write(f"LCDTERM_URODATA const uint8_t font8x16_glyphs[{0x100 - FIRST}][{CELL_H}] = {{\n")
        for code in range(FIRST, 0x100):
            if code <= LAST:
                ch = chr(code)
                label = "'" + ("\\" + ch if ch in "\\'" else ch) + "'"
            elif code == REPLACEMENT:
                label = "replacement"
            elif code < 0xA0:
                label = f"U+{EXTRA[code - 0x80]:04X}"
            else:
                label = f"U+{code:04X}"
            data = ", ".join(f"0x{reverse8(r):02x}" for r in by_code[code])
            f.write(f"    /* 0x{code:02x} {label} */ {{ {data} }},\n")
        f.write("};\n\n")
        pairs = sorted((cp, 0x80 + i) for i, cp in enumerate(EXTRA))
        f.write("/* The code points at 0x80..0x9F, sorted, and their glyph codes. */\n")
        f.write(f"LCDTERM_URODATA const uint16_t font8x16_map_cp[{len(pairs)}] = {{\n   ")
        f.write(",".join(f" 0x{cp:04x}" for cp, _ in pairs) + "\n};\n")
        f.write(f"LCDTERM_URODATA const uint8_t font8x16_map_code[{len(pairs)}] = {{\n   ")
        f.write(",".join(f" 0x{code:02x}" for _, code in pairs) + "\n};\n")

if __name__ == "__main__":
    main()
