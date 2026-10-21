#!/usr/bin/env python3
"""Phase 61 Test Suite: Restricted Markdown Engine & Ribbon Reader.

Verifies:
1. Native host test harness under ASan/UBSan (tests/host/markdown_host.c)
   - Block parsing: headings, code blocks, math blocks, lists, quotes, alerts, tables, footnotes, TOC.
   - Inline span tokenization: bold, italic, bold+italic, code, links, images, footnotes, math, entities.
   - No-HTML rule: raw HTML tags preserved literally.
   - Slug generation and URL/anchor resolution.
2. QEMU Interactive Verification:
   - Opening Markdown document in Ribbon Reader: 'reader /sd0/help/markdown_showcase.md'
   - Verifying Ribbon window list reflects Reader window.
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
    print("[Phase 61] 1. Compiling Host ASan/UBSan Markdown Test...")
    cmd = ["make", "-C", str(REPO_ROOT / "tests" / "host"), "../../build/host/asan/markdown_host"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Host build failed:\n{res.stderr}")
        return False

    print("[Phase 61] 2. Running Host ASan/UBSan Markdown Test...")
    exe = REPO_ROOT / "build" / "host" / "asan" / "markdown_host"
    res = subprocess.run([str(exe)], capture_output=True, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print(f"Host test failed:\n{res.stderr}")
        return False
    return True


def run_qemu_tests() -> bool:
    elf_path = REPO_ROOT / "build" / "rv32" / "lugalos.elf"
    img_path = REPO_ROOT / "build" / "rv32" / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Error: {elf_path} not found. Run 'ninja -C build/rv32' first.")
        return False

    arch_img = img_path.with_name("test_rv32_markdown_sd.img")
    shutil.copyfile(img_path, arch_img)

    print(f"[Phase 61] 3. Testing Markdown Ribbon Reader in QEMU using {elf_path}...")
    session = QemuSession(elf_path, arch_img, "rv32")
    session.start()

    all_passed = True

    try:
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=6.0)
        if not ok:
            print("Failed to reach shell prompt.")
            return False

        # Open markdown showcase in Ribbon Reader
        print("  Checking 'reader /sd0/help/markdown_showcase.md' in ribbon...")
        ok, log = session.send_and_expect(
            "reader /sd0/help/markdown_showcase.md",
            r"reader: opened .* in ribbon",
            timeout=4.0,
        )
        if ok:
            print("    [PASS] reader opened markdown_showcase.md in ribbon")
        else:
            print(f"    [FAIL] reader in ribbon:\n{log}")
            all_passed = False

        # Verify ribbon list shows 2 windows including Reader
        print("  Checking 'ribbon list' reflects Reader window...")
        ok, log = session.send_and_expect("ribbon list", r"Ribbon Status: 2 windows", timeout=4.0)
        if ok and "Reader" in log:
            print("    [PASS] ribbon list shows Reader window with Markdown document")
        else:
            print(f"    [FAIL] ribbon list:\n{log}")
            all_passed = False

        # Test terminal markdown viewer (md command)
        print("  Checking 'md /sd0/help/markdown_showcase.md' pager...")
        ok, log = session.send_and_expect("md /sd0/help/markdown_showcase.md", r"(More|End of)", timeout=4.0)
        if ok:
            print("    [PASS] md pager displayed markdown document")
            session.send_and_expect("q", r">", timeout=2.0)
        else:
            print(f"    [FAIL] md pager:\n{log}")
            all_passed = False

        # Test man command
        print("  Checking 'man shell' pager...")
        ok, log = session.send_and_expect("man shell", r"(More|End of)", timeout=4.0)
        if ok:
            print("    [PASS] man shell opened help manual")
            session.send_and_expect("q", r">", timeout=2.0)
        else:
            print(f"    [FAIL] man shell:\n{log}")
            all_passed = False

        # Test ea config command
        print("  Checking 'ea config'...")
        ok, log = session.send_and_expect("ea config", r"LLM Endpoint Configuration:", timeout=4.0)
        if ok:
            print("    [PASS] ea config returned configuration")
        else:
            print(f"    [FAIL] ea config:\n{log}")
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
    print("\n[Phase 61] All host and QEMU tests passed successfully!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
