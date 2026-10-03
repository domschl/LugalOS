#!/usr/bin/env python3
"""tests/test_cas_calculus.py -- Comprehensive verification suite for Phase 43.4
Symbolic Differentiation & Integration (cas/calculus.lisp).

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


def run_tests(arch: str = "rv32") -> int:
    elf_path = REPO_ROOT / "build" / arch / "lugalos.elf"
    img_path = REPO_ROOT / "build" / arch / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/{arch}' first.")
        return 1

    arch_img = img_path.with_name(f"test_{arch}_cas_calc_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.4] Testing CAS Calculus Engine ({arch}) using {elf_path}...")
    session = QemuSession(elf_path, arch_img, arch)
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
            '(load "/sd0/cas/calculus.lisp")\n'
            ";; --- 1. Differentiation Constants & Variables ---\n"
            "(diff 42 'x)\n"
            "(diff 'y 'x)\n"
            "(diff 'x 'x)\n"
            "(diff '(* 3 x) 'x)\n"
            "(diff '(+ x 5) 'x)\n"
            "(diff '(+ (* 3 x) 4) 'x)\n"
            ";; --- 2. Powers & Polynomials ---\n"
            "(diff '(^ x 2) 'x)\n"
            "(diff '(^ x 3) 'x)\n"
            "(diff '(+ (^ x 3) (* 2 (^ x 2)) (* 5 x) 7) 'x)\n"
            "(diff '(^ x -1) 'x)\n"
            "(diff '(^ x 1/2) 'x)\n"
            "(diff '(sqrt x) 'x)\n"
            ";; --- 3. Products & Quotients ---\n"
            "(diff '(* x (sin x)) 'x)\n"
            "(diff '(* x (exp x)) 'x)\n"
            "(diff '(/ 1 x) 'x)\n"
            "(diff '(/ x (+ x 1)) 'x)\n"
            ";; --- 4. Chain Rule & Transcendental ---\n"
            "(diff '(sin (* 2 x)) 'x)\n"
            "(diff '(cos (* 3 x)) 'x)\n"
            "(diff '(tan x) 'x)\n"
            "(diff '(exp (* 2 x)) 'x)\n"
            "(diff '(log x) 'x)\n"
            "(diff '(log (+ x 1)) 'x)\n"
            "(diff '(^ (+ (* 3 x) 1) 4) 'x)\n"
            ";; --- 5. Higher-Order Derivatives ---\n"
            "(diff '(^ x 3) 'x 0)\n"
            "(diff '(^ x 3) 'x 1)\n"
            "(diff '(^ x 3) 'x 2)\n"
            "(diff '(^ x 3) 'x 3)\n"
            "(diff '(^ x 3) 'x 4)\n"
            "(diff '(sin x) 'x 2)\n"
            "(diff '(sin x) 'x 4)\n"
            ";; --- 6. Integration Constants & Powers ---\n"
            "(integrate 5 'x)\n"
            "(integrate 'y 'x)\n"
            "(integrate 'x 'x)\n"
            "(integrate '(* 4 x) 'x)\n"
            "(integrate '(^ x 2) 'x)\n"
            "(integrate '(^ x 3) 'x)\n"
            "(integrate '(^ x -1) 'x)\n"
            "(integrate '(/ 1 x) 'x)\n"
            ";; --- 7. Integration Linearity & Polynomials ---\n"
            "(integrate '(+ (* 3 (^ x 2)) (* 4 x) 5) 'x)\n"
            "(integrate '(* (+ x 1) (+ x 2)) 'x)\n"
            ";; --- 8. Integration Scaled Linear Forms ---\n"
            "(integrate '(/ 1 (+ x 1)) 'x)\n"
            "(integrate '(/ 1 (+ (* 2 x) 3)) 'x)\n"
            "(integrate '(^ (+ x 1) 2) 'x)\n"
            ";; --- 9. Integration Transcendental & Trig ---\n"
            "(integrate '(exp x) 'x)\n"
            "(integrate '(exp (* 3 x)) 'x)\n"
            "(integrate '(sin x) 'x)\n"
            "(integrate '(sin (* 2 x)) 'x)\n"
            "(integrate '(cos x) 'x)\n"
            "(integrate '(cos (* 4 x)) 'x)\n"
            "(integrate '(log x) 'x)\n"
            ";; --- 10. Integration by Parts ---\n"
            "(integrate '(* x (exp x)) 'x)\n"
            "(integrate '(* x (cos x)) 'x)\n"
            "(integrate '(* x (sin x)) 'x)\n"
            ";; --- 11. Inverse Trigonometric ---\n"
            "(integrate '(/ 1 (+ (^ x 2) 1)) 'x)\n"
            "(integrate '(/ 1 (sqrt (- 1 (^ x 2)))) 'x)\n"
            ";; --- 12. Definite Integrals ---\n"
            "(integrate-def '(^ x 2) 'x 0 1)\n"
            "(integrate-def '(+ (* 2 x) 3) 'x 0 2)\n"
            "(integrate-def 'x 'x 1 5)\n"
            ";; --- 13. Fundamental Theorem of Calculus (diff . integrate) ---\n"
            "(diff (integrate '(+ (* 3 (^ x 2)) (* 4 x)) 'x) 'x)\n"
            "(diff (integrate '(cos (* 2 x)) 'x) 'x)\n"
            "(diff (integrate '(exp (* 3 x)) 'x) 'x)\n"
            '(display "CALCULUS_TESTS_DONE\n")\n'
            "exit\n"
        )

        ok, log = session.send_and_expect(test_script, r"CALCULUS_TESTS_DONE", timeout=16.0)
        if not ok:
            print("Failed waiting for CALCULUS_TESTS_DONE. Captured log:\n", log)
            return 1

        expected_results = [
            # 1. Differentiation Constants & Variables
            ("diff constant 42", "=> 0"),
            ("diff independent variable y", "=> 0"),
            ("diff linear x", "=> 1"),
            ("diff monomial 3x", "=> 3"),
            ("diff x + 5", "=> 1"),
            ("diff 3x + 4", "=> 3"),
            # 2. Powers & Polynomials
            ("diff x^2", "=> (* 2 x)"),
            ("diff x^3", "=> (* 3 (^ x 2))"),
            ("diff x^3 + 2x^2 + 5x + 7", "=> (+ 5 (* 4 x) (* 3 (^ x 2)))"),
            ("diff x^-1", "=> (* -1 (^ x -2))"),
            ("diff x^1/2", "=> (* 1/2 (^ x -1/2))"),
            ("diff sqrt(x)", "=> (* 1/2 (^ x -1/2))"),
            # 3. Products & Quotients
            ("diff x * sin(x)", "=> (+ (* x (cos x)) (sin x))"),
            ("diff x * exp(x)", "=> (+ (* x (exp x)) (exp x))"),
            ("diff 1/x", "=> (* -1 (^ x -2))"),
            ("diff x/(x+1)", "=> (^ (+ 1 x) -2)"),
            # 4. Chain Rule & Transcendental
            ("diff sin(2x)", "=> (* 2 (cos (* 2 x)))"),
            ("diff cos(3x)", "=> (* -3 (sin (* 3 x)))"),
            ("diff tan(x)", "=> (+ 1 (^ (tan x) 2))"),
            ("diff exp(2x)", "=> (* 2 (exp (* 2 x)))"),
            ("diff log(x)", "=> (^ x -1)"),
            ("diff log(x+1)", "=> (^ (+ 1 x) -1)"),
            ("diff (3x+1)^4", "=> (* 12 (^ (+ 1 (* 3 x)) 3))"),
            # 5. Higher-Order Derivatives
            ("diff x^3 order 0", "=> (^ x 3)"),
            ("diff x^3 order 1", "=> (* 3 (^ x 2))"),
            ("diff x^3 order 2", "=> (* 6 x)"),
            ("diff x^3 order 3", "=> 6"),
            ("diff x^3 order 4", "=> 0"),
            ("diff sin(x) order 2", "=> (* -1 (sin x))"),
            ("diff sin(x) order 4", "=> (sin x)"),
            # 6. Integration Constants & Powers
            ("integrate 5", "=> (* 5 x)"),
            ("integrate y", "=> (* x y)"),
            ("integrate x", "=> (* 1/2 (^ x 2))"),
            ("integrate 4x", "=> (* 2 (^ x 2))"),
            ("integrate x^2", "=> (* 1/3 (^ x 3))"),
            ("integrate x^3", "=> (* 1/4 (^ x 4))"),
            ("integrate x^-1", "=> (log x)"),
            ("integrate 1/x", "=> (log x)"),
            # 7. Integration Linearity & Polynomials
            ("integrate 3x^2 + 4x + 5", "=> (+ (* 5 x) (* 2 (^ x 2)) (^ x 3))"),
            ("integrate (x+1)(x+2)", "=> (+ (* 2 x) (* 3/2 (^ x 2)) (* 1/3 (^ x 3)))"),
            # 8. Integration Scaled Linear Forms
            ("integrate 1/(x+1)", "=> (log (+ 1 x))"),
            ("integrate 1/(2x+3)", "=> (* 1/2 (log (+ 3 (* 2 x))))"),
            ("integrate (x+1)^2", "=> (* 1/3 (^ (+ 1 x) 3))"),
            # 9. Integration Transcendental & Trig
            ("integrate exp(x)", "=> (exp x)"),
            ("integrate exp(3x)", "=> (* 1/3 (exp (* 3 x)))"),
            ("integrate sin(x)", "=> (* -1 (cos x))"),
            ("integrate sin(2x)", "=> (* -1/2 (cos (* 2 x)))"),
            ("integrate cos(x)", "=> (sin x)"),
            ("integrate cos(4x)", "=> (* 1/4 (sin (* 4 x)))"),
            ("integrate log(x)", "=> (+ (* -1 x) (* x (log x)))"),
            # 10. Integration by Parts
            ("integrate x * exp(x)", "=> (* (+ -1 x) (exp x))"),
            ("integrate x * cos(x)", "=> (+ (* x (sin x)) (cos x))"),
            ("integrate x * sin(x)", "=> (+ (* -1 x (cos x)) (sin x))"),
            # 11. Inverse Trigonometric
            ("integrate 1/(x^2 + 1)", "=> (atan x)"),
            ("integrate 1/sqrt(1 - x^2)", "=> (asin x)"),
            # 12. Definite Integrals
            ("integrate-def x^2 from 0 to 1", "=> 1/3"),
            ("integrate-def 2x+3 from 0 to 2", "=> 10"),
            ("integrate-def x from 1 to 5", "=> 12"),
            # 13. Fundamental Theorem of Calculus
            ("FTC diff(integrate(3x^2+4x))", "=> (+ (* 4 x) (* 3 (^ x 2)))"),
            ("FTC diff(integrate(cos(2x)))", "=> (cos (* 2 x))"),
            ("FTC diff(integrate(exp(3x)))", "=> (exp (* 3 x))"),
        ]

        print("\n--- Test Results ---")
        passed_count = 0
        log_pos = 0

        for name, expected in expected_results:
            found_idx = log.find(expected, log_pos)
            if found_idx != -1:
                print(f"  [PASS] {name}")
                passed_count += 1
                log_pos = found_idx + len(expected)
            else:
                print(f"  [FAIL] {name}: Expected '{expected}'")
                all_passed = False

        print(f"\nTotal: {passed_count}/{len(expected_results)} passed.")

    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    return 0 if (all_passed and passed_count == len(expected_results)) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test CAS Calculus Engine")
    parser.add_argument("--arch", choices=["rv32", "rv64"], default="rv32")
    args = parser.parse_args()
    sys.exit(run_tests(args.arch))
