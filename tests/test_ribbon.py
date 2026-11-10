#!/usr/bin/env python3
"""Phase 44 Test Suite: Virtual Terminals (44.1) and Ribbon Geometry Manager (44.2).

Runs automated verification in QEMU:
- vtermselftest (root init, allocation, FIFO pump/drain, focus switch, destroy)
- vterm list / new / switch / close shell commands
- ribbonselftest (geometry calculation, column presets, sliding viewport, window move, resize)
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]


WIN = re.compile(r"\[(\d)\] (\w+)\s+\((?:[^,]*, )*?(\d+) (cols|rows), at (\d+)\.\.(\d+)[^)]*\)( \*)?")


def _ribbon(session: QemuSession) -> tuple[str, list[tuple[str, int, str, int, int, bool]]]:
    _, log = session.send_and_expect("ribbon list", r"lsh> ", timeout=5.0)
    head = re.search(r"axis (\w), view (\d+)", log)
    wins = [(m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6)), bool(m.group(7)))
            for m in WIN.finditer(log)]
    return (f"{head.group(1)} {head.group(2)}" if head else "?"), wins


def _portrait(session: QemuSession) -> bool:
    steps: list[tuple[str, bool, str]] = []

    def check(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)))

    for cmd, want in (("lisp", r"lisp> "), ("(canvas-window 'split-half)", r"=> #t"), ("exit", r"lsh> ")):
        session.send_and_expect(cmd, want, timeout=5.0)
    head0, land = _ribbon(session)
    session.send_and_expect("ramscreen turn", r"ramscreen: 480 x 800", timeout=5.0)
    head, port = _ribbon(session)
    check("turned: axis Y, a top/bottom pair, order and focus kept",
          head.startswith("Y") and [w[0] for w in port] == [w[0] for w in land] and
          [w[5] for w in port] == [w[5] for w in land] and all(w[2] == "rows" for w in port) and
          port[0][5] and port[1][3] > port[0][4] and port[1][4] + 2 < 800, port)
    _, log = session.send_and_expect("lisp", r"lisp> ", timeout=5.0)
    _, log = session.send_and_expect("(canvas-size)", r"=> \(\d+ \d+\)", timeout=5.0)
    m = re.search(r"=> \((\d+) (\d+)\)", log)
    session.send_and_expect("exit", r"lsh> ", timeout=5.0)
    check("the canvas spans the width, under the terminal", bool(m) and int(m.group(1)) > 400 and int(m.group(2)) < 400,
          m.group(0) if m else log[-200:])
    session.send_and_expect("viewtest open", r"lsh> ", timeout=5.0)
    head, three = _ribbon(session)
    view = int(head.split()[1])
    check("a window alone on the screen after the pair's first moves the view down to it",
          len(three) == len(port) + 1 and three[1][0] == "View" and three[1][5] and
          view > 0 and three[1][3] - view >= 22 and three[1][4] + 2 - view <= 800, (head, three))
    # The focused window is always shown whole -- also a window alone at the
    # ribbon's end (Cmd+Down to the last one).
    for _ in range(len(three)):
        session.send_and_expect("key cmd-down", r"lsh> ", timeout=5.0)
    _, log = session.send_and_expect("ribbon list", r"lsh> ", timeout=5.0)
    check("the last window, focused, is shown", "told: focused, shown" in log, log[-400:])
    for _ in range(len(three)):
        session.send_and_expect("key cmd-up", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-down", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-up", r"lsh> ", timeout=5.0)
    head, up = _ribbon(session)
    check("Cmd+Up: the window above, the view back at the top", up[0][5] and head.endswith(" 0"), (head, up))
    session.send_and_expect("key cmd-down", r"lsh> ", timeout=5.0)
    _, down = _ribbon(session)
    check("Cmd+Down: the window below", down[1][5], down)
    session.send_and_expect("key cmd-w", r"lsh> ", timeout=5.0)
    _, closed = _ribbon(session)
    if not closed[0][5]:
        session.send_and_expect("key cmd-up", r"lsh> ", timeout=5.0)
    session.send_and_expect("ramscreen turn", r"ramscreen: 800 x 480", timeout=5.0)
    head, back = _ribbon(session)
    check("turned back: the same columns, order and focus as before", head.startswith("X") and back == land, (back, land))
    session.send_and_expect("lisp", r"lisp> ", timeout=5.0)
    session.send_and_expect("(canvas-window 'text)", r"=> #t", timeout=5.0)
    session.send_and_expect("exit", r"lsh> ", timeout=5.0)
    ok = all(s[1] for s in steps)
    for what, good, got in steps:
        print(f"    [{'PASS' if good else 'FAIL'}] {what}" + ("" if good else f": {got}"))
    return ok


def _wm_keys(session: QemuSession) -> bool:
    """The window manager's width keys (kernel/wmkeys.c) give the same widths
    through a view's keys (`key`) and over the serial line (CSI-u through the
    line editor); Cmd+\\ no longer is one. 800 px: 38, 48, 64, 98 columns."""
    steps: list[tuple[str, bool, str]] = []

    def width() -> int:
        _, wins = _ribbon(session)
        focused = [w for w in wins if w[5]]
        return focused[0][1] if focused else -1

    session.send_and_expect("key cmd-f", r"lsh> ", timeout=5.0)
    if width() != 98:
        session.send_and_expect("key cmd-f", r"lsh> ", timeout=5.0)
    seq = [
        ("key cmd-f", 48), ("key cmd-=", 64), ("key cmd--", 48), ("key cmd-r", 64), ("key cmd-r", 98),
        ("key cmd-r", 38), ("\x1b[114;9u", 48), ("\x1b[61;9u", 64), ("\x1b[45;9u", 48),
        ("\x1b[102;9u", 98), ("\x1b[102;9u", 48), ("\x1b[92;9u", 48),
    ]
    for how, want in seq:
        session.send_and_expect(how, r"lsh> ", timeout=5.0)
        got = width()
        steps.append((f"{how!r} -> {want} columns", got == want, str(got)))
    ok = all(st[1] for st in steps)
    for what, good, got in steps:
        print(f"    [{'PASS' if good else 'FAIL'}] {what}" + ("" if good else f": got {got}"))
    return ok


STACK = re.compile(r"stack: (\d) of (\d), (\d+) rows, y (\d+)\.\.(\d+)")


def _stacks(session: QemuSession) -> bool:
    """Stacks on the RAM screen by the window manager's keys: Cmd+[ joins
    the column on the left, Cmd+Up/Down move within it, Cmd+Shift+R changes
    the height, a turn to portrait lays the stack out window by window and
    turning back restores it, Cmd+] leaves it."""
    steps: list[tuple[str, bool, str]] = []

    def check(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)))

    def listing() -> tuple[list[tuple[str, int, str, int, int, bool]], list[tuple[int, int, int, int, int]]]:
        _, log = session.send_and_expect("ribbon list", r"lsh> ", timeout=5.0)
        wins = [(m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6)), bool(m.group(7)))
                for m in WIN.finditer(log)]
        return wins, [tuple(int(g) for g in m.groups()) for m in STACK.finditer(log)]  # type: ignore[misc]

    session.send_and_expect("viewtest open", r"lsh> ", timeout=5.0)
    before, _ = listing()
    session.send_and_expect("key cmd-[", r"lsh> ", timeout=5.0)
    wins, st = listing()
    focused = [i for i, w in enumerate(wins) if w[5]]
    check("Cmd+[: the view joins the column on its left, below",
          len(st) == 2 and st[0][:2] == (1, 2) and st[1][:2] == (2, 2) and st[1][3] > st[0][4] and
          len(focused) == 1 and wins[focused[0]][0] == "View" and wins[focused[0]][3] == wins[focused[0] - 1][3],
          (before, wins, st))
    session.send_and_expect("key cmd-up", r"lsh> ", timeout=5.0)
    up, _ = listing()
    check("Cmd+Up: the window above has the focus", focused and up[focused[0] - 1][5], up)
    session.send_and_expect("key cmd-down", r"lsh> ", timeout=5.0)
    rows0 = st[1][2]
    session.send_and_expect("key cmd-shift-r", r"lsh> ", timeout=5.0)
    _, st2 = listing()
    check("Cmd+Shift+R: another height, the column's rows shared out",
          len(st2) == 2 and st2[1][2] != rows0 and abs((st2[0][2] + st2[1][2]) - (st[0][2] + st[1][2])) <= 1, (st, st2))
    session.send_and_expect("ramscreen turn", r"ramscreen: 480 x 800", timeout=5.0)
    port, stp = listing()
    check("portrait: no stacks, window by window", not stp and len(port) == len(wins), (port, stp))
    session.send_and_expect("ramscreen turn", r"ramscreen: 800 x 480", timeout=5.0)
    back, stb = listing()
    check("landscape again: the stack is back, as wide as it was",
          len(stb) == 2 and [w[0] for w in back] == [w[0] for w in wins] and [w[1] for w in back] == [w[1] for w in wins],
          (back, wins, stb))
    session.send_and_expect("key cmd-]", r"lsh> ", timeout=5.0)
    out, sto = listing()
    check("Cmd+]: out of the stack, a column of its own on the right",
          not sto and out[focused[0]][0] == "View" and out[focused[0]][3] > out[focused[0] - 1][4], (out, sto))
    session.send_and_expect("key cmd-w", r"lsh> ", timeout=5.0)
    ok = all(st_[1] for st_ in steps)
    for what, good, got in steps:
        print(f"    [{'PASS' if good else 'FAIL'}] {what}" + ("" if good else f": {got}"))
    return ok


def _fullscreen(session: QemuSession) -> bool:
    """Cmd+Shift+F on a view and on a terminal: the window covers the screen
    (its content the screen's width), the hint shows and goes, the pointer at
    the top edge reveals the bars and a click on the title bar leaves; the
    key leaves too; full screen follows the focus."""
    import time
    steps: list[tuple[str, bool, str]] = []

    def check(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)))

    def status() -> tuple[str, list[tuple[str, int, str, int, int, bool]]]:
        _, log = session.send_and_expect("ribbon list", r"lsh> ", timeout=5.0)
        head = re.search(r"Ribbon Status: [^\n]*", log)
        wins = [(m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6)), bool(m.group(7)))
                for m in WIN.finditer(log)]
        return (head.group(0) if head else "?"), wins

    session.send_and_expect("viewtest open", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-shift-f", r"lsh> ", timeout=5.0)
    head, wins = status()
    view = [w for w in wins if w[0] == "View"]
    check("Cmd+Shift+F: the view covers the screen, the hint over it",
          "full screen (hint)" in head and view and view[0][3:5] == (0, 801), (head, wins))
    time.sleep(3.0)
    head, _ = status()
    check("the hint goes after a while", "full screen" in head and "hint" not in head, head)
    session.send_and_expect("ms move 300 100", r"lsh> ", timeout=5.0)
    session.send_and_expect("ms move 300 0", r"lsh> ", timeout=5.0)
    head, _ = status()
    check("the pointer at the top edge reveals the bars", "bars revealed" in head, head)
    session.send_and_expect("ms click 400 30", r"lsh> ", timeout=5.0)
    head, wins = status()
    view = [w for w in wins if w[0] == "View"]
    check("a click on the revealed title bar leaves full screen",
          "full screen" not in head and view and view[0][3:5] != (0, 801), (head, wins))
    session.send_and_expect("key cmd-shift-f", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-left", r"lsh> ", timeout=5.0)
    head, _ = status()
    check("full screen follows the focus: another window, the ribbon", "full screen" not in head, head)
    session.send_and_expect("key cmd-right", r"lsh> ", timeout=5.0)
    head, _ = status()
    check("... and back to the view, full screen again", "full screen" in head, head)
    session.send_and_expect("key cmd-shift-f", r"lsh> ", timeout=5.0)
    head, _ = status()
    check("Cmd+Shift+F again leaves", "full screen" not in head, head)
    session.send_and_expect("key cmd-w", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-shift-f", r"lsh> ", timeout=5.0)
    head, wins = status()
    term = [w for w in wins if w[5]]
    check("a terminal full screen too", "full screen" in head and term and term[0][0] == "Term" and term[0][3:5] == (0, 801),
          (head, wins))
    session.send_and_expect("key cmd-shift-f", r"lsh> ", timeout=5.0)
    head, _ = status()
    check("... and back", "full screen" not in head, head)
    ok = all(st_[1] for st_ in steps)
    for what, good, got in steps:
        print(f"    [{'PASS' if good else 'FAIL'}] {what}" + ("" if good else f": {got}"))
    return ok


MENU = re.compile(r"menu \[(\d+)\] (.+?) at (-?\d+) (-?\d+) size (\d+) (\d+)( \(dimmed\))?( \(ticked\))?")


def _menu(session: QemuSession) -> bool:
    """76.4b: the zoom box and the title-bar menu by the mouse (right click,
    clicks on items): widths, the zoom box, a click outside, join, height,
    leave the stack, full screen, close -- each the same as its key."""
    steps: list[tuple[str, bool, str]] = []

    def check(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)[-500:]))

    def listing() -> tuple[int, list[tuple[str, int, str, int, int, bool]], dict[str, tuple[int, int, bool, bool]], str]:
        _, log = session.send_and_expect("ribbon list", r"lsh> ", timeout=5.0)
        head = re.search(r"view (\d+)", log)
        wins = [(m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6)), bool(m.group(7)))
                for m in WIN.finditer(log)]
        items = {m.group(2): (int(m.group(3)) + int(m.group(5)) // 2, int(m.group(4)) + int(m.group(6)) // 2,
                              bool(m.group(7)), bool(m.group(8))) for m in MENU.finditer(log)}
        return (int(head.group(1)) if head else 0), wins, items, log

    def view_win() -> tuple[int, int, int, int]:
        v, wins, _, _ = listing()
        w = [x for x in wins if x[0] == "View"][0]
        return w[3] - v, w[4] - v, w[1], len(wins)

    def open_menu() -> dict[str, tuple[int, int, bool, bool]]:
        x0, x1, _, _ = view_win()
        session.send_and_expect(f"ms click {(x0 + x1) // 2} 30 right", r"lsh> ", timeout=5.0)
        return listing()[2]

    def pick(items: dict[str, tuple[int, int, bool, bool]], label: str) -> None:
        x, y, _, _ = items[label]
        session.send_and_expect(f"ms click {x} {y}", r"lsh> ", timeout=5.0)

    session.send_and_expect("viewtest open", r"lsh> ", timeout=5.0)
    items = open_menu()
    cols = view_win()[2]
    check("right click on the title bar: the menu, the width ticked, Move Right dimmed",
          "1/2 Width" in items and "Close" in items and items["Move Right"][2] and
          any(t[3] for k, t in items.items() if k.endswith("Width")), (cols, items))
    pick(items, "1/2 Width")
    x0, x1, cols, _ = view_win()
    _, _, after, _ = listing()
    check("1/2 Width: the view's column at half (48 columns), the menu closed", cols == 48 and not after, (cols, after))
    session.send_and_expect(f"ms click {x1 - 10} 30", r"lsh> ", timeout=5.0)
    check("the zoom box: full width", view_win()[2] == 98, view_win())
    session.send_and_expect(f"ms click {view_win()[1] - 10} 30", r"lsh> ", timeout=5.0)
    check("... and back", view_win()[2] == 48, view_win())
    items = open_menu()
    session.send_and_expect("ms click 5 300", r"lsh> ", timeout=5.0)
    _, _, after, _ = listing()
    check("a click outside closes the menu, nothing else", not after and view_win()[2] == 48, (after, view_win()))
    pick(open_menu(), "Join Left")
    _, _, _, log = listing()
    check("Join Left: in the stack on its left", "stack: 2 of 2" in log, log)
    items = open_menu()
    check("in a stack: heights and Leave Stack", "2/3 Height" in items and "Leave Stack Right" in items, items)
    pick(items, "2/3 Height")
    items = open_menu()
    check("2/3 Height: ticked now", items.get("2/3 Height", (0, 0, False, False))[3], items)
    pick(items, "Leave Stack Right")
    _, _, _, log = listing()
    check("Leave Stack Right: a column of its own", "stack:" not in log, log)
    pick(open_menu(), "Full Screen")
    _, _, _, log = listing()
    check("Full Screen", "full screen" in log, log[-300:])
    session.send_and_expect("key cmd-shift-f", r"lsh> ", timeout=5.0)
    pick(open_menu(), "Close")
    _, wins, _, _ = listing()
    check("Close: the view's window gone", not any(w[0] == "View" for w in wins), wins)

    ok = all(st_[1] for st_ in steps)
    for what, good, got in steps:
        print(f"    [{'PASS' if good else 'FAIL'}] {what}" + ("" if good else f": {got}"))
    return ok


def _handles(session: QemuSession) -> bool:
    """76.4b: a column's edge and a stack's gap dragged by the mouse (`ms
    drag`) snap to the presets: the column to 2/3, the full width and 2/5
    (64, 98, 38 columns at 800 px); the window above a gap to 2/3 of the
    column and back to 1/3."""
    steps: list[tuple[str, bool, str]] = []

    def check(what: str, ok: bool, got: object) -> None:
        steps.append((what, ok, str(got)[-500:]))

    def listing() -> tuple[int, list[tuple[str, int, str, int, int, bool]], list[tuple[int, ...]]]:
        _, log = session.send_and_expect("ribbon list", r"lsh> ", timeout=5.0)
        head = re.search(r"view (\d+)", log)
        wins = [(m.group(2), int(m.group(3)), m.group(4), int(m.group(5)), int(m.group(6)), bool(m.group(7)))
                for m in WIN.finditer(log)]
        return (int(head.group(1)) if head else 0), wins, [tuple(int(g) for g in m.groups()) for m in STACK.finditer(log)]  # type: ignore[misc]

    session.send_and_expect("viewtest open", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-left", r"lsh> ", timeout=5.0)    # the terminal: its column to half
    for _ in range(3):
        session.send_and_expect("key cmd--", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-=", r"lsh> ", timeout=5.0)
    v, wins, _ = listing()
    fi = next(i for i, w in enumerate(wins) if w[5])              # the terminal's column, on the screen
    edge = wins[fi][4] - v + 5                                     # in the gap right of it
    check("the terminal's column at half (48)", wins[fi][1] == 48, wins)
    for to, want in ((520, 64), (790, 98), (300, 38)):
        x0 = wins[fi][3] - v
        session.send_and_expect(f"ms drag {edge} 200 {x0 + to - 4} 200", r"lsh> ", timeout=10.0)
        v, wins, _ = listing()
        check(f"its edge dragged to {to - 4} px from its left: {want} columns", wins[fi][1] == want, wins)
        edge = wins[fi][4] - v + 5
    session.send_and_expect("key cmd-right", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-[", r"lsh> ", timeout=5.0)    # the view below the terminal
    v, wins, st = listing()
    gx = wins[fi][3] - v + 50                                      # over the stacked column
    gap = (st[0][4] + st[1][3]) // 2 if len(st) == 2 else 0
    session.send_and_expect(f"ms drag {gx} {gap} {gx} 330", r"lsh> ", timeout=10.0)
    _, _, st2 = listing()
    check("the gap dragged down: the upper window at 2/3 (16 of 25 rows)", len(st2) == 2 and st2[0][2] == 16, (st, st2))
    gap = (st2[0][4] + st2[1][3]) // 2 if len(st2) == 2 else 0
    session.send_and_expect(f"ms drag {gx} {gap} {gx} 120", r"lsh> ", timeout=10.0)
    _, _, st3 = listing()
    check("... and up: 1/3 (8 rows)", len(st3) == 2 and st3[0][2] == 8, st3)
    session.send_and_expect("key cmd-]", r"lsh> ", timeout=5.0)
    session.send_and_expect("key cmd-w", r"lsh> ", timeout=5.0)
    ok = all(st_[1] for st_ in steps)
    for what, good, got in steps:
        print(f"    [{'PASS' if good else 'FAIL'}] {what}" + ("" if good else f": {got}"))
    return ok


def run_tests() -> int:
    elf_path = REPO_ROOT / "build" / "rv32" / "lugalos.elf"
    img_path = REPO_ROOT / "build" / "rv32" / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/rv32' first.")
        return 1

    arch_img = img_path.with_name("test_rv32_ribbon_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 44.1 & 44.2] Testing vterm & ribbon geometry using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    try:
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return 1

        # 1. Test vterm selftest
        print("  Running vtermselftest...")
        ok, log = session.send_and_expect("vtermselftest", r"vterm selftest: (PASSED|FAILED)", timeout=5.0)
        if ok and "vterm selftest: PASSED" in log:
            print("    [PASS] vtermselftest")
        else:
            print(f"    [FAIL] vtermselftest:\n{log}")
            all_passed = False

        # 2. Test vterm shell commands
        print("  Testing vterm shell operations...")
        ok, log = session.send_and_expect("vterm list", r"Virtual Terminals \(1 allocated\)", timeout=5.0)
        if not ok:
            print(f"    [FAIL] vterm list root check:\n{log}")
            all_passed = False

        ok, log = session.send_and_expect("vterm new WorkerTerminal", r"Spawned terminal on vterm 1", timeout=5.0)
        if not ok:
            print(f"    [FAIL] vterm new:\n{log}")
            all_passed = False

        ok, log = session.send_and_expect("vterm list", r"Virtual Terminals \(2 allocated\)", timeout=5.0)
        if not ok or "WorkerTerminal" not in log:
            print(f"    [FAIL] vterm list with 2 terminals:\n{log}")
            all_passed = False

        ok, log = session.send_and_expect("vterm switch 0", r"Switched to vterm 0", timeout=5.0)
        if not ok:
            print(f"    [FAIL] vterm switch 0:\n{log}")
            all_passed = False
        else:
            print("    [PASS] vterm shell commands (new, list, switch)")

        # 3. Test ribbon selftest
        print("  Running ribbonselftest...")
        ok, log = session.send_and_expect("ribbonselftest", r"ribbon geometry selftest: (PASSED|FAILED)", timeout=5.0)
        if ok and "ribbon geometry selftest: PASSED" in log:
            print("    [PASS] ribbonselftest (22 assertions)")
        else:
            print(f"    [FAIL] ribbonselftest:\n{log}")
            all_passed = False

        # 4. Test usbkbd hotkeys
        print("  Running usbkbdselftest (Phase 44 hotkeys)...")
        ok, log = session.send_and_expect("usbkbdselftest", r"USBKBD_SELFTEST_(OK|FAIL)", timeout=5.0)
        if ok and "USBKBD_SELFTEST_OK" in log:
            print("    [PASS] usbkbdselftest (Phase 44 hotkeys: Enter, Arrows, Ctrl-Arrows, W, 1..9)")
        else:
            print(f"    [FAIL] usbkbdselftest:\n{log}")
            all_passed = False

        # 5. Test Canvas & Ribbon integration (Milestone 44.5)
        print("  Testing Canvas & Ribbon integration (Lisp canvas-window)...")
        ok, log = session.send_and_expect("lisp", r"lisp> ", timeout=5.0)
        if not ok:
            print(f"    [FAIL] Entering lisp:\n{log}")
            all_passed = False
        else:
            steps = [
                ("(canvas-window 'split-narrow)", r"=> #t"),
                ("(canvas-size)", r"=> \(261 434\)"),
                ("(canvas-line 10 10 100 100 1)", r"=> #t"),
                ("(canvas-window 'split-half)", r"=> #t"),
                ("(canvas-window 'ribbon)", r"=> #t"),
                ("(canvas-window 'text)", r"=> #t"),
                ("exit", r"lsh> "),
            ]
            canvas_ok = True
            for expr, expected in steps:
                ok, log = session.send_and_expect(expr, expected, timeout=5.0)
                if not ok:
                    print(f"    [FAIL] {expr} expected {expected}, got:\n{log}")
                    canvas_ok = False
                    all_passed = False
                    break
            if canvas_ok:
                print("    [PASS] Canvas & Ribbon integration (split, split-narrow, ribbon, line, text)")

        # 6. 76.3: the ribbon down a portrait screen (the RAM screen turned,
        # as the Tab5's panel turns): a side-by-side pair becomes a top/bottom
        # pair, the order and the focus kept; a third window moves the view
        # down; Cmd+Up / Down step through it; turning back restores the
        # columns.
        print("  Testing the portrait ribbon (ramscreen turn)...")
        portrait_ok = _portrait(session)
        all_passed = all_passed and portrait_ok

        print("  Testing the window manager's width keys (key and serial CSI-u)...")
        all_passed = _wm_keys(session) and all_passed

        print("  Testing stacks (Cmd+[ / ], Cmd+Up/Down, Cmd+Shift+R, a turn)...")
        all_passed = _stacks(session) and all_passed

        print("  Testing full screen (Cmd+Shift+F, the hint, the pointer at the top edge)...")
        all_passed = _fullscreen(session) and all_passed

        print("  Testing the zoom box and the title-bar menu (76.4b)...")
        all_passed = _menu(session) and all_passed

        print("  Testing the handles: a column's edge, a stack's gap (76.4b)...")
        all_passed = _handles(session) and all_passed

    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    if all_passed:
        print("[Phase 44.1 & 44.2] ALL TESTS PASSED.")
        return 0
    else:
        print("[Phase 44.1 & 44.2] SOME TESTS FAILED.")
        return 1


if __name__ == "__main__":
    sys.exit(run_tests())
