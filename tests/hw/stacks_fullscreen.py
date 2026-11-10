"""Stacks and full screen (76.4a) on a board, by the window manager's keys.

The launcher and the root terminal make a column of two (Cmd+[), the focus
moves within it (Cmd+Up/Down), a height preset changes the rows
(Cmd+Shift+R), Cmd+] takes the launcher out again; then the launcher goes
full screen (Cmd+Shift+F) and is left again by every way the board has: a
swipe down from the top edge (`ts swipe top`) where it has touch, the pointer
at the top edge and a click on the revealed title bar, and the key. Every step is read back
from `ribbon list`.

A suite passes `console(line)` (one shell line's output), as for
hello_ugui.py. Landscape only: a portrait Tab5 has no stacks.
"""

import re
import time
from collections.abc import Callable

WIN = re.compile(r"\[(\d)\] (\w+)\s+\((?:[^,]*, )*?(\d+) (cols|rows), at (-?\d+)\.\.(-?\d+)[^)]*\)( \*)?")
STACK = re.compile(r"stack: (\d) of (\d), (\d+) rows, y (\d+)\.\.(\d+)")
HEAD = re.compile(r"Ribbon Status: (\d+) windows, active idx (\d+), axis (\w)[^\n]*")


def check(console: Callable[[str], str]) -> tuple[bool, str]:
    steps: list[tuple[str, bool, str]] = []

    def step(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)[-400:]))

    def listing() -> tuple[str, list[tuple[str, bool, int, int, int]], list[tuple[int, ...]]]:
        out = console("ribbon list")
        h = HEAD.search(out)
        wins = [(m.group(2), bool(m.group(7)), int(m.group(5)), int(m.group(6)), int(m.group(3)))
                for m in WIN.finditer(out)]
        return (h.group(0) if h else out[-300:]), wins, [tuple(int(g) for g in m.groups()) for m in STACK.finditer(out)]

    head, wins, _ = listing()
    if " axis X" not in head:
        return True, "SKIPPED (the ribbon runs down the screen: no stacks in portrait)"
    console("launcher open")
    head, wins, _ = listing()
    li = next((i for i, x in enumerate(wins) if x[1]), -1)
    if li < 1 or wins[li][0] != "Launcher":
        console("launcher close")
        return False, f"the launcher is not focused with a window on its left: {head} {wins}"
    width = wins[li][3] - wins[li][2] + 1
    cell = (width - 7) // max(wins[li][4], 1)          # a window c columns wide spans 8c + 7 px (16c + 7)
    scale = 2 if cell >= 16 else 1                     # CHROME_SCALE
    title_y = (22 + 8) * scale                         # the revealed title bar's middle row

    console("key cmd-[")
    _, wins, st = listing()
    step("Cmd+[: the launcher joins the column on its left", len(st) == 2 and st[1][:2] == (2, 2), (wins, st))
    console("key cmd-up")
    _, up, _ = listing()
    step("Cmd+Up: the window above", up[li - 1][1], up)
    console("key cmd-down")
    rows = st[1][2] if len(st) == 2 else -1
    console("key cmd-shift-r")
    _, _, st2 = listing()
    step("Cmd+Shift+R: another height", len(st2) == 2 and st2[1][2] != rows, (st, st2))
    console("key cmd-]")
    _, wins, st3 = listing()
    step("Cmd+]: a column of its own again", not st3 and wins[li][0] == "Launcher" and wins[li][1], (wins, st3))
    width = wins[li][3] - wins[li][2] + 1                # the column it had joined gave it its width

    console("key cmd-shift-f")
    head, wins, _ = listing()
    full = wins[li][3] - wins[li][2] - 1 if wins else 0
    step("Cmd+Shift+F: the launcher covers the screen, the hint over it",
         "full screen (hint)" in head and full > width, (head, wins))
    time.sleep(3.0)
    head, _, _ = listing()
    step("the hint goes", "full screen" in head and "hint" not in head, head)

    w = full                                           # full screen: the content is the screen's width
    mouse = "ms: pointer at" in console("ms")
    ts = console("ts")
    touch = "touch:" in ts and "no touch" not in ts     # where `ts` is built and finds a controller
    # Every way out this board has, each from full screen: touch, the mouse, the key.
    ways = (["touch"] if touch else []) + (["mouse"] if mouse else []) + ["key"]
    for i, way in enumerate(ways):
        if i:
            console("key cmd-shift-f")
            head, _, _ = listing()
            step(f"full screen again for {way}", "full screen" in head, head)
        if way == "mouse":
            console(f"ms move {w // 2} 120")
            console(f"ms move {w // 2} 0")
            head, _, _ = listing()
            step("the pointer at the top edge reveals the bars", "bars revealed" in head, head)
            console(f"ms click {w // 2 + 40} {title_y}")  # the pointer rests on the bars meanwhile
            console(f"ms move {w // 2} 200")
        elif way == "touch":
            console("ts swipe top")                      # a swipe down from the top edge
        else:
            console("key cmd-shift-f")
        head, wins, _ = listing()
        step(f"{way}: out of full screen", "full screen" not in head and wins[li][3] - wins[li][2] + 1 == width,
             (head, wins))

    console("key cmd-left")
    console("key cmd-shift-f")
    head, wins, _ = listing()
    step("a terminal full screen", "full screen" in head and any(x[0] == "Term" and x[1] for x in wins), (head, wins))
    console("key cmd-shift-f")
    head, _, _ = listing()
    step("... and back", "full screen" not in head, head)
    console("launcher close")

    ok = all(s[1] for s in steps)
    detail = "; ".join(f"{'ok' if s[1] else 'FAIL'}: {s[0]}" + ("" if s[1] else f" ({s[2]})") for s in steps)
    return ok, detail

