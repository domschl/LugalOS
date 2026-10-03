#!/usr/bin/env python3
"""Phase 43.2 Test Suite: CAS Symbolic Expression Canonicalization & Pattern Simplifier.

Verifies canonical representation, associative flattening, algebraic identity rules,
like-term and like-factor collection, exact constant folding, radical simplification,
and ordering in QEMU via /sd0/cas/simplify.lisp.
"""

from __future__ import annotations

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

    import shutil

    arch_img = img_path.with_name("test_rv32_cas_simplify_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.2] Testing CAS Simplifier using {elf_path}...")
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
            ";; --- 1. Canonical Ordering & Sorting ---\n"
            "(cas-expr<? 3 'x)\n"
            "(cas-expr<? 'x 'y)\n"
            "(cas-expr<? 'x '(^ x 2))\n"
            "(cas-sort '(3 1 4 2) <)\n"
            "(cas-sort '(y z x) symbol<?)\n"
            ";; --- 2. Associative Flattening ---\n"
            "(simplify '(+ a (+ b c)))\n"
            "(simplify '(* a (* b c)))\n"
            "(simplify '(+ (+ a b) (+ c d)))\n"
            "(simplify '(* (* a b) (* c d)))\n"
            ";; --- 3. Canonical Term Order ---\n"
            "(simplify '(+ y x 3))\n"
            "(simplify '(* y x 3))\n"
            "(simplify '(+ 2 x 3 y 1/2))\n"
            ";; --- 4. Additive Identities & Inverses ---\n"
            "(simplify '(+ x 0))\n"
            "(simplify '(+ 0 x))\n"
            "(simplify '(+ x))\n"
            "(simplify '(+))\n"
            "(simplify '(- x x))\n"
            "(simplify '(- (- x)))\n"
            "(simplify '(- 0 x))\n"
            ";; --- 5. Multiplicative Identities & Inverses ---\n"
            "(simplify '(* x 1))\n"
            "(simplify '(* 1 x))\n"
            "(simplify '(* x 0))\n"
            "(simplify '(* 0 x))\n"
            "(simplify '(* x))\n"
            "(simplify '(*))\n"
            "(simplify '(/ x x))\n"
            ";; --- 6. Exponent Identities ---\n"
            "(simplify '(^ (+ x 1) 0))\n"
            "(simplify '(^ x 0))\n"
            "(simplify '(^ x 1))\n"
            "(simplify '(^ 1 x))\n"
            "(simplify '(^ 0 5))\n"
            ";; --- 7. Like-Term Collection (Addition) ---\n"
            "(simplify '(+ (* 3 x) (* 2 x)))\n"
            "(simplify '(+ x x))\n"
            "(simplify '(- (* 4 x) x))\n"
            "(simplify '(+ (* 3 x) (* -3 x)))\n"
            "(simplify '(+ (* 2 x (^ y 2)) (* 5 x (^ y 2))))\n"
            "(simplify '(+ (* 2 (^ y 2) x) (* 5 x (^ y 2))))\n"
            "(simplify '(+ (* 1/2 x) (* 1/2 x)))\n"
            ";; --- 8. Like-Factor Collection (Multiplication) ---\n"
            "(simplify '(* x x))\n"
            "(simplify '(* x x x))\n"
            "(simplify '(* (^ x 2) (^ x 3)))\n"
            "(simplify '(* (^ x 2) (^ x -2)))\n"
            "(simplify '(* 2 x 3 (^ x 2)))\n"
            "(simplify '(^ (^ x 2) 3))\n"
            "(simplify '(^ (* 2 x) 2))\n"
            ";; --- 9. Exact Constant Folding ---\n"
            "(simplify '(+ 1/2 (* 3 1/4)))\n"
            "(simplify '(^ 2 10))\n"
            "(simplify '(^ 2 -3))\n"
            "(simplify '(^ 2/3 2))\n"
            "(simplify '(/ (* 6 x) 2))\n"
            "(simplify '(/ (* 3 x) x))\n"
            ";; --- 10. Radical Simplifications ---\n"
            "(simplify '(sqrt 16))\n"
            "(simplify '(sqrt 18))\n"
            "(simplify '(sqrt 72))\n"
            "(simplify '(sqrt 100))\n"
            "(simplify '(sqrt 1/4))\n"
            "(simplify '(sqrt 1/2))\n"
            "(simplify '(sqrt (^ x 2)))\n"
            "(simplify '(sqrt (^ x 4)))\n"
            ";; --- 11. Transcendental & Special Functions ---\n"
            "(simplify '(exp 0))\n"
            "(simplify '(log 1))\n"
            "(simplify '(log (exp x)))\n"
            "(simplify '(sin 0))\n"
            "(simplify '(cos 0))\n"
            "(simplify '(tan 0))\n"
            "(simplify '(abs -5))\n"
            "(simplify '(abs (* -1 x)))\n"
            '(display "TEST_PHASE43_2_DONE")\n'
            "(newline)\n"
        )

        ok, log = session.send_and_expect(test_script, r"TEST_PHASE43_2_DONE", timeout=15.0)
        if not ok:
            print("FAILED: Did not receive TEST_PHASE43_2_DONE sentinel.")
            print("Log snippet:\n", log[-2000:] if len(log) > 2000 else log)
            return 1

        checks: list[tuple[str, bool]] = [
            # Ordering & Sorting
            ("cas-expr<? number < symbol", "=> #t" in log),
            ("cas-sort numbers (3 1 4 2 -> (1 2 3 4))", "=> (1 2 3 4)" in log),
            ("cas-sort symbols (y z x -> (x y z))", "=> (x y z)" in log),
            # Associative Flattening
            ("flatten add (+ a (+ b c) -> (+ a b c))", "=> (+ a b c)" in log),
            ("flatten mul (* a (* b c) -> (* a b c))", "=> (* a b c)" in log),
            ("flatten nested add", "=> (+ a b c d)" in log),
            ("flatten nested mul", "=> (* a b c d)" in log),
            # Canonical Term Order
            ("canonical term order (+ y x 3 -> (+ 3 x y))", "=> (+ 3 x y)" in log),
            ("canonical factor order (* y x 3 -> (* 3 x y))", "=> (* 3 x y)" in log),
            ("canonical sum with rational (+ 2 x 3 y 1/2 -> (+ 11/2 x y))", "=> (+ 11/2 x y)" in log),
            # Additive Identities
            ("additive identity (+ x 0 -> x)", "=> x" in log),
            ("additive identity (+ 0 x -> x)", "=> x" in log),
            ("additive identity (+ x -> x)", "=> x" in log),
            ("additive identity (+ -> 0)", "=> 0" in log),
            ("additive inverse (- x x -> 0)", "=> 0" in log),
            ("double negation (- (- x) -> x)", "=> x" in log),
            ("negation (- 0 x -> (* -1 x))", "=> (* -1 x)" in log),
            # Multiplicative Identities
            ("multiplicative identity (* x 1 -> x)", "=> x" in log),
            ("multiplicative identity (* 1 x -> x)", "=> x" in log),
            ("zero annihilation (* x 0 -> 0)", "=> 0" in log),
            ("zero annihilation (* 0 x -> 0)", "=> 0" in log),
            ("multiplicative identity (* x -> x)", "=> x" in log),
            ("multiplicative identity (* -> 1)", "=> 1" in log),
            ("multiplicative inverse (/ x x -> 1)", "=> 1" in log),
            # Exponent Identities
            ("exponent zero power ((x+1)^0 -> 1)", "=> 1" in log),
            ("exponent identity (x^0 -> 1)", "=> 1" in log),
            ("exponent identity (x^1 -> x)", "=> x" in log),
            ("exponent base 1 (1^x -> 1)", "=> 1" in log),
            ("exponent base 0 (0^5 -> 0)", "=> 0" in log),
            # Like-Term Collection (Addition)
            ("like terms 3x + 2x = 5x", "=> (* 5 x)" in log),
            ("like terms x + x = 2x", "=> (* 2 x)" in log),
            ("like terms 4x - x = 3x", "=> (* 3 x)" in log),
            ("like terms 3x - 3x = 0", "=> 0" in log),
            ("multivariate like terms 2xy^2 + 5xy^2 = 7xy^2", "=> (* 7 x (^ y 2))" in log),
            ("multivariate out-of-order 2y^2x + 5xy^2 = 7xy^2", "=> (* 7 x (^ y 2))" in log),
            ("rational like terms 1/2x + 1/2x = x", "=> x" in log),
            # Like-Factor Collection (Multiplication)
            ("like factors x * x = x^2", "=> (^ x 2)" in log),
            ("like factors x * x * x = x^3", "=> (^ x 3)" in log),
            ("like factors x^2 * x^3 = x^5", "=> (^ x 5)" in log),
            ("like factors x^2 * x^-2 = 1", "=> 1" in log),
            ("like factors 2x * 3x^2 = 6x^3", "=> (* 6 (^ x 3))" in log),
            ("power of power (x^2)^3 = x^6", "=> (^ x 6)" in log),
            ("power of product (2x)^2 = 4x^2", "=> (* 4 (^ x 2))" in log),
            # Exact Constant Folding
            ("constant folding (+ 1/2 (* 3 1/4)) = 5/4", "=> 5/4" in log),
            ("constant power 2^10 = 1024", "=> 1024" in log),
            ("constant power 2^-3 = 1/8", "=> 1/8" in log),
            ("constant power (2/3)^2 = 4/9", "=> 4/9" in log),
            ("division folding 6x / 2 = 3x", "=> (* 3 x)" in log),
            ("division cancellation 3x / x = 3", "=> 3" in log),
            # Radicals
            ("exact square root (sqrt 16 -> 4)", "=> 4" in log),
            ("radical factor extraction (sqrt 18 -> 3*sqrt(2))", "=> (* 3 (sqrt 2))" in log),
            ("radical factor extraction (sqrt 72 -> 6*sqrt(2))", "=> (* 6 (sqrt 2))" in log),
            ("exact square root (sqrt 100 -> 10)", "=> 10" in log),
            ("exact rational square root (sqrt 1/4 -> 1/2)", "=> 1/2" in log),
            ("rational radical (sqrt 1/2 -> 1/2*sqrt(2))", "=> (* 1/2 (sqrt 2))" in log),
            ("symbolic radical (sqrt x^2 -> x)", "=> x" in log),
            ("symbolic radical (sqrt x^4 -> x^2)", "=> (^ x 2)" in log),
            # Transcendental & Special
            ("transcendental (exp 0 -> 1)", "=> 1" in log),
            ("transcendental (log 1 -> 0)", "=> 0" in log),
            ("inverse transcendental (log (exp x) -> x)", "=> x" in log),
            ("trigonometric (sin 0 -> 0)", "=> 0" in log),
            ("trigonometric (cos 0 -> 1)", "=> 1" in log),
            ("trigonometric (tan 0 -> 0)", "=> 0" in log),
            ("absolute value (abs -5 -> 5)", "=> 5" in log),
            ("symbolic abs (abs -x -> (abs x))", "=> (abs x)" in log),
        ]

        print("\n--- Test Results ---")
        failed = 0
        for name, passed in checks:
            status = "PASS" if passed else "FAIL"
            if not passed:
                failed += 1
                all_passed = False
            print(f"  [{status}] {name}")

        print(f"\nTotal: {len(checks) - failed}/{len(checks)} passed.")
        if failed > 0:
            print("\nLog output for debugging:\n", log)

    finally:
        session.close()

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(run_tests())
