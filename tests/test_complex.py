#!/usr/bin/env python3
"""Complex Number Support Verification Suite for LugalOS.

Verifies:
1. Layer 2: Core Exact Gaussian Rationals Q(i), literals, arithmetic, and R7RS primitives.
2. Layer 1: Symbolic CAS imaginary unit 'i', powers, negative radicals, Euler's identity.
3. Polynomial solving and factorization over C (cubics, quartics, quadratics).
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

    arch_img = img_path.with_name("test_rv32_complex_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Complex Tests] Testing Complex Numbers using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    try:
        ok, _ = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return 1

        session.send_and_expect("lisp", r"lisp> ", timeout=5.0)

        # List of test expressions and expected substrings in output
        tests: list[tuple[str, str, str]] = [
            # --- 1. Layer 2: Numeric Gaussian Rationals & Literal Parsing ---
            ("Literal 3+4i", "3+4i", "3+4i"),
            ("Literal 3-4i", "3-4i", "3-4i"),
            ("Literal +i", "+i", "+i"),
            ("Literal -i", "-i", "-i"),
            ("Literal 2i", "2i", "+2i"),
            ("Literal -3i", "-3i", "-3i"),
            ("Literal rational 1/2+3/4i", "1/2+3/4i", "1/2+3/4i"),
            ("Complex addition", "(+ 1+2i 3+4i)", "4+6i"),
            ("Complex subtraction", "(- 5+6i 2+3i)", "3+3i"),
            ("Complex multiplication (demotion to real)", "(* 1+1i 1-1i)", "=> 2"),
            ("Complex multiplication conjugate", "(* 3+4i 3-4i)", "=> 25"),
            ("Complex reciprocal", "(/ 1 +i)", "-i"),
            ("Complex division rational", "(/ (+ 1 2i) (+ 1 1i))", "3/2+1/2i"),
            # --- 2. Layer 2: Scheme R7RS Primitives ---
            ("complex? on complex", "(complex? 3+4i)", "=> #t"),
            ("complex? on integer", "(complex? 5)", "=> #t"),
            ("real? on complex", "(real? 3+4i)", "=> #f"),
            ("real? on integer", "(real? 5)", "=> #t"),
            ("make-rectangular", "(make-rectangular 3 4)", "3+4i"),
            ("make-polar real", "(make-polar 5 0)", "=> 5"),
            ("real-part", "(real-part 3+4i)", "=> 3"),
            ("imag-part", "(imag-part 3+4i)", "=> 4"),
            ("magnitude-squared", "(magnitude-squared 3+4i)", "=> 25"),
            ("magnitude", "(magnitude 3+4i)", "=> 5"),
            ("conjugate positive", "(conjugate 3+4i)", "3-4i"),
            ("conjugate negative", "(conjugate 3-4i)", "3+4i"),
            # --- 3. Layer 1: Symbolic CAS Complex Engine ---
            ("CAS loader", '(load "/sd0/cas/cas.lisp")', "=> #t"),
            ("CAS i^2 reduction", "(simplify '(^ i 2))", "=> -1"),
            ("CAS i^3 reduction", "(simplify '(^ i 3))", "=> (* -1 i)"),
            ("CAS i^4 reduction", "(simplify '(^ i 4))", "=> 1"),
            ("CAS i^5 reduction", "(simplify '(^ i 5))", "=> i"),
            ("CAS sqrt(-1)", "(simplify '(sqrt -1))", "=> i"),
            ("CAS sqrt(-4)", "(simplify '(sqrt -4))", "=> (* 2 i)"),
            ("CAS sqrt(-9)", "(simplify '(sqrt -9))", "=> (* 3 i)"),
            ("CAS sqrt(-2)", "(simplify '(sqrt -2))", "=> (* i (sqrt 2))"),
            ("CAS 1/i", "(simplify '(/ 1 i))", "=> (* -1 i)"),
            ("CAS Euler identity e^(i*pi)", "(simplify '(exp (* i pi)))", "=> -1"),
            ("CAS Euler identity e^(pi*i)", "(simplify '(exp (* pi i)))", "=> -1"),
            ("CAS real-part", "(cas-real-part '(+ 3 (* 4 i)))", "=> 3"),
            ("CAS imag-part", "(cas-imag-part '(+ 3 (* 4 i)))", "=> 4"),
            ("CAS complex-abs", "(cas-complex-abs '(+ 3 (* 4 i)))", "=> 5"),
            # --- 4. Layer 1: Polynomial Solving & Factorization over C ---
            ("Solve x^2 + 1 = 0", "(solve '(= (+ (^ x 2) 1) 0) 'x)", "((x * -1 i) (x . i))"),
            ("Solve x^2 + 4 = 0", "(solve '(= (+ (^ x 2) 4) 0) 'x)", "((x * -2 i) (x * 2 i))"),
            ("Solve x^2 + 2x + 2 = 0", "(solve '(= (+ (^ x 2) (* 2 x) 2) 0) 'x)", "((x + -1 (* -1 i)) (x + -1 i))"),
            ("Solve x^4 - 1 = 0", "(solve '(= (+ (^ x 4) -1) 0) 'x)", "((x . 1) (x . -1) (x . i) (x * -1 i))"),
            ("Factor x^2 + 1 over C", "(poly-factor-complex '(+ (^ x 2) 1) 'x)", "(* (+ i x) (+ (* -1 i) x))"),
            ("Factor x^2 + 4 over C", "(poly-factor-complex '(+ (^ x 2) 4) 'x)", "(* (+ (* 2 i) x) (+ (* -2 i) x))"),
            ("Factor x^4 - 1 over C", "(poly-factor-complex '(+ (^ x 4) -1) 'x)", "(* (+ -1 x) (+ 1 x) (+ (* -1 i) x) (+ i x))"),
            ("Factor x^2 + 2x + 2 over C", "(poly-factor-complex '(+ (^ x 2) (* 2 x) 2) 'x)", "(* (+ 1 i x) (+ 1 (* -1 i) x))"),
            ("Factor 3(x^2 + 9) over C", "(poly-factor-complex '(* 3 (+ (^ x 2) 9)) 'x)", "(* (+ (* 3 i) x) (+ (* -3 i) x) 3)"),
            # --- 5. Complex Linear Algebra & Matrices ---
            ("Matrix conjugate", "(matrix-conjugate '((1+2i 3) (4-5i 6i)))", "((1-2i 3) (4+5i -6i))"),
            ("Matrix adjoint", "(matrix-adjoint '((1+2i 3) (4-5i 6i)))", "((1-2i 4+5i) (3 -6i))"),
            ("Matrix trace", "(matrix-trace '((1+2i 3) (4-5i 6i)))", "1+8i"),
            ("Vector conjugate", "(vector-conjugate '(1+i 2-i))", "(1-i 2+i)"),
            ("Hermitian dot", "(vector-hermitian-dot '(1+i 2) '(1+i 3))", "8"),
            ("Hermitian matrix test #t", "(matrix-hermitian? '((2 1-i) (1+i 3)))", "#t"),
            ("Hermitian matrix test #f", "(matrix-hermitian? '((2 1-i) (1-i 3)))", "#f"),
            ("Unitary Pauli-Y test", "(matrix-unitary? '((0 (* -1 i)) (i 0)))", "#t"),
            ("Unitary non-unitary test", "(matrix-unitary? '((1 1) (0 1)))", "#f"),
            ("Complex matrix inverse", "(matrix-inv '((1 i) (0 1)))", "((1 (* -1 i)) (0 1))"),
            # --- 6. Milestone 48.3b: Complex Trig & Exp Simplification ---
            ("sinh(0)", "(simplify '(sinh 0))", "=> 0"),
            ("cosh(0)", "(simplify '(cosh 0))", "=> 1"),
            ("tanh(0)", "(simplify '(tanh 0))", "=> 0"),
            ("sinh odd symmetry", "(simplify '(sinh (* -1 x)))", "(* -1 (sinh x))"),
            ("cosh even symmetry", "(simplify '(cosh (* -1 x)))", "(cosh x)"),
            ("Euler relation sin(i*x)", "(simplify '(sin (* i x)))", "(* i (sinh x))"),
            ("Euler relation cos(i*x)", "(simplify '(cos (* i x)))", "(cosh x)"),
            ("Euler relation tan(i*x)", "(simplify '(tan (* i x)))", "(* i (tanh x))"),
            ("Euler relation sinh(i*x)", "(simplify '(sinh (* i x)))", "(* i (sin x))"),
            ("Euler relation cosh(i*x)", "(simplify '(cosh (* i x)))", "(cos x)"),
            ("Euler relation tanh(i*x)", "(simplify '(tanh (* i x)))", "(* i (tan x))"),
            ("Euler identity e^(i*pi/2)", "(simplify '(exp (* 1/2 (* i pi))))", "=> i"),
            ("Euler identity e^(-i*pi/2)", "(simplify '(exp (* -1/2 (* i pi))))", "(* -1 i)"),
            ("Euler identity e^(2*i*pi)", "(simplify '(exp (* 2 (* i pi))))", "=> 1"),
            ("Cartesian exp split e^(1+i*pi)", "(simplify '(exp (+ 1 (* i pi))))", "(* -1 (exp 1))"),
            ("Complex log ln(-1)", "(simplify '(log -1))", "(* i pi)"),
            ("Complex log ln(i)", "(simplify '(log i))", "(* 1/2 i pi)"),
            ("Complex log ln(-i)", "(simplify '(log (* -1 i)))", "(* -1/2 i pi)"),
            ("Complex log ln(-5)", "(simplify '(log -5))", "(* i pi)"),
            ("Complex log ln(2i)", "(simplify '(log (* 2 i)))", "(* 1/2 i pi)"),
            ("Complex power i^i", "(simplify '(^ i i))", "(exp (* -1/2 pi))"),
            ("Complex power i^(-i)", "(simplify '(^ i (* -1 i)))", "(exp (* 1/2 pi))"),
            ("Diff sinh", "(diff '(sinh x) 'x)", "(cosh x)"),
            ("Diff cosh", "(diff '(cosh x) 'x)", "(sinh x)"),
            ("Diff tanh", "(diff '(tanh x) 'x)", "(tanh x)"),
            ("to-exp cos", "(to-exp '(cos x))", "(exp (* i x))"),
            ("to-trig exp(i*x)", "(to-trig '(exp (* i x)))", "(cos x)"),
            # --- 7. Milestone 48.3a: Complex Power Series & Laurent Expansions ---
            ("Taylor x^3 around i", "(taylor '(^ x 3) 'x 3 'i)", "(* -1 i)"),
            ("Taylor removable sin(x)/x at 0", "(taylor '(/ (sin x) x) 'x 3 0)", "(+ 1 (* -1/6 (^ x 2)))"),
            ("Laurent exp(x)/x at 0 pole order 1", "(taylor '(/ (exp x) x) 'x 2 0)", "(^ x -1)"),
            ("Laurent 1/(x^2+1) at i complex pole", "(taylor '(/ 1 (+ (^ x 2) 1)) 'x 1 'i)", "(* -1 i)"),
            ("Infix Laurent exp(x)/x", "(to-infix (taylor '(/ (exp x) x) 'x 2 0))", "1/x"),
        ]

        print("\n--- Running Complex Tests ---")
        failed = 0
        for name, cmd, expected in tests:
            ok, out = session.send_and_expect(cmd, r"lisp> ", timeout=12.0)
            passed = ok and expected in out
            status = "PASS" if passed else "FAIL"
            if not passed:
                failed += 1
                all_passed = False
                print(f"  [{status}] {name}")
                print(f"        Sent:     {cmd}")
                print(f"        Expected: {expected}")
                print(f"        Got:      {out.strip()}")
            else:
                print(f"  [{status}] {name}")

        print(f"\nTotal: {len(tests) - failed}/{len(tests)} passed.")

    finally:
        session.close()

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(run_tests())
