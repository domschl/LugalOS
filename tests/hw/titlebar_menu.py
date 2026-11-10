"""The zoom box, the title-bar menu and the handles (76.4b) on a board.

On the launcher, by the mouse (`ms click X Y right`, clicks, `ms drag`) and,
where the board has touch, by a finger (`ts hold X Y MS` -- the long press --
`ts tap`, `ts drag`): the menu opens with the launcher's Close dimmed (it has
no close box); 1/2 Width is ticked once chosen; the zoom box goes to the full
width and back; a tap or click outside closes the menu. Across a landscape
screen also: Join Left puts the launcher in a stack, the stack's gap dragged
makes the upper window taller and shorter, Leave Stack Right takes it out,
and its column's edge dragged makes it narrower and wider. Every step is read
back from `ribbon list`, which lists an open menu's items with their places.

A suite passes `console(line)` (one shell line's output), as for
stacks_fullscreen.py. Down a portrait screen the menu's widths are Alone and
One of a Pair.
"""

import re
from collections.abc import Callable

WIN = re.compile(r"\[(\d)\] (\w+)\s+\((?:[^,]*, )*?(\d+) (cols|rows), at (-?\d+)\.\.(-?\d+)[^)]*\)( \*)?")
HEAD = re.compile(r"Ribbon Status: (\d+) windows, active idx (\d+), axis (\w), view (\d+)")
STACK = re.compile(r"stack: (\d) of (\d), (\d+) rows, y (\d+)\.\.(\d+)")
MENU = re.compile(r"menu \[(\d+)\] (.+?) at (-?\d+) (-?\d+) size (\d+) (\d+)( \(dimmed\))?( \(ticked\))?")


def check(console: Callable[[str], str]) -> tuple[bool, str]:
    steps: list[tuple[str, bool, str]] = []

    def step(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)[-400:]))

    def listing() -> tuple[str, int, list[tuple[str, bool, int, int, int]], dict[str, tuple[int, int, bool, bool]], str]:
        out = console("ribbon list")
        h = HEAD.search(out)
        wins = [(m.group(2), bool(m.group(7)), int(m.group(5)), int(m.group(6)), int(m.group(3)))
                for m in WIN.finditer(out)]
        items = {m.group(2): (int(m.group(3)) + int(m.group(5)) // 2, int(m.group(4)) + int(m.group(6)) // 2,
                              bool(m.group(7)), bool(m.group(8))) for m in MENU.finditer(out)}
        return (h.group(3) if h else "?"), (int(h.group(4)) if h else 0), wins, items, out

    console("launcher open")
    axis, view, wins, _, out = listing()
    li = next((i for i, x in enumerate(wins) if x[0] == "Launcher"), -1)
    if li < 1:
        console("launcher close")
        return False, f"no launcher with a window on its left: {out[-300:]}"
    down = axis == "Y"
    if down:
        scale = 2                                        # only the Tab5 turns: 16 x 32 cells
    else:
        cell = (wins[li][3] - wins[li][2] + 1 - 7) // max(wins[li][4], 1)
        scale = 2 if cell >= 16 else 1
    width_item = "One of a Pair" if down else "1/2 Width"

    def bar() -> tuple[int, int, int, int]:
        """The launcher's title bar on the screen: x0, x1, its middle row, its size."""
        _, v, ws, _, _ = listing()
        if down:
            return 12, 700, ws[li][2] - v + 8 * scale, ws[li][4]
        return ws[li][2] - v, ws[li][3] - v, 22 * scale + 8 * scale, ws[li][4]

    mouse = "ms: pointer at" in console("ms")
    ts = console("ts")
    touch = "touch:" in ts and "no touch" not in ts
    ways = (["touch"] if touch else []) + (["mouse"] if mouse else [])
    for way in ways:
        def press(x: int, y: int) -> None:
            console(f"ts tap {x} {y}" if way == "touch" else f"ms click {x} {y}")

        def open_menu() -> dict[str, tuple[int, int, bool, bool]]:
            x0, x1, y, _ = bar()
            x = (x0 + x1) // 2 if x0 >= 0 else x1 // 2
            console(f"ts hold {x} {y} 900" if way == "touch" else f"ms click {x} {y} right")
            return listing()[3]

        items = open_menu()
        step(f"{way}: the menu, the launcher's Close dimmed", "Close" in items and items["Close"][2], items)
        if width_item not in items:
            continue
        press(*items[width_item][:2])
        items = open_menu()
        step(f"{way}: {width_item}, ticked after", items.get(width_item, (0, 0, False, False))[3], items)
        x0, x1, y, _ = bar()
        press(max(x0, 0) + 4, y + 200)                   # outside the menu, in the window
        after = listing()[3]
        step(f"{way}: a press outside closes the menu", not after, after)
        _, x1, y, size = bar()
        press(x1 - 10, y)
        full = bar()[3]
        _, x1, y, _ = bar()
        press(x1 - 10, y)
        step(f"{way}: the zoom box, all of it and back", full > size and bar()[3] == size, (size, full, bar()))
        if down:
            continue                                     # no stacks down the screen
        press(*open_menu()["Join Left"][:2])
        out = listing()[4]
        step(f"{way}: Join Left, a stack", "stack: 2 of 2" in out, out[-300:])
        # The stack's gap dragged down and up (the keyboard away: out, it gives the
        # focused window the whole column).
        if touch:
            console("ts osk hide")
        st = [tuple(int(g) for g in m.groups()) for m in STACK.finditer(listing()[4])]
        if len(st) == 2:
            x0, x1, _, _ = bar()
            gx, gap = max(x0, 0) + 60, (st[0][4] + st[1][3]) // 2
            drag = "ts drag" if way == "touch" else "ms drag"
            console(f"{drag} {gx} {gap} {gx} {st[1][4] - 20}")
            st2 = [tuple(int(g) for g in m.groups()) for m in STACK.finditer(listing()[4])]
            console(f"{drag} {gx} {(st2[0][4] + st2[1][3]) // 2 if len(st2) == 2 else gap} {gx} {st[0][3] + 40}")
            st3 = [tuple(int(g) for g in m.groups()) for m in STACK.finditer(listing()[4])]
            step(f"{way}: the stack's gap dragged down and up, the upper window taller and shorter",
                 len(st2) == 2 and len(st3) == 2 and st2[0][2] > st[0][2] and st3[0][2] < st2[0][2], (st, st2, st3))
        items = open_menu()
        if "Leave Stack Right" in items:
            press(*items["Leave Stack Right"][:2])
        out = listing()[4]
        step(f"{way}: Leave Stack Right, a column of its own", "stack:" not in out, out[-300:])
        # The column's edge dragged: narrower, then wider.
        x0, x1, _, size = bar()
        if x1 + 30 < 2000:
            drag = "ts drag" if way == "touch" else "ms drag"
            console(f"{drag} {x1 + 5} 300 {max(x0, 0) + (x1 - max(x0, 0)) // 2} 300")
            narrow = bar()[3]
            x0, x1, _, _ = bar()
            console(f"{drag} {x1 + 5} 300 {x1 + 300} 300")
            step(f"{way}: the column's edge dragged, narrower and wider again", narrow < size and bar()[3] > narrow,
                 (size, narrow, bar()))
    console("launcher close")

    if not ways:
        return True, "SKIPPED (neither a pointer nor touch on this board)"
    ok = all(s[1] for s in steps)
    return ok, "; ".join(f"{'ok' if s[1] else 'FAIL'}: {s[0]}" + ("" if s[1] else f" ({s[2]})") for s in steps)
