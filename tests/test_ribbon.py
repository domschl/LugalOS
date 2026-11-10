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
    check("landscape again: the stack is back", len(stb) == 2 and [w[0] for w in back] == [w[0] for w in wins], (back, stb))
    session.send_and_expect("key cmd-]", r"lsh> ", timeout=5.0)
    out, sto = listing()
    check("Cmd+]: out of the stack, a column of its own on the right",
          not sto and out[focused[0]][0] == "View" and out[focused[0]][3] > out[focused[0] - 1][4], (out, sto))
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
