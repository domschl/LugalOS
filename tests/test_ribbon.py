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
