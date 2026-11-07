#!/usr/bin/env python3
"""draw_icons.py - draws LugalOS's first icon set into assets/icons/*.png.

Classic Macintosh style: black outlines, white faces, a few flat grays, no
anti-aliasing at the design size. Each icon is drawn on a 64 x 64 grid with
2-pixel strokes on even coordinates, so tools/gen_icons.py's 32 x 32 version
(2 x 2 averages) stays crisp.

The PNGs are the sources: any of them may be replaced by drawn art (64 x 64,
gray with alpha), and gen_icons.py turns whatever is in assets/icons/ into
the built-in tables. This script only makes the first set. Standard library
only.

Usage: python3 tools/draw_icons.py [--out assets/icons]
"""

from __future__ import annotations

import argparse
import struct
import zlib
from collections.abc import Callable
from pathlib import Path

N = 64
INK, DARK, MID, LIGHT, PALE, WHITE = 0, 70, 130, 185, 215, 255

Pixel = int | None              # a gray level, or None: transparent


class Canvas:
    def __init__(self) -> None:
        self.px: list[list[Pixel]] = [[None] * N for _ in range(N)]

    def set(self, x: int, y: int, v: int) -> None:
        if 0 <= x < N and 0 <= y < N:
            self.px[y][x] = v

    def rect(self, x0: int, y0: int, x1: int, y1: int, v: int) -> None:
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, v)

    def frame(self, x0: int, y0: int, x1: int, y1: int, v: int = INK, t: int = 2) -> None:
        self.rect(x0, y0, x1, y0 + t - 1, v)
        self.rect(x0, y1 - t + 1, x1, y1, v)
        self.rect(x0, y0, x0 + t - 1, y1, v)
        self.rect(x1 - t + 1, y0, x1, y1, v)

    def box(self, x0: int, y0: int, x1: int, y1: int, fill: int = WHITE, t: int = 2) -> None:
        self.rect(x0, y0, x1, y1, fill)
        self.frame(x0, y0, x1, y1, INK, t)

    def line(self, x0: int, y0: int, x1: int, y1: int, v: int = INK, t: int = 2) -> None:
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.rect(x0, y0, x0 + t - 1, y0 + t - 1, v)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def inside_poly(self, pts: list[tuple[float, float]], x: float, y: float) -> bool:
        c = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                c = not c
            j = i
        return c

    def poly(self, pts: list[tuple[float, float]], fill: int | None = WHITE, outline: int | None = INK,
             t: float = 2.0) -> None:
        """Filled at pixel centres; the outline is the band within t of an edge, inside."""
        for y in range(N):
            for x in range(N):
                cx, cy = x + 0.5, y + 0.5
                if not self.inside_poly(pts, cx, cy):
                    continue
                if outline is not None and self.edge_dist(pts, cx, cy) < t:
                    self.set(x, y, outline)
                elif fill is not None:
                    self.set(x, y, fill)

    @staticmethod
    def edge_dist(pts: list[tuple[float, float]], x: float, y: float) -> float:
        best = 1e9
        for i in range(len(pts)):
            ax, ay = pts[i]
            bx, by = pts[(i + 1) % len(pts)]
            vx, vy = bx - ax, by - ay
            ll = vx * vx + vy * vy
            u = 0.0 if ll == 0 else max(0.0, min(1.0, ((x - ax) * vx + (y - ay) * vy) / ll))
            px, py = ax + u * vx - x, ay + u * vy - y
            best = min(best, (px * px + py * py) ** 0.5)
        return best

    def ellipse(self, cx: float, cy: float, rx: float, ry: float, fill: int | None = WHITE,
                outline: int | None = INK, t: float = 2.0) -> None:
        for y in range(N):
            for x in range(N):
                dx, dy = (x + 0.5 - cx) / rx, (y + 0.5 - cy) / ry
                if dx * dx + dy * dy > 1.0:
                    continue
                ix, iy = (x + 0.5 - cx) / max(rx - t, 0.1), (y + 0.5 - cy) / max(ry - t, 0.1)
                if outline is not None and ix * ix + iy * iy > 1.0:
                    self.set(x, y, outline)
                elif fill is not None:
                    self.set(x, y, fill)


# --- the icons -------------------------------------------------------------

def page(c: Canvas, x0: int = 12, y0: int = 4, x1: int = 51, y1: int = 59) -> None:
    """A document: a white page with its top-right corner folded."""
    fold = 12
    c.poly([(x0, y0), (x1 - fold, y0), (x1 + 1, y0 + fold + 1), (x1 + 1, y1 + 1), (x0, y1 + 1)])
    c.poly([(x1 - fold, y0), (x1 - fold, y0 + fold + 1), (x1 + 1, y0 + fold + 1)], fill=LIGHT)
    c.line(x1 - fold, y0, x1 - fold, y0 + fold)
    c.line(x1 - fold, y0 + fold, x1, y0 + fold)


def doc(c: Canvas) -> None:
    page(c)


def doc_txt(c: Canvas) -> None:
    page(c)
    for i, y in enumerate((22, 28, 34, 40, 46, 52)):
        c.rect(18, y, 45 if i % 3 != 2 else 36, y + 1, MID)


def doc_md(c: Canvas) -> None:
    page(c)
    c.line(18, 46, 18, 26)            # M
    c.line(18, 26, 24, 36)
    c.line(24, 36, 30, 26)
    c.line(30, 26, 30, 46)
    c.line(39, 26, 39, 44)            # down arrow
    c.poly([(33, 38), (46, 38), (39.5, 47)], fill=INK, outline=None)


def doc_gmi(c: Canvas) -> None:
    page(c)
    c.rect(18, 30, 30, 31, INK)       # =>
    c.rect(18, 38, 30, 39, INK)
    c.line(34, 26, 44, 34)
    c.line(44, 34, 34, 42)
    for y in (50, 54):
        c.rect(18, y, 44, y + 1, MID)


def doc_lisp(c: Canvas) -> None:
    page(c)
    c.line(20, 22, 24, 22)            # lambda
    c.line(24, 22, 38, 50)
    c.line(30, 34, 20, 50)
    c.line(38, 50, 42, 50)


def doc_c(c: Canvas) -> None:
    page(c)
    c.ellipse(31, 37, 11, 13, fill=None, outline=INK, t=3)
    c.rect(36, 30, 44, 44, WHITE)     # the opening of the C
    c.rect(38, 28, 39, 31, INK)
    c.rect(38, 43, 39, 46, INK)


def folder(c: Canvas) -> None:
    c.poly([(4, 14), (22, 14), (27, 20), (60, 20), (60, 54), (4, 54)], fill=PALE)
    c.rect(6, 24, 58, 25, INK)        # the flap's edge


def files(c: Canvas) -> None:
    c.poly([(16, 6), (40, 6), (48, 14), (48, 30), (16, 30)])
    for y in (14, 19, 24):
        c.rect(21, y, 41, y + 1, MID)
    c.poly([(4, 18), (22, 18), (27, 24), (60, 24), (60, 58), (4, 58)], fill=PALE)
    c.rect(6, 28, 58, 29, INK)


def book(c: Canvas) -> None:
    c.box(14, 6, 49, 57)
    c.rect(14, 6, 21, 57, INK)        # the spine
    c.rect(18, 8, 19, 55, MID)
    for x in range(50, 54, 2):        # the pages' edge
        c.rect(x, 9 + (x - 50), x, 58 - (x - 50) // 2, MID)
    c.rect(26, 16, 43, 17, INK)       # the title on the cover
    c.rect(26, 21, 39, 22, INK)
    c.frame(25, 13, 44, 26, MID, 1)


def reader(c: Canvas) -> None:
    c.poly([(4, 14), (31, 19), (31, 55), (4, 50)])
    c.poly([(33, 19), (60, 14), (60, 50), (33, 55)])
    c.rect(31, 19, 33, 55, INK)
    for i in range(5):
        y = 25 + 5 * i
        c.line(9, y, 26, y + 3, MID, 1)
        c.line(38, y + 3, 55, y, MID, 1)
        c.line(9, y + 1, 26, y + 4, MID, 1)
        c.line(38, y + 4, 55, y + 1, MID, 1)


def chess(c: Canvas) -> None:
    c.ellipse(32, 17, 8, 8, fill=WHITE)               # head
    c.poly([(26, 26), (38, 26), (38, 30), (26, 30)])  # collar
    c.poly([(28, 30), (36, 30), (42, 48), (22, 48)])  # body
    c.box(16, 48, 47, 57)                             # base
    c.rect(20, 52, 43, 53, MID)


def moonbase(c: Canvas) -> None:
    for x, y in ((8, 8), (20, 4), (50, 10), (58, 22), (12, 26)):
        c.rect(x, y, x + 1, y + 1, INK)
    c.ellipse(32, 47, 18, 18, fill=WHITE)             # the dome, its lower half under the ground
    c.rect(0, 46, 63, 63, MID)                        # the ground
    c.rect(0, 46, 63, 47, INK)
    c.ellipse(9, 56, 6, 3, fill=DARK, outline=None)   # craters
    c.ellipse(32, 58, 7, 3, fill=DARK, outline=None)
    c.ellipse(55, 54, 5, 2, fill=DARK, outline=None)
    c.ellipse(26, 38, 4, 4, fill=LIGHT)               # windows
    c.ellipse(38, 38, 4, 4, fill=LIGHT)
    c.line(32, 29, 32, 14)                            # the mast
    c.ellipse(32, 12, 3, 3, fill=INK, outline=None)


def ceres(c: Canvas) -> None:
    c.ellipse(32, 32, 26, 26, fill=LIGHT)
    for cx, cy, r in ((22, 22, 6), (40, 38, 8), (24, 44, 4), (44, 20, 3)):
        c.ellipse(cx, cy, r, r, fill=MID, outline=DARK, t=1.5)
    c.ellipse(32, 32, 26, 26, fill=None, outline=INK)


def terminal(c: Canvas) -> None:
    c.box(4, 6, 59, 47, fill=PALE)
    c.rect(10, 12, 53, 41, INK)                       # the screen
    c.line(14, 18, 20, 23, WHITE)                     # >
    c.line(20, 23, 14, 28, WHITE)
    c.rect(24, 26, 31, 28, WHITE)                     # cursor
    c.poly([(24, 48), (40, 48), (44, 54), (20, 54)], fill=LIGHT)
    c.box(14, 54, 49, 59, fill=PALE)


def editor(c: Canvas) -> None:
    page(c, 6, 4, 43, 59)
    for y in (22, 28, 34, 40):
        c.rect(12, y, 34, y + 1, MID)
    c.poly([(50, 10), (58, 18), (32, 44), (24, 36)], fill=LIGHT)   # the pencil
    c.poly([(54, 6), (62, 14), (58, 18), (50, 10)], fill=DARK)
    c.poly([(24, 36), (32, 44), (20, 48)], fill=WHITE)
    c.poly([(23, 43), (25, 45), (20, 48)], fill=INK, outline=None)


def settings(c: Canvas) -> None:
    c.box(6, 8, 57, 55, fill=PALE)
    for x, knob in ((18, 22), (32, 38), (46, 28)):
        c.rect(x, 14, x + 1, 49, INK)
        c.box(x - 5, knob, x + 6, knob + 7, fill=WHITE)
        c.rect(x - 2, knob + 3, x + 3, knob + 4, MID)


def bomb(c: Canvas) -> None:
    """The alert for a program that stopped answering: a round bomb, its fuse lit."""
    c.poly([(38, 22), (46, 14), (52, 20), (44, 28)], fill=MID)   # the neck, its cap
    c.ellipse(28, 40, 20, 20, fill=INK, outline=None)            # the body
    c.ellipse(20, 32, 6, 5, fill=DARK, outline=None)              # its shine
    c.ellipse(18, 30, 3, 2, fill=LIGHT, outline=None)
    c.line(49, 16, 50, 12)                                        # the fuse
    c.line(50, 12, 53, 9)
    for x0, y0, x1, y1 in ((56, 0, 56, 3), (56, 11, 56, 14), (49, 6, 51, 6),
                           (61, 6, 63, 6), (51, 1, 52, 2), (60, 10, 61, 11),
                           (60, 1, 61, 2), (51, 10, 52, 11)):        # the spark
        c.line(x0, y0, x1, y1)
    c.rect(55, 6, 56, 7, INK)


def app(c: Canvas) -> None:
    c.poly([(32, 2), (62, 32), (32, 62), (2, 32)], fill=LIGHT)
    c.poly([(32, 16), (48, 32), (32, 48), (16, 32)], fill=WHITE)
    c.rect(28, 30, 35, 33, INK)


ICONS: dict[str, Callable[[Canvas], None]] = {
    "app": app, "bomb": bomb, "book": book, "ceres": ceres, "chess": chess, "doc": doc, "doc-c": doc_c,
    "doc-gmi": doc_gmi, "doc-lisp": doc_lisp, "doc-md": doc_md, "doc-txt": doc_txt, "editor": editor,
    "files": files, "folder": folder, "moonbase": moonbase, "reader": reader, "settings": settings,
    "terminal": terminal,
}


def write_png(path: Path, c: Canvas) -> None:
    """8-bit gray with alpha (colour type 4)."""
    raw = bytearray()
    for row in c.px:
        raw.append(0)
        for v in row:
            raw += bytes((0, 0)) if v is None else bytes((v, 255))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 4, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "assets" / "icons")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, draw in ICONS.items():
        c = Canvas()
        draw(c)
        write_png(args.out / f"{name}.png", c)
    print(f"draw_icons: {len(ICONS)} icons in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
