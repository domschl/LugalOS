#!/usr/bin/env python3
"""Phase 48 Test Suite: Lisp Language Expansion & Foundations.

Verifies:
- (error ...) builtin primitive and clean unwinding
- (sleep ...) and (sleep-ms ...) primitives
- (define-macro ...) / (defmacro ...) syntactic macro engine
- (macro? ...) predicate
- Non-evaluating short-circuiting control macros (when, unless)
"""

from __future__ import annotations

import re
import sys
import time
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
    arch_img = img_path.with_name("test_rv32_expansion_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 48] Testing Lisp Language Expansion using {elf_path}...")
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
            ";; --- 1. Error Primitive ---\n"
            "(error \"simulated error\")\n"
            "(+ 1 2)\n"
            ";; --- 2. Sleep Primitives ---\n"
            "(sleep 5)\n"
            "(sleep-ms 5)\n"
            ";; --- 3. Macro Predicate & Definition ---\n"
            "(macro? +)\n"
            "(macro? (lambda (x) x))\n"
            "(define-macro (when-test cond-val . body) (list 'if cond-val (cons 'begin body) #f))\n"
            "(macro? when-test)\n"
            ";; --- 4. Macro Expansion & Non-evaluation ---\n"
            "(when-test #t (+ 10 20))\n"
            "(when-test #f (error \"should never evaluate\"))\n"
            "(defmacro (unless-test cond-val . body) (list 'if cond-val #f (cons 'begin body)))\n"
            "(macro? unless-test)\n"
            "(unless-test #f (+ 30 40))\n"
            "(unless-test #t (error \"should never evaluate\"))\n"
            ";; --- 5. Boot Macros from stdlib ---\n"
            "(when (= 1 1) (+ 100 200))\n"
            "(unless (= 1 2) (+ 300 400))\n"
            ";; --- 6. Require Macros Module ---\n"
            "(require 'macros)\n"
            "(let ((count 0)) (dotimes (i 5) (inc! count i)) count)\n"
            ";; --- 7. Require Seq Module ---\n"
            "(require 'seq)\n"
            "(fold-left + 0 '(1 2 3 4 5))\n"
            "(range 0 5)\n"
            "(iota 4 10 2)\n"
            "(take '(a b c d e) 3)\n"
            "(drop '(a b c d e) 3)\n"
            ";; --- 8. Require Str Module ---\n"
            "(require 'str)\n"
            "(string-split \"apple,banana,orange\" \",\")\n"
            "(string-join '(\"foo\" \"bar\" \"baz\") \"-\")\n"
            "(string-trim \"   hello lugal   \")\n"
            ";; --- 9. Require Dict Module ---\n"
            "(require 'dict)\n"
            "(alist-ref '((a . 1) (b . 2)) 'b)\n"
            "(plist-get '(name \"lugal\" arch \"rv32\") 'arch)\n"
            ";; --- 10. Raw I2C Bus Primitives ---\n"
            "(i2c-read 85 0 2)\n"
            "(i2c-write 85 0 '(1 2))\n"
            ";; --- 11. Vectors (48.4b) ---\n"
            "(vector? #(1 2 3))\n"
            "(vector? '(1 2 3))\n"
            "(make-vector 3 42)\n"
            "(vector 10 20 30)\n"
            "(vector-length #(10 20 30))\n"
            "(vector-ref #(10 20 30) 1)\n"
            "(let ((v (vector 1 2 3))) (vector-set! v 1 99) v)\n"
            "(vector->list #(1 2 3))\n"
            "(list->vector '(4 5 6))\n"
            ";; --- 12. GPIO, ADC, Timers & RTC (48.2b-d) ---\n"
            "(gpio-mode 0 'out)\n"
            "(gpio-set 0 1)\n"
            "(gpio-get 0)\n"
            "(> (time-us) 0)\n"
            "(number? (adc-read 0))\n"
            "(pair? (rtc-status))\n"
            ";; --- 13. CAS Taylor Series (48.3a) ---\n"
            "(load \"/sd0/cas/simplify.lisp\")\n"
            "(load \"/sd0/cas/poly.lisp\")\n"
            "(load \"/sd0/cas/calculus.lisp\")\n"
            "(taylor '(exp x) 'x 4 0)\n"
            "(taylor '(sin x) 'x 3 0)\n"
            ";; --- 14. CAS Trig & Exp Simplification (48.3b) ---\n"
            "(simplify '(+ (^ (sin x) 2) (^ (cos x) 2)))\n"
            "(simplify '(* (exp a) (exp b)))\n"
            "(simplify '(^ (exp x) 2))\n"
            "(simplify '(exp (log x)))\n"
            "(simplify '(sin (* -1 x)))\n"
            "(simplify '(cos (* -1 x)))\n"
            "(trig-simplify '(sin (* 2 x)))\n"
            "(trig-simplify '(+ 1 (^ (tan x) 2)))\n"
            "(trig-simplify '(- (^ (cosh x) 2) (^ (sinh x) 2)))\n"
            ";; --- 15. Exact Matrix Algebra (48.3c) ---\n"
            "(require 'matrix)\n"
            "(matrix-add '((1 2) (3 4)) '((10 20) (30 40)))\n"
            "(matrix-mul '((1 2) (3 4)) '((2 0) (1 2)))\n"
            "(matrix-transpose '((1 2 3) (4 5 6)))\n"
            "(matrix-det '((1 2) (3 4)))\n"
            "(matrix-inv '((1 2) (3 4)))\n"
            "(matrix-solve '((1 2) (3 4)) '(5 11))\n"
            "(vector-dot '(1 2 3) '(4 5 6))\n"
            "(vector-cross '(1 0 0) '(0 1 0))\n"
            "(display \"TEST_PHASE48_DONE\\n\")\n"
            "exit\n"
        )

        ok, raw_log = session.send_and_expect(test_script, r"TEST_PHASE48_DONE", timeout=20.0)
        if not ok:
            print("FAILED: Did not receive TEST_PHASE48_DONE sentinel.")
            print("Log snippet:\n", raw_log[-2000:] if len(raw_log) > 2000 else raw_log)
            all_passed = False

        checks = [
            ("error output", r"\[Lisp Error\] simulated error"),
            ("recovery after error (+ 1 2)", r"=> 3"),
            ("sleep 5", r"=> #t"),
            ("sleep-ms 5", r"=> #t"),
            ("macro? primitive false", r"=> #f"),
            ("macro? closure false", r"=> #f"),
            ("define-macro returned name", r"=> when-test"),
            ("macro? when-test true", r"=> #t"),
            ("when-test #t evaluated", r"=> 30"),
            ("when-test #f short-circuit", r"=> #f"),
            ("defmacro returned name", r"=> unless-test"),
            ("macro? unless-test true", r"=> #t"),
            ("unless-test #f evaluated", r"=> 70"),
            ("unless-test #t short-circuit", r"=> #f"),
            ("boot when evaluated", r"=> 300"),
            ("boot unless evaluated", r"=> 700"),
            ("require macros", r"=> #t"),
            ("dotimes and inc! (0+1+2+3+4)", r"=> 10"),
            ("require seq", r"=> #t"),
            ("fold-left + 0 '(1 2 3 4 5)", r"=> 15"),
            ("range 0 5", r"=> \(0 1 2 3 4\)"),
            ("iota 4 10 2", r"=> \(10 12 14 16\)"),
            ("take '(a b c d e) 3", r"=> \(a b c\)"),
            ("drop '(a b c d e) 3", r"=> \(d e\)"),
            ("require str", r"=> #t"),
            ("string-split", r"=> \(\"apple\" \"banana\" \"orange\"\)"),
            ("string-join", r"=> \"foo-bar-baz\""),
            ("string-trim", r"=> \"hello lugal\""),
            ("require dict", r"=> #t"),
            ("alist-ref", r"=> 2"),
            ("plist-get", r"=> \"rv32\""),
            ("i2c-read nak returns #f", r"=> #f"),
            ("i2c-write nak returns #f", r"=> #f"),
            # Vectors
            ("vector? on vector", r"=> #t"),
            ("vector? on list", r"=> #f"),
            ("make-vector", r"=> #\(42 42 42\)"),
            ("vector constructor", r"=> #\(10 20 30\)"),
            ("vector-length", r"=> 3"),
            ("vector-ref", r"=> 20"),
            ("vector-set!", r"=> #\(1 99 3\)"),
            ("vector->list", r"=> \(1 2 3\)"),
            ("list->vector", r"=> #\(4 5 6\)"),
            # GPIO, Timers, ADC, RTC
            ("gpio-mode", r"=> #t"),
            ("gpio-set", r"=> #t"),
            ("gpio-get", r"=> [01]"),
            ("time-us > 0", r"=> #t"),
            ("adc-read is number", r"=> #t"),
            ("rtc-status is pair", r"=> #t"),
            # CAS Taylor series
            ("taylor exp(x)", r"=> \(\+ 1 x \(\* 1/2 \(\^ x 2\)\) \(\* 1/6 \(\^ x 3\)\) \(\* 1/24 \(\^ x 4\)\)\)"),
            ("taylor sin(x)", r"=> \(\+ x \(\* -1/6 \(\^ x 3\)\)\)"),
            # CAS Trig / Exp
            ("sin^2 + cos^2 = 1", r"=> 1"),
            ("exp(a)*exp(b) = exp(a+b)", r"=> \(exp \(\+ a b\)\)"),
            ("(exp(x))^2 = exp(2x)", r"=> \(exp \(\* 2 x\)\)"),
            ("exp(log(x)) = x", r"=> x"),
            ("sin(-x) = -sin(x)", r"=> \(\* -1 \(sin x\)\)"),
            ("cos(-x) = cos(x)", r"=> \(cos x\)"),
            ("trig-simplify sin(2x)", r"=> \(\* 2"),
            ("trig-simplify 1 + tan^2(x)", r"=> \(\^ \(sec x\) 2\)"),
            ("trig-simplify cosh^2(x) - sinh^2(x)", r"=> 1"),
            # Matrix Algebra
            ("matrix-add", r"=> \(\(11 22\) \(33 44\)\)"),
            ("matrix-mul", r"=> \(\(4 4\) \(10 8\)\)"),
            ("matrix-transpose", r"=> \(\(1 4\) \(2 5\) \(3 6\)\)"),
            ("matrix-det", r"=> -2"),
            ("matrix-inv", r"=> \(\(-2 1\) \(3/2 -1/2\)\)"),
            ("matrix-solve", r"=> \(1 2\)"),
            ("vector-dot", r"=> 32"),
            ("vector-cross", r"=> \(0 0 1\)"),
        ]

        print("\n--- Test Results ---")
        for desc, pattern in checks:
            match = re.search(pattern, raw_log)
            if match:
                print(f"  [PASS] {desc}")
            else:
                print(f"  [FAIL] {desc} (expected pattern: {pattern})")
                all_passed = False

        if not all_passed:
            print("\n--- Raw Output ---")
            print(raw_log)

    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(run_tests())
