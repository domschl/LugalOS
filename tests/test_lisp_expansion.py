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
            "(display \"TEST_PHASE48_DONE\\n\")\n"
            "exit\n"
        )

        ok, raw_log = session.send_and_expect(test_script, r"TEST_PHASE48_DONE", timeout=12.0)
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
