#!/usr/bin/env python3
"""tests/test_cas_solve.py -- Verification suite for Phase 43.5
Exact Equation & Linear System Solving (cas/solve.lisp).

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

# (name, lisp expression, expected output after "=> ")
CASES: list[tuple[str, str, str]] = [
    ("linear 3x+4=10", "(solve '(= (+ (* 3 x) 4) 10) 'x)", "(x . 2)"),
    ("linear fractional 3x=2", "(solve '(= (* 3 x) 2) 'x)", "(x . 2/3)"),
    ("linear implicit (no =) 2x-6", "(solve '(- (* 2 x) 6) 'x)", "(x . 3)"),
    ("linear both sides 5x=2x+9", "(solve '(= (* 5 x) (+ (* 2 x) 9)) 'x)", "(x . 3)"),
    ("linear no solution x=x+1", "(solve '(= x (+ x 1)) 'x)", "none"),
    ("linear identity x=x", "(solve '(= x x) 'x)", "all"),
    ("quadratic x^2-5x+6", "(solve '(= (+ (^ x 2) (* -5 x) 6) 0) 'x)", "((x . 2) (x . 3))"),
    ("quadratic x^2-9", "(solve '(= (^ x 2) 9) 'x)", "((x . -3) (x . 3))"),
    ("quadratic double root", "(solve '(= (+ (^ x 2) (* -4 x) 4) 0) 'x)", "((x . 2))"),
    ("quadratic rational roots 2x^2-x-1", "(solve '(= (+ (* 2 (^ x 2)) (* -1 x) -1) 0) 'x)", "((x . -1/2) (x . 1))"),
    ("quadratic factored (x-1)(x+2)", "(solve '(= (* (- x 1) (+ x 2)) 0) 'x)", "((x . -2) (x . 1))"),
    ("system 2x+y=5, x-3y=-8", "(solve-system '((= (+ (* 2 x) y) 5) (= (- x (* 3 y)) -8)) '(x y))", "((x . 1) (y . 3))"),
    ("system x+y=10, x-y=2", "(solve-system '((= (+ x y) 10) (= (- x y) 2)) '(x y))", "((x . 6) (y . 4))"),
    ("system fractional 2x+4y=1, 3x+y=1", "(solve-system '((= (+ (* 2 x) (* 4 y)) 1) (= (+ (* 3 x) y) 1)) '(x y))", "((x . 3/10) (y . 1/10))"),
    ("system 3x3", "(solve-system '((= (+ x y z) 6) (= (+ (* 2 x) y (- z)) 1) (= (+ x (* -1 y) (* 2 z)) 5)) '(x y z))", "((x . 1) (y . 2) (z . 3))"),
    ("system needs row swap", "(solve-system '((= y 2) (= (+ x y) 5)) '(x y))", "((x . 3) (y . 2))"),
    ("system singular", "(solve-system '((= (+ x y) 1) (= (+ (* 2 x) (* 2 y)) 2)) '(x y))", "singular"),
]


def run_tests(arch: str = "rv32") -> int:
    elf_path = REPO_ROOT / "build" / arch / "lugalos.elf"
    img_path = REPO_ROOT / "build" / arch / "lugalos_sd.img"
    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/{arch}' first.")
        return 1

    arch_img = img_path.with_name(f"test_{arch}_cas_solve_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.5] Testing CAS Solver ({arch}) using {elf_path}...")
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
            '(load "/sd0/cas/simplify.lisp")\n'
            '(load "/sd0/cas/poly.lisp")\n'
            '(load "/sd0/cas/calculus.lisp")\n'
            '(load "/sd0/cas/solve.lisp")\n'
            + "".join(expr + "\n" for _, expr, _ in CASES)
            + '(display "SOLVE_TESTS_DONE\\n")\n'
            + "exit\n"
        )
        ok, log = session.send_and_expect(script, r"SOLVE_TESTS_DONE", timeout=60.0)
        if not ok:
            print("Failed waiting for SOLVE_TESTS_DONE. Captured log:\n", log)
            return 1

        print("\n--- Test Results ---")
        pos = 0
        for name, _, expected in CASES:
            needle = f"=> {expected}\n"
            idx = log.replace("\r", "").find(needle, pos)
            if idx != -1:
                print(f"  [PASS] {name}")
                passed += 1
                pos = idx + len(needle)
            else:
                print(f"  [FAIL] {name}: expected '{expected}'")
        print(f"\nTotal: {passed}/{len(CASES)} passed.")
    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    return 0 if passed == len(CASES) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test CAS Solver")
    parser.add_argument("--arch", choices=["rv32", "rv64"], default="rv32")
    sys.exit(run_tests(parser.parse_args().arch))
