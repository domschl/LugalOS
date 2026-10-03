#!/usr/bin/env python3
"""tests/test_cas_format.py -- Verification suite for Phase 43.6
Bidirectional infix <-> prefix conversion and math-notation front-end (cas/format.lisp).

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

# (name, lisp expression, expected substring of the console output)
CASES: list[tuple[str, str, str]] = [
    # --- to-infix ---
    ("fmt polynomial", "(to-infix '(+ 1 (* 2 x) (* 3 (^ x 2))))", '=> "3*x^2 + 2*x + 1"'),
    ("fmt sin + x cos", "(to-infix '(+ (* x (cos x)) (sin x)))", '=> "sin(x) + x*cos(x)"'),
    ("fmt subtraction", "(to-infix '(+ x (* -1 y)))", '=> "x - y"'),
    ("fmt negation", "(to-infix '(* -1 x))", '=> "-x"'),
    ("fmt rational coefficient", "(to-infix '(* 1/2 x))", '=> "x/2"'),
    ("fmt negative power", "(to-infix '(^ x -2))", '=> "1/x^2"'),
    ("fmt quotient with sum", "(to-infix '(* 3 (^ (+ x 1) -1)))", '=> "3/(1 + x)"'),
    ("fmt power of sum", "(to-infix '(^ (+ x 1) 2))", '=> "(1 + x)^2"'),
    ("fmt power of power", "(to-infix '(^ (^ x 2) 3))", '=> "(x^2)^3"'),
    ("fmt negative exponent literal", "(to-infix '(^ 2 (* -1 y)))", '=> "2^(-y)"'),
    ("fmt sqrt and ln", "(to-infix '(+ (sqrt x) (log x)))", '=> "ln(x) + sqrt(x)"'),
    ("fmt equation", "(to-infix '(= (+ 4 (* 3 x)) 10))", '=> "3*x + 4 = 10"'),
    ("fmt solution list", "(to-infix '((x . 2) (x . 3)))", '=> "[x = 2, x = 3]"'),
    ("fmt reciprocal", "(to-infix '(* -1 (^ x -1)))", '=> "-1/x"'),
    # --- from-infix ---
    ("parse precedence", '(from-infix "1 + 2*x^2")', "=> (+ 1 (* 2 (^ x 2)))"),
    ("parse implicit mult 3x^2", '(from-infix "3x^2")', "=> (* 3 (^ x 2))"),
    ("parse unary minus -x^2", '(from-infix "-x^2")', "=> (* -1 (^ x 2))"),
    ("parse right-assoc power", '(from-infix "a^b^c")', "=> (^ a (^ b c))"),
    ("parse ** as power", '(from-infix "x**3")', "=> (^ x 3)"),
    ("parse decimal exact", '(from-infix "0.25*x")', "=> (* 1/4 x)"),
    ("parse leading-dot decimal", '(from-infix ".5")', "=> 1/2"),
    ("parse adjacent parens", '(from-infix "(x+1)(x-1)")', "=> (* (+ x 1) (- x 1))"),
    ("parse function call", '(from-infix "sin(2x)")', "=> (sin (* 2 x))"),
    ("parse ln as log", '(from-infix "ln(x)")', "=> (log x)"),
    ("parse split letters xy", '(from-infix "xy")', "=> (* x y)"),
    ("parse name with digit x1", '(from-infix "x1 + 1")', "=> (+ x1 1)"),
    ("parse sinx splits", '(from-infix "xsin(x)")', "=> (* x (sin x))"),
    ("parse equation", '(from-infix "3x + 4 = 10")', "=> (= (+ (* 3 x) 4) 10)"),
    ("parse list", '(from-infix "[a, b]")', "=> (cas-list a b)"),
    ("parse error: dangling +", '(from-infix "3 +")', "parse error: unexpected end of input"),
    ("parse error: bad char", '(from-infix "3 $ 4")', "parse error: unexpected character '$'"),
    ("parse error: missing paren", '(from-infix "(1 + 2")', "parse error: expected ')'"),
    # --- round trip ---
    ("round trip polynomial",
     "(equal? (simplify (from-infix (to-infix '(+ 1 (* 2 x) (* 3 (^ x 2)))))) (simplify '(+ 1 (* 2 x) (* 3 (^ x 2)))))",
     "=> #t"),
    ("round trip quotient",
     "(equal? (simplify (from-infix (to-infix '(* 3 (^ (+ x 1) -1))))) (simplify '(* 3 (^ (+ x 1) -1))))",
     "=> #t"),
    # --- math / calc front-end ---
    ("math diff", '(math "diff(x*sin(x), x)")', "sin(x) + x*cos(x)"),
    ("math diff order 2", '(math "diff(x^3, x, 2)")', "6*x"),
    ("math integrate", '(math "integrate(3x^2 + 4x + 5, x)")', "x^3 + 2*x^2 + 5*x"),
    ("math definite integral", '(math "integrate(x^2, x, 0, 1)")', "1/3"),
    ("math expand", '(math "expand((x+1)^3)")', "x^3 + 3*x^2 + 3*x + 1"),
    ("math solve quadratic", '(math "solve(x^2 - 5x + 6 = 0, x)")', "[x = 2, x = 3]"),
    ("math solve system", '(math "solve([2x + y = 5, x - 3y = -8], [x, y])")', "[x = 1, y = 3]"),
    ("math simplify", '(math "x + x + 2x")', "4*x"),
    ("math nested command", '(math "diff(integrate(x^2, x), x)")', "x^2"),
]


def run_tests(arch: str = "rv32") -> int:
    elf_path = REPO_ROOT / "build" / arch / "lugalos.elf"
    img_path = REPO_ROOT / "build" / arch / "lugalos_sd.img"
    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/{arch}' first.")
        return 1

    arch_img = img_path.with_name(f"test_{arch}_cas_format_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.6] Testing CAS infix front-end ({arch}) using {elf_path}...")
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
            + '(display "FORMAT_TESTS_DONE\\n")\n'
            + "exit\n"
        )
        ok, log = session.send_and_expect(script, r"FORMAT_TESTS_DONE", timeout=120.0)
        if not ok:
            print("Failed waiting for FORMAT_TESTS_DONE. Captured log:\n", log)
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
    parser = argparse.ArgumentParser(description="Test CAS infix front-end")
    parser.add_argument("--arch", choices=["rv32", "rv64"], default="rv32")
    sys.exit(run_tests(parser.parse_args().arch))
