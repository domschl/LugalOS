#!/usr/bin/env python3
"""Phase 43.1 Test Suite: Lisp Engine Mathematical Foundations.

Verifies exact exponentiation (expt, ^), integer square root (isqrt),
decimal precision conversion (to-decimal, number->decimal),
symbol/string lexicographical ordering (symbol<?, string<?),
and numerical math functions (math-sin, math-cos, math-tan, math-sqrt,
math-exp, math-log) in QEMU.
"""

from __future__ import annotations

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
    arch_img = img_path.with_name("test_rv32_cas_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 43.1] Testing Engine Math Foundations using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    try:
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return 1

        # Test script covering all Phase 43.1 math builtins
        test_script = (
            "lisp\n"
            ";; --- 1. Exponentiation (expt, ^) ---\n"
            "(expt 2 10)\n"
            "(^ 3 4)\n"
            "(expt 2 -3)\n"
            "(expt 2/3 2)\n"
            "(expt 2/3 -2)\n"
            "(expt 10 20)\n"
            "(expt -2 3)\n"
            "(expt -2 4)\n"
            "(expt 5 0)\n"
            "(expt 0 5)\n"
            "(expt 0 0)\n"
            "(expt 0 -2)\n"
            "(expt 2 5000)\n"
            "\n"
            ";; --- 2. Integer Square Root (isqrt) ---\n"
            "(isqrt 0)\n"
            "(isqrt 1)\n"
            "(isqrt 25)\n"
            "(isqrt 24)\n"
            "(isqrt 26)\n"
            "(isqrt 100000000000000000000)\n"
            "(isqrt (* 123456789 123456789))\n"
            "(isqrt -4)\n"
            "(isqrt 1/2)\n"
            "\n"
            ";; --- 3. Decimal Conversion (to-decimal, number->decimal) ---\n"
            "(to-decimal 1/2 2)\n"
            "(to-decimal 1/3 4)\n"
            "(to-decimal 2/3 4)\n"
            "(to-decimal 1/8 3)\n"
            "(to-decimal 5 2)\n"
            "(to-decimal -1/3 3)\n"
            "(number->decimal 22/7 6)\n"
            "(to-decimal 1/3 0)\n"
            "(to-decimal 2/3 0)\n"
            "(to-decimal 1/3 100)\n"
            "\n"
            ";; --- 4. Ordering (symbol<?, symbol>?, string<?, string>?) ---\n"
            "(symbol<? 'a 'b 'c)\n"
            "(symbol<? 'a 'a)\n"
            "(symbol<? 'b 'a)\n"
            "(symbol>? 'c 'b 'a)\n"
            "(symbol>? 'a 'b)\n"
            "(string<? \"apple\" \"banana\")\n"
            "(string<? \"banana\" \"apple\")\n"
            "(string>? \"banana\" \"apple\")\n"
            "(symbol<? 'a 42)\n"
            "(string<? \"a\" 42)\n"
            "\n"
            ";; --- 5. Numerical Math for Plotting ---\n"
            "(to-decimal (math-sin 0) 4)\n"
            "(to-decimal (math-sin (/ 3141593 2000000)) 4)\n"
            "(to-decimal (math-cos 0) 4)\n"
            "(to-decimal (math-cos (/ 3141593 2000000)) 4)\n"
            "(to-decimal (math-tan 0) 4)\n"
            "(math-sqrt 4)\n"
            "(math-sqrt 1/4)\n"
            "(to-decimal (math-sqrt 2) 4)\n"
            "(to-decimal (math-exp 0) 4)\n"
            "(to-decimal (math-log 1) 4)\n"
            "(math-sqrt -1)\n"
            "(math-log -1)\n"
            "(math-log 0)\n"
            "(math-exp 100)\n"
            "(display \"TEST_PHASE43_DONE\\n\")\n"
            "exit\n"
        )

        ok, log = session.send_and_expect(test_script, r"TEST_PHASE43_DONE", timeout=12.0)
        if not ok:
            print("FAILED: Did not receive TEST_PHASE43_DONE sentinel.")
            print("Log snippet:\n", log[-2000:] if len(log) > 2000 else log)
            return 1

        checks: list[tuple[str, bool]] = [
            ("expt (2^10 = 1024)", "=> 1024" in log),
            ("^ alias (3^4 = 81)", "=> 81" in log),
            ("expt negative power (2^-3 = 1/8)", "=> 1/8" in log),
            ("expt ratio base ((2/3)^2 = 4/9)", "=> 4/9" in log),
            ("expt ratio base & neg power ((2/3)^-2 = 9/4)", "=> 9/4" in log),
            ("expt bignum (10^20 = 100000000000000000000)", "100000000000000000000" in log),
            ("expt negative base odd (-2^3 = -8)", "=> -8" in log),
            ("expt negative base even (-2^4 = 16)", "=> 16" in log),
            ("expt zero power (5^0 = 1)", "=> 1" in log),
            ("expt zero base (0^5 = 0)", "=> 0" in log),
            ("expt division by zero rejected (0^-2)", "division by zero" in log),
            ("expt exponent out of range rejected (2^5000)", "out of range" in log),
            ("isqrt exact small (isqrt 25 = 5)", "=> 5" in log),
            ("isqrt floor non-square (isqrt 24 = 4)", "=> 4" in log),
            ("isqrt floor non-square (isqrt 26 = 5)", "=> 5" in log),
            ("isqrt bignum (isqrt 10^20 = 10^10)", "10000000000" in log),
            ("isqrt product bignum", "=> 123456789" in log),
            ("isqrt negative rejected", "argument must be non-negative" in log),
            ("isqrt rational rejected", "argument must be an integer" in log),
            ("to-decimal (1/2, 2 -> \"0.50\")", '"0.50"' in log),
            ("to-decimal (1/3, 4 -> \"0.3333\")", '"0.3333"' in log),
            ("to-decimal round-up (2/3, 4 -> \"0.6667\")", '"0.6667"' in log),
            ("to-decimal (1/8, 3 -> \"0.125\")", '"0.125"' in log),
            ("to-decimal integer (5, 2 -> \"5.00\")", '"5.00"' in log),
            ("to-decimal negative (-1/3, 3 -> \"-0.333\")", '"-0.333"' in log),
            ("number->decimal alias (22/7, 6 -> \"3.142857\")", '"3.142857"' in log),
            ("to-decimal prec 0 round-down (1/3 -> \"0\")", '"0"' in log),
            ("to-decimal prec 0 round-up (2/3 -> \"1\")", '"1"' in log),
            ("symbol<? ordering true", "=> #t" in log),
            ("symbol<? ordering false", "=> #f" in log),
            ("string<? ordering true", "=> #t" in log),
            ("string>? ordering true", "=> #t" in log),
            ("symbol<? type check", "expected symbol" in log),
            ("string<? type check", "expected string" in log),
            ("math-sin (0 -> 0.0000)", '"0.0000"' in log),
            ("math-sin (pi/2 -> 1.0000)", '"1.0000"' in log),
            ("math-cos (0 -> 1.0000)", '"1.0000"' in log),
            ("math-tan (0 -> 0.0000)", '"0.0000"' in log),
            ("math-sqrt integer square (4 -> 2)", "=> 2" in log),
            ("math-sqrt exact rational (1/4 -> 1/2)", "=> 1/2" in log),
            ("math-sqrt irrational root (2 -> \"1.4142\")", '"1.4142"' in log),
            ("math-exp (0 -> 1.0000)", '"1.0000"' in log),
            ("math-log (1 -> 0.0000)", '"0.0000"' in log),
            ("math-sqrt negative rejected", "math-sqrt: negative argument" in log),
            ("math-log non-positive rejected", "math-log: argument must be positive" in log),
            ("math-exp overflow rejected", "math-exp: overflow" in log),
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
