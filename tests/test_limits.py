#!/usr/bin/env python3
"""Symbolic Limits & Limes Verification Suite for LugalOS CAS.

Milestone 48.3f:
Verifies:
1. Continuous direct substitution.
2. Removable algebraic roots via polynomial factor cancellation and division.
3. Classic 0/0 indeterminate forms via L'Hôpital's rule and power series:
   - sin(x)/x -> 1
   - sin(3x)/x -> 3
   - (1 - cos(x))/x^2 -> 1/2
   - (x - sin(x))/x^3 -> 1/6
   - (exp(x) - 1)/x -> 1
   - ln(1 + x)/x -> 1
   - ln(x)/(x - 1) -> 1
   - tan(x)/x -> 1
4. Limits at +infinity and -infinity for rational functions and degrees.
5. Directional limits and vertical asymptote poles (right, left, two-sided undefined/infinity).
6. German mathematical alias (limes expr var point [dir]).
7. Infix command integration: calc("limit(...)").
"""

from __future__ import annotations

import sys
import shutil
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

    arch_img = img_path.with_name("test_rv32_limits_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Limits Tests] Testing Symbolic Limits using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    try:
        ok, _ = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return 1

        session.send_and_expect("lisp", r"lisp> ", timeout=5.0)

        # Load CAS suite
        print("Loading CAS calculus & format...")
        session.send_and_expect('(load "/sd0/cas/simplify.lisp")', r"lisp> ", timeout=10.0)
        session.send_and_expect('(load "/sd0/cas/poly.lisp")', r"lisp> ", timeout=10.0)
        session.send_and_expect('(load "/sd0/cas/calculus.lisp")', r"lisp> ", timeout=10.0)
        session.send_and_expect('(load "/sd0/cas/format.lisp")', r"lisp> ", timeout=10.0)

        checks = [
            # 1. Continuous substitution
            ("(limit '(+ (* x x) (* 3 x) 2) 'x 2)", "12", "continuous polynomial"),
            ("(limit '(cos x) 'x 0)", "1", "continuous cos(0)"),
            ("(limit '(/ (+ (* x x) 3) (+ x 1)) 'x 2)", "7/3", "continuous rational"),

            # 2. Removable algebraic roots
            ("(limit '(/ (- (* x x) 1) (- x 1)) 'x 1)", "2", "removable (x^2-1)/(x-1)"),
            ("(limit '(/ (- (^ x 3) 8) (- x 2)) 'x 2)", "12", "removable (x^3-8)/(x-2)"),
            ("(limit '(/ (- (^ x 4) 16) (- x 2)) 'x 2)", "32", "removable (x^4-16)/(x-2)"),

            # 3. L'Hopital transcendental 0/0
            ("(limit '(/ (sin x) x) 'x 0)", "1", "sin(x)/x at 0"),
            ("(limit '(/ (sin (* 3 x)) x) 'x 0)", "3", "sin(3x)/x at 0"),
            ("(limit '(/ (- 1 (cos x)) (^ x 2)) 'x 0)", "1/2", "(1 - cos(x))/x^2 at 0"),
            ("(limit '(/ (- x (sin x)) (^ x 3)) 'x 0)", "1/6", "(x - sin(x))/x^3 at 0"),
            ("(limit '(/ (- (exp x) 1) x) 'x 0)", "1", "(exp(x)-1)/x at 0"),
            ("(limit '(/ (log (+ 1 x)) x) 'x 0)", "1", "ln(1+x)/x at 0"),
            ("(limit '(/ (log x) (- x 1)) 'x 1)", "1", "ln(x)/(x-1) at 1"),
            ("(limit '(/ (tan x) x) 'x 0)", "1", "tan(x)/x at 0"),

            # 4. Limits at infinity
            ("(limit '(/ (+ (* 3 (^ x 2)) (* 5 x)) (+ (* 2 (^ x 2)) -7)) 'x 'infinity)", "3/2", "deg 2 / deg 2 at +inf"),
            ("(limit '(/ (+ x 1) (+ (^ x 2) 2)) 'x 'inf)", "0", "deg 1 / deg 2 at inf"),
            ("(limit '(/ (+ (^ x 3) 1) (+ (^ x 2) 2)) 'x '+inf)", "infinity", "deg 3 / deg 2 at +inf"),
            ("(limit '(/ (+ (^ x 3) 1) (+ (^ x 2) 2)) 'x '-infinity)", "-infinity", "deg 3 / deg 2 at -inf (odd power)"),
            ("(limit '(/ (+ (^ x 4) 1) (+ (^ x 2) 2)) 'x '-inf)", "infinity", "deg 4 / deg 2 at -inf (even power)"),

            # 5. Directional limits & poles
            ("(limit '(/ 1 x) 'x 0 'right)", "infinity", "1/x at 0+"),
            ("(limit '(/ 1 x) 'x 0 'left)", "-infinity", "1/x at 0-"),
            ("(limit '(/ 1 x) 'x 0)", "undefined", "1/x two-sided undefined"),
            ("(limit '(/ 1 (^ x 2)) 'x 0)", "infinity", "1/x^2 two-sided infinity"),
            ("(limit '(/ -1 (^ x 2)) 'x 0)", "-infinity", "-1/x^2 two-sided -infinity"),
            ("(limit '(/ 1 (- x 2)) 'x 2 'right)", "infinity", "1/(x-2) at 2+"),
            ("(limit '(/ 1 (- x 2)) 'x 2 'left)", "-infinity", "1/(x-2) at 2-"),
            ("(limit '(/ 1 (- x 2)) 'x 2)", "undefined", "1/(x-2) two-sided undefined"),

            # 6. German mathematical alias limes
            ("(limes '(/ (sin x) x) 'x 0)", "1", "limes alias sin(x)/x"),
            ("(limes '(/ (- (* x x) 9) (- x 3)) 'x 3)", "6", "limes alias (x^2-9)/(x-3)"),

            # 7. Infix parser integration
            ('(calc "limit(sin(x) / x, x, 0)")', "1", 'infix calc "limit(sin(x)/x, x, 0)"'),
            ('(calc "limit((x^2 - 1)/(x - 1), x, 1)")', "2", 'infix calc "limit((x^2-1)/(x-1), x, 1)"'),
            ('(calc "limit(1/x^2, x, 0)")', "infinity", 'infix calc "limit(1/x^2, x, 0)"'),
        ]

        print(f"\nRunning {len(checks)} limit verification tests...")
        for expr, expected, desc in checks:
            ok, out = session.send_and_expect(expr, r"lisp> ", timeout=5.0)
            if not ok or expected not in out:
                print(f"  [FAIL] {desc}: sent '{expr}', expected '{expected}', got: {out.strip()}")
                all_passed = False
            else:
                print(f"  [PASS] {desc}: {expected}")

    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    if all_passed:
        print("\nAll 30 limit verification tests PASSED!")
        return 0
    else:
        print("\nSome tests FAILED.")
        return 1


if __name__ == "__main__":
    sys.exit(run_tests())
