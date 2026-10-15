#!/usr/bin/env python3
"""Phase 53 Test Suite: Gemtext Parser, Help Migration & Interactive Browser.

Verifies:
1. Native host test harness under ASan/UBSan (tests/host/gemtext_host.c)
2. QEMU interactive verification:
   - 'gem' usage banner
   - 'man editors' loading /sd0/help/editors.gmi
   - Unambiguous link following ('1' -> c_programming.gmi)
   - History stack pop ('b' -> back to editors.gmi)
   - Clean quit ('q' -> lsh prompt)
   - 'gem /sd0/help/shell.gmi' direct path browsing
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]


def run_host_tests() -> bool:
    print("[Phase 53] 1. Running Host ASan/UBSan Gemtext Test...")
    cmd = ["make", "-C", str(REPO_ROOT / "tests" / "host"), "../../build/host/asan/gemtext_host"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Host build failed:\n{res.stderr}")
        return False

    exe = REPO_ROOT / "build" / "host" / "asan" / "gemtext_host"
    res = subprocess.run([str(exe)], capture_output=True, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print(f"Host test failed:\n{res.stderr}")
        return False

    print("[Phase 53] 1b. Running Host ASan/UBSan Spartan Protocol Test...")
    cmd = ["make", "-C", str(REPO_ROOT / "tests" / "host"), "../../build/host/asan/spartan_host"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Host build failed:\n{res.stderr}")
        return False

    exe = REPO_ROOT / "build" / "host" / "asan" / "spartan_host"
    res = subprocess.run([str(exe)], capture_output=True, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print(f"Spartan host test failed:\n{res.stderr}")
        return False
    return True


def run_qemu_tests() -> bool:
    elf_path = REPO_ROOT / "build" / "rv32" / "lugalos.elf"
    img_path = REPO_ROOT / "build" / "rv32" / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/rv32' first.")
        return False

    arch_img = img_path.with_name("test_rv32_gemtext_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 53] 2. Testing Gemtext Browser in QEMU using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    def send_key(key: bytes, pattern: str, timeout: float = 4.0) -> tuple[bool, str]:
        session._drain()
        if not session._write(key):
            return False, "Failed to write key"
        accum = b""
        start = time.time()
        import re
        rx = re.compile(pattern, re.MULTILINE | re.DOTALL)
        while time.time() - start < timeout:
            chunk = session._log_file.read() if session._log_file else None
            if not chunk:
                time.sleep(0.02)
                continue
            accum += chunk
            txt = accum.decode("utf-8", "replace")
            if rx.search(txt):
                return True, txt
        return False, accum.decode("utf-8", "replace")

    try:
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return False

        # Test 1: 'gem' usage banner
        print("  Checking 'gem' usage...")
        ok, log = session.send_and_expect("gem", r"Usage: gem <file\.gmi \| topic \| spartan://host\[:port\]/path>", timeout=4.0)
        if ok:
            print("    [PASS] gem usage banner")
        else:
            print(f"    [FAIL] gem usage:\n{log}")
            all_passed = False

        # Test 2: 'man editors' opens editors.gmi
        print("  Checking 'man editors' (Gemtext help)...")
        ok, log = session.send_and_expect("man editors", r"More \(Space: Next.*1\.\.9: Link\)", timeout=5.0, sentinel=False)
        if ok and "Text Editors" in log:
            print("    [PASS] man editors opened Gemtext help")
        else:
            print(f"    [FAIL] man editors:\n{log}")
            all_passed = False

        # Test 3: Follow link '1' to C programming
        print("  Following link [1] to c_programming.gmi...")
        ok, log = send_key(b"1", r"C Programming", timeout=4.0)
        if ok:
            print("    [PASS] Link [1] jumped to c_programming.gmi")
        else:
            print(f"    [FAIL] Link navigation:\n{log}")
            all_passed = False

        # Test 4: Back stack 'b' returns to editors.gmi
        print("  Testing back navigation ('b')...")
        ok, log = send_key(b"b", r"Text Editors", timeout=4.0)
        if ok:
            print("    [PASS] Back navigation returned to editors.gmi")
        else:
            print(f"    [FAIL] Back navigation:\n{log}")
            all_passed = False

        # Test 5: Exit viewer with 'q'
        print("  Exiting viewer ('q')...")
        ok, log = send_key(b"q", r"lsh>", timeout=4.0)
        if ok:
            print("    [PASS] Clean exit to lsh prompt")
        else:
            print(f"    [FAIL] Exit to shell:\n{log}")
            all_passed = False

        # Test 6: Direct path browsing with 'gem /sd0/help/shell.gmi'
        print("  Testing 'gem /sd0/help/shell.gmi'...")
        ok, log = session.send_and_expect("gem /sd0/help/shell.gmi", r"Shell", timeout=4.0, sentinel=False)
        if ok:
            print("    [PASS] gem /sd0/help/shell.gmi opened successfully")
        else:
            print(f"    [FAIL] gem direct path:\n{log}")
            all_passed = False

        # Exit viewer
        send_key(b"q", r"lsh>", timeout=3.0)

        # Test 7: spartand daemon status (stopped initially)
        print("  Checking 'spartand status'...")
        ok, log = session.send_and_expect("spartand status", r"spartand: stopped.*sandbox:  /sd0/pub", timeout=4.0)
        if ok:
            print("    [PASS] spartand initial status (stopped)")
        else:
            print(f"    [FAIL] spartand status:\n{log}")
            all_passed = False

        # Test 8: spartand daemon start
        print("  Checking 'spartand start'...")
        ok, log = session.send_and_expect("spartand start", r"spartand: started on port 300", timeout=4.0)
        if ok:
            print("    [PASS] spartand started on port 300")
        else:
            print(f"    [FAIL] spartand start:\n{log}")
            all_passed = False

        # Test 9: spartand daemon status (running)
        print("  Checking 'spartand status' (running)...")
        ok, log = session.send_and_expect("spartand status", r"spartand: running on port 300", timeout=4.0)
        if ok:
            print("    [PASS] spartand reports running on port 300")
        else:
            print(f"    [FAIL] spartand status running:\n{log}")
            all_passed = False

        # Test 10: spartand daemon stop
        print("  Checking 'spartand stop'...")
        ok, log = session.send_and_expect("spartand stop", r"spartand: stopped", timeout=4.0)
        if ok:
            print("    [PASS] spartand stopped cleanly")
        else:
            print(f"    [FAIL] spartand stop:\n{log}")
            all_passed = False

    finally:
        session.close()
        if arch_img.exists():
            arch_img.unlink()

    return all_passed


def main() -> int:
    if not run_host_tests():
        return 1
    if not run_qemu_tests():
        return 1
    print("\n[SUCCESS] All Phase 53 Gemtext tests passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
