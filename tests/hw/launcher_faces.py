"""The launcher's two faces (77.2) on a board, by a pointer alone.

By a finger where the board has touch (`ts tap`, `ts hold X Y MS` for the
long press) and by the mouse (`ms click`, `ms click X Y right`): the Files
tab, /sd0, help, a Markdown file opened in the reader; back to the launcher
by its menu bar title; the file's sheet adds it to the shortcuts; the
Shortcuts tab, the new tile's sheet removes it, confirmed in the alert.
Every place comes from `launcher status` (window-relative boxes, with the
name of each row and tile) and `ribbon list` (where the window is).

A suite passes `console(line)`, as for titlebar_menu.py, whose ways of
finding the chrome's scale (from a window's cell width) and the pointers a
board has (`ms`, `ts`) this shares. Landscape only: down a portrait screen
the check is skipped. The shortcuts are put back to the defaults.
"""

import re
import time
from collections.abc import Callable

HIT = re.compile(r'hit id (\d+) index (-?\d+): x (\d+)\.\.(\d+) y (\d+)\.\.(\d+)[^\n"]*(?:"([^"\n]*)")?')


def check(console: Callable[[str], str]) -> tuple[bool, str]:
    steps: list[tuple[str, bool, str]] = []

    def step(what: str, ok: bool, got: object) -> bool:
        steps.append((what, ok, str(got)[-300:]))
        return ok

    def place() -> tuple[int, int] | None:
        """The launcher's content's top left on the screen."""
        rl = console("ribbon list")
        v = re.search(r"axis (\w), view (\d+)", rl)
        w = re.search(r"Launcher \(slot \d+, \d+ cols, at (\d+)\.\.", rl)
        if not (v and w) or v.group(1) != "X":
            return None
        return int(w.group(1)) - int(v.group(2)) + 1, (22 + 17) * scale

    def hits() -> tuple[list[tuple[int, int, int, int, str]], str]:
        st = console("launcher status")
        p = place()
        out = []
        for m in HIT.finditer(st):
            if p is None:
                break
            x = p[0] + (int(m.group(3)) + int(m.group(4))) // 2
            y = p[1] + (int(m.group(5)) + int(m.group(6))) // 2
            out.append((int(m.group(1)), int(m.group(2)), x, y, m.group(7) or ""))
        return out, st

    def find(hs, hid: int, name: str | None = None, index: int | None = None):
        for h in hs:
            if h[0] == hid and (name is None or h[4] == name) and (index is None or h[1] == index):
                return h
        return None

    def tap(way: str, h) -> None:
        console(f"ts tap {h[2]} {h[3]}" if way == "touch" else f"ms click {h[2]} {h[3]}")
        time.sleep(0.4)

    def context(way: str, h) -> None:
        console(f"ts hold {h[2]} {h[3]} 900" if way == "touch" else f"ms click {h[2]} {h[3]} right")
        time.sleep(0.4)

    console("shortcuts reset")
    console("launcher close")
    if "launcher: opened" not in console("launcher open"):
        return False, "the launcher did not open"
    rl = console("ribbon list")
    w = re.search(r"Launcher \(slot \d+, (\d+) cols, at (\d+)\.\.(\d+)", rl)
    if not w or "axis X" not in rl:
        console("launcher close")
        return True, "SKIPPED: the ribbon runs down a portrait screen"
    scale = 2 if (int(w.group(3)) - int(w.group(2)) + 1 - 7) // max(int(w.group(1)), 1) >= 16 else 1
    mouse = "ms: pointer at" in console("ms")
    ts = console("ts")
    ways = tuple((["touch"] if "touch:" in ts and "no touch" not in ts else []) + (["mouse"] if mouse else []))
    if not ways:
        console("launcher close")
        return True, "SKIPPED: neither a touch screen nor a mouse to drive it"
    try:
        for way in ways:
            console("launcher open")                                   # in front: a click on another window only focuses it
            hs, st = hits()
            if "face Files" not in st:
                h = find(hs, 2)                                       # the Files tab
                if not step(f"{way}: the Files tab", h is not None, st):
                    continue
                tap(way, h)
                hs, st = hits()
            # Up to the roots by the path bar's first segment, then /sd0.
            h = find(hs, 20, index=0)
            if h:
                tap(way, h)
                hs, st = hits()
            h = find(hs, 30, "sd0")
            if not step(f"{way}: the roots list sd0", h is not None, st):
                continue
            tap(way, h)
            hs, st = hits()
            h = find(hs, 30, "help")
            if not step(f"{way}: /sd0 lists help", h is not None, st):
                continue
            tap(way, h)
            hs, st = hits()
            h = find(hs, 30, "about.md")
            if not step(f"{way}: help lists about.md", h is not None, st):
                continue
            tap(way, h)
            rl = console("ribbon list")
            step(f"{way}: about.md open in the reader", re.search(r"Reader [^\n]*\*", rl) is not None, rl)
            # Back by the menu bar's title, LugalOS.
            console(f"ts tap {20 * scale} {8 * scale}" if way == "touch" else f"ms click {20 * scale} {8 * scale}")
            time.sleep(0.5)
            hs, st = hits()
            h = find(hs, 30, "about.md")
            if not step(f"{way}: the title brings the launcher back", h is not None, st):
                continue
            context(way, h)
            hs, st = hits()
            m = re.search(r"sheet: about\.md:[^\n]*\[(\d)\] Add to Shortcuts", st)
            if not step(f"{way}: about.md's sheet", m is not None, st):
                continue
            tap(way, find(hs, 550 + int(m.group(1))))
            sc = console("shortcuts")
            step(f"{way}: added to the shortcuts", "/sd0/help/about.md" in sc, sc)
            hs, st = hits()
            tap(way, find(hs, 1))                                      # the Shortcuts tab
            hs, st = hits()
            tiles = [h for h in hs if h[0] == 10]
            if not step(f"{way}: the new tile shows", bool(tiles) and tiles[-1][1] == 6, st):
                continue
            context(way, tiles[-1])
            hs, st = hits()
            m = re.search(r"sheet: [^\n]*\[(\d)\] Remove", st)
            if not step(f"{way}: the tile's sheet", m is not None, st):
                continue
            tap(way, find(hs, 550 + int(m.group(1))))
            hs, st = hits()
            h = find(hs, 541)                                          # the alert's Remove, its default
            if not step(f"{way}: Remove asks first", h is not None and "alert: Remove the shortcut?" in st, st):
                continue
            tap(way, h)
            sc = console("shortcuts")
            step(f"{way}: the shortcut removed", "about.md" not in sc and "shortcuts: ok, 6" in sc, sc)
            console("ribbon focus 0")
    finally:
        # The reader windows this opened, and the launcher, closed again.
        for _ in range(4):
            rl = console("ribbon list")
            m = re.search(r"\[(\d)\] Reader", rl)
            if not m:
                break
            console(f"ribbon focus {m.group(1)}")
            console("key cmd-w")
            time.sleep(0.4)
        console("launcher close")
        console("shortcuts reset")

    bad = [f"{what}: {got}" for what, ok, got in steps if not ok]
    if bad:
        return False, "; ".join(bad)
    return True, f"{len(steps)} steps by {' and '.join(ways)}: Files to a document, a shortcut added and removed"
