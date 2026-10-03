#!/usr/bin/env python3
"""tests/test_cas_poly.py -- Comprehensive verification suite for Phase 43.3
Polynomial Algebra, Expansion, Inspection & Evaluation (cas/poly.lisp).

Runs in QEMU (rv32 / rv64) non-destructively, validating exact mathematical results.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]


def run_tests() -> int:
    elf_path = REPO_ROOT / "build" / "rv32" / "lugalos.elf"
    img_path = REPO_ROOT / "build" / "rv32" / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/rv32' first.")
        return 1

    arch_img = img_path.with_name("test_rv32_cas_poly_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.3] Testing CAS Polynomial Engine using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    try:
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return 1

        test_script = (
            "lisp\n"
            '(load "/sd0/cas/simplify.lisp")\n'
            '(load "/sd0/cas/poly.lisp")\n'
            ";; --- 1. Distributive Expansion ---\n"
            "(expand '(* (+ x 1) (+ x 2)))\n"
            "(expand '(* (+ x 1) (+ x -1)))\n"
            "(expand '(* 2 (+ x 3)))\n"
            "(expand '(* (+ x y) (+ x (* -1 y))))\n"
            "(expand '(* (+ x 1) (+ x 2) (+ x 3)))\n"
            "(expand '(* 3 (+ 2 4)))\n"
            "(expand '(* 0 (+ x 1)))\n"
            "(expand '(* 1 (+ x 1)))\n"
            ";; --- 2. Powers & Binomial Expansion ---\n"
            "(expand '(^ (+ x 1) 0))\n"
            "(expand '(^ (+ x 1) 1))\n"
            "(expand '(^ (+ x 1) 2))\n"
            "(expand '(^ (+ x 1) 3))\n"
            "(expand '(^ (+ x 1) 4))\n"
            "(expand '(^ (+ x -1) 2))\n"
            "(expand '(^ (+ x y) 2))\n"
            "(expand '(^ (* 2 x) 3))\n"
            ";; --- 3. Polynomial Degree Inspection ---\n"
            "(poly-degree 42 'x)\n"
            "(poly-degree 'y 'x)\n"
            "(poly-degree 'x 'x)\n"
            "(poly-degree '(^ x 5) 'x)\n"
            "(poly-degree '(* 3 (^ x 4)) 'x)\n"
            "(poly-degree '(+ (^ x 3) (* 2 (^ x 5)) 7) 'x)\n"
            "(poly-degree '(* (+ x 1) (+ x 2)) 'x)\n"
            "(poly-degree '(* (^ x 2) (^ y 3)) 'x)\n"
            "(poly-degree '(* (^ x 2) (^ y 3)) 'y)\n"
            ";; --- 4. Polynomial Coefficients ---\n"
            "(poly-coeffs 42 'x)\n"
            "(poly-coeffs 'x 'x)\n"
            "(poly-coeffs '(^ x 3) 'x)\n"
            "(poly-coeffs '(+ (^ x 2) (* 3 x) 2) 'x)\n"
            "(poly-coeffs '(+ (^ x 3) -1) 'x)\n"
            "(poly-coeffs '(+ (* 4 (^ x 2)) (* -5 x) 6) 'x)\n"
            "(poly-coeffs '(+ (* a (^ x 2)) (* b x) c) 'x)\n"
            ";; --- 5. Leading Coefficient ---\n"
            "(poly-lead-coeff '(+ (* 7 (^ x 3)) (* 2 x)) 'x)\n"
            "(poly-lead-coeff '(+ (* a (^ x 2)) b) 'x)\n"
            "(poly-lead-coeff 99 'x)\n"
            ";; --- 6. Horner's Rule & Evaluation ---\n"
            "(poly-eval-horner '(1 3 2) 5)\n"
            "(poly-eval-horner '(2 -4 1) 3)\n"
            "(poly-eval-horner '(1 0 0 -8) 2)\n"
            "(poly-eval-horner '(2 1) 1/2)\n"
            "(poly-eval '(+ (^ x 2) (* 3 x) 2) '((x . 5)))\n"
            "(poly-eval '(+ (* x y) 3) '((x . 4) (y . 5)))\n"
            ";; --- 7. Polynomial Long Division ---\n"
            "(poly-div '(+ (^ x 2) (* 3 x) 2) '(+ x 1) 'x)\n"
            "(poly-div '(+ (^ x 3) (* -2 (^ x 2)) -4) '(+ x -3) 'x)\n"
            "(poly-div '(+ (^ x 2) 1) '(+ x 1) 'x)\n"
            "(poly-div '(+ (^ x 2) 4) '(+ (^ x 2) 1) 'x)\n"
            ";; --- 8. High-Level Aliases ---\n"
            "(poly-add '(+ (^ x 2) x) '(+ (* 2 x) 1))\n"
            "(poly-mul '(+ x 1) '(+ x 2))\n"
            '(display "TEST_PHASE43_3_DONE")\n'
            "(newline)\n"
        )

        ok, log = session.send_and_expect(test_script, r"TEST_PHASE43_3_DONE", timeout=15.0)
        if not ok:
            print("FAILED: Did not receive TEST_PHASE43_3_DONE sentinel.")
            print("Log snippet:\n", log[-2000:] if len(log) > 2000 else log)
            return 1

        checks: list[tuple[str, bool]] = [
            # Distributive Expansion
            ("linear product (x+1)(x+2)", "=> (+ 2 (* 3 x) (^ x 2))" in log),
            ("difference of squares (x+1)(x-1)", "=> (+ -1 (^ x 2))" in log),
            ("scalar distribution 2(x+3)", "=> (+ 6 (* 2 x))" in log),
            ("multivariate diff of squares (x+y)(x-y)", "=> (+ (^ x 2) (* -1 (^ y 2)))" in log),
            ("three linear factors (x+1)(x+2)(x+3)", "=> (+ 6 (* 11 x) (* 6 (^ x 2)) (^ x 3))" in log),
            ("expansion with constants (* 3 (+ 2 4))", "=> 18" in log),
            ("expansion zero multiplication (* 0 (+ x 1))", "=> 0" in log),
            ("expansion identity (* 1 (+ x 1))", "=> (+ 1 x)" in log),

            # Powers & Binomial Expansion
            ("power zero ((x+1)^0)", "=> 1" in log),
            ("power one ((x+1)^1)", "=> (+ 1 x)" in log),
            ("binomial square ((x+1)^2)", "=> (+ 1 (* 2 x) (^ x 2))" in log),
            ("binomial cube ((x+1)^3)", "=> (+ 1 (* 3 x) (* 3 (^ x 2)) (^ x 3))" in log),
            ("binomial quartic ((x+1)^4)", "=> (+ 1 (* 4 x) (* 6 (^ x 2)) (* 4 (^ x 3)) (^ x 4))" in log),
            ("binomial difference square ((x-1)^2)", "=> (+ 1 (* -2 x) (^ x 2))" in log),
            ("bivariate binomial square ((x+y)^2)", "=> (+ (* 2 x y) (^ x 2) (^ y 2))" in log),
            ("monomial cube ((2x)^3)", "=> (* 8 (^ x 3))" in log),

            # Polynomial Degree Inspection
            ("degree of constant (42)", "=> 0" in log),
            ("degree of other variable (y in x)", "=> 0" in log),
            ("degree of linear variable (x)", "=> 1" in log),
            ("degree of power (x^5)", "=> 5" in log),
            ("degree of monomial (3x^4)", "=> 4" in log),
            ("degree of polynomial (x^3 + 2x^5 + 7)", "=> 5" in log),
            ("degree of unexpanded product (x+1)(x+2)", "=> 2" in log),
            ("degree multivariate x^2*y^3 in x", "=> 2" in log),
            ("degree multivariate x^2*y^3 in y", "=> 3" in log),

            # Polynomial Coefficients
            ("coeffs of constant 42", "=> (42)" in log),
            ("coeffs of variable x", "=> (1 0)" in log),
            ("coeffs of pure power x^3", "=> (1 0 0 0)" in log),
            ("coeffs of quadratic x^2 + 3x + 2", "=> (1 3 2)" in log),
            ("coeffs with missing terms x^3 - 1", "=> (1 0 0 -1)" in log),
            ("coeffs with negative values 4x^2 - 5x + 6", "=> (4 -5 6)" in log),
            ("coeffs symbolic a*x^2 + b*x + c", "=> (a b c)" in log),

            # Leading Coefficient
            ("lead coeff of 7x^3 + 2x", "=> 7" in log),
            ("lead coeff of symbolic a*x^2 + b", "=> a" in log),
            ("lead coeff of constant 99", "=> 99" in log),

            # Horner's Rule & Evaluation
            ("Horner (1 3 2) at 5 = 42", "=> 42" in log),
            ("Horner (2 -4 1) at 3 = 7", "=> 7" in log),
            ("Horner (1 0 0 -8) at 2 = 0", "=> 0" in log),
            ("Horner rational (2 1) at 1/2 = 2", "=> 2" in log),
            ("poly-eval x^2 + 3x + 2 at x=5", "=> 42" in log),
            ("poly-eval bivariate x*y + 3 at x=4 y=5", "=> 23" in log),

            # Polynomial Long Division
            ("exact division (x^2+3x+2)/(x+1)", "=> ((+ 2 x) 0)" in log),
            ("division with remainder (x^3-2x^2-4)/(x-3)", "=> ((+ 3 x (^ x 2)) 5)" in log),
            ("division with remainder (x^2+1)/(x+1)", "=> ((+ -1 x) 2)" in log),
            ("division constant remainder (x^2+4)/(x^2+1)", "=> (1 3)" in log),

            # High-Level Aliases
            ("poly-add (x^2+x) + (2x+1)", "=> (+ 1 (* 3 x) (^ x 2))" in log),
            ("poly-mul (x+1)*(x+2)", "=> (+ 2 (* 3 x) (^ x 2))" in log),
        ]

        print("\n--- Test Results ---")
        passed = 0
        for name, ok_check in checks:
            if ok_check:
                passed += 1
                print(f"  [PASS] {name}")
            else:
                all_passed = False
                print(f"  [FAIL] {name}")

        print(f"\nTotal: {passed}/{len(checks)} passed.")

    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(run_tests())
