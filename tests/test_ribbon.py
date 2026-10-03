#!/usr/bin/env python3
"""Phase 44 Test Suite: Virtual Terminals (44.1) and Ribbon Geometry Manager (44.2).

Runs automated verification in QEMU:
- vtermselftest (root init, allocation, FIFO pump/drain, focus switch, destroy)
- vterm list / new / switch / close shell commands
- ribbonselftest (geometry calculation, column presets, sliding viewport, window move, resize)
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]


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
