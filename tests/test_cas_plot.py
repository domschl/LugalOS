#!/usr/bin/env python3
"""tests/test_cas_plot.py -- Verification suite for Phase 43.7
Mathematical Plotting on LugalOS Canvas (cas/plot.lisp).

Runs in QEMU (rv32 / rv64) non-destructively.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]

CASES: list[tuple[str, str, str]] = [
    # --- 1. plot-eval numerical evaluator ---
    ("eval polynomial pos", "(plot-eval '(- (^ x 3) (* 3 x)) 'x 2)", "=> 2"),
    ("eval polynomial neg", "(plot-eval '(- (^ x 3) (* 3 x)) 'x -2)", "=> -2"),
    ("eval division", "(plot-eval '(/ 1 x) 'x 2)", "=> 1/2"),
    ("eval division by zero", "(plot-eval '(/ 1 x) 'x 0)", "=> #f"),
    ("eval sqrt exact", "(plot-eval '(sqrt x) 'x 4)", "=> 2"),
    ("eval sqrt negative", "(plot-eval '(sqrt x) 'x -1)", "=> #f"),
    ("eval sin zero", "(plot-eval '(sin x) 'x 0)", "=> 0"),
    ("eval cos zero approx 1", "(> (plot-eval '(cos x) 'x 0) 999999/1000000)", "=> #t"),
    ("eval infix string", '(plot-eval "x^2 + 1" \'x 3)', "=> 10"),
    ("eval procedure lambda", "(plot-eval (lambda (x) (* x x)) 'x 5)", "=> 25"),
    ("eval constant pi", "(plot-eval 'pi 'x 0)", "=> 392699/125000"),
    # --- 2. Sampling & Bounds ---
    ("sample x^2", "(map cdr (plot-sample-fn '(^ x 2) 'x 0 2 2))", "=> (0 1 4)"),
    ("bounds x^2", "(let ((b (plot-find-bounds (list (plot-sample-fn '(^ x 2) 'x 0 2 2))))) #t)", "=> #t"),
    # --- 3. High-level plotting calls ---
    ("plot cubic AST", "(plot '(- (^ x 3) (* 3 x)) '(x -3 3))", "=> #t"),
    ("plot infix string", '(plot "x^2 - 4" \'(x -3 3))', "=> #t"),
    ("plot with options", '(plot "sin(x)" \'(x -6 6) \'((ymin . -2) (ymax . 2)))', "=> #t"),
    ("plot-diff symbolic calculus", '(plot-diff "x^3" \'(x -2 2))', "=> #t"),
    ("plot-diff trig", '(plot-diff "sin(x)" \'(x -4 4))', "=> #t"),
    ("plot-multi multiple curves", '(plot-multi (list "sin(x)" "cos(x)") \'(x -3 3))', "=> #t"),
    ("plot-parametric circle", "(plot-parametric '(cos t) '(sin t) '(t 0 7))", "=> #t"),
]


def run_tests(arch: str = "rv32") -> int:
    elf_path = REPO_ROOT / "build" / arch / "lugalos.elf"
    img_path = REPO_ROOT / "build" / arch / "lugalos_sd.img"
    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/{arch}' first.")
        return 1

    arch_img = img_path.with_name(f"test_{arch}_cas_plot_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.7] Testing CAS plotting ({arch}) using {elf_path}...")
    session = QemuSession(elf_path, arch_img, arch)
    session.start()
    passed = 0
    try:
        ok, _ = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return 1

        script = (
            "lisp\n"
            '(load "/sd0/cas/cas.lisp")\n'
            + "".join(expr + "\n" for _, expr, _ in CASES)
            + '(display "PLOT_TESTS_DONE\\n")\n'
            + "exit\n"
        )
        ok, log = session.send_and_expect(script, r"PLOT_TESTS_DONE", timeout=120.0)
        if not ok:
            print("Failed waiting for PLOT_TESTS_DONE. Captured log:\n", log)
            return 1

        log = log.replace("\r", "")
        print("\n--- Test Results ---")
        pos = 0
        for name, _, expected in CASES:
            idx = log.find(expected, pos)
            if idx != -1:
                print(f"  [PASS] {name}")
                passed += 1
                pos = idx + len(expected)
            else:
                print(f"  [FAIL] {name}: expected '{expected}'")
        print(f"\nTotal: {passed}/{len(CASES)} passed.")
    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    return 0 if passed == len(CASES) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test CAS canvas plotting")
    parser.add_argument("--arch", choices=["rv32", "rv64"], default="rv32")
    sys.exit(run_tests(parser.parse_args().arch))
