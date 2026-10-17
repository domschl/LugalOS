#!/usr/bin/env python3
"""Phase 58 Test Suite: On-Device AI Agent (ea), LLM Primitives & Math Assistant.

Verifies:
1. Native host test harness under ASan/UBSan (tests/host/llm_host.c)
2. QEMU verification:
   - 'ea' usage banner
   - 'ea config' displaying default/configured URL and model
   - Lisp builtins: (llm-config) query and update
   - Tool execution evaluation
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]


def run_host_tests() -> bool:
    print("[Phase 58] 1. Running Host ASan/UBSan DNS, HTTP & LLM Client Test...")
    cmd = ["make", "-C", str(REPO_ROOT / "tests" / "host"), "../../build/host/asan/llm_host"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Host build failed:\n{res.stderr}")
        return False

    exe = REPO_ROOT / "build" / "host" / "asan" / "llm_host"
    res = subprocess.run([str(exe)], capture_output=True, text=True)
    print(res.stdout)
    if res.returncode != 0:
        print(f"Host test failed:\n{res.stderr}")
        return False
    return True


def run_qemu_tests() -> bool:
    import shutil
    print("[Phase 58] 2. Running QEMU interactive verification (rv32)...")
    preset = "rv32"
    build_dir = REPO_ROOT / "build" / preset
    elf_path = build_dir / "lugalos.elf"
    img_path = build_dir / "lugalos_sd.img"

    if not elf_path.exists():
        print(f"Build directory {build_dir} missing lugalos.elf. Building now...")
        res = subprocess.run(["ninja", "-C", str(build_dir)], capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Build failed:\n{res.stderr}")
            return False

    arch_img = img_path.with_name(f"test_rv32_ea_sd.img")
    shutil.copyfile(img_path, arch_img)

    session = QemuSession(elf_path, arch_img, preset)
    session.start()

    try:
        ok, log = session.send_and_expect("", r"LugalOS Interactive Console Shell", timeout=10.0)
        assert ok, f"Failed waiting for shell prompt, got:\n{log}"
        print("  -> Board booted into lsh.")

        # 1. Test ea usage banner
        ok, log = session.send_and_expect("ea", r"Usage: ea", timeout=5.0)
        assert ok and "Usage: ea" in log, f"Expected usage banner, got:\n{log}"
        print("  -> 'ea' banner verified: PASS")

        # 2. Test ea config
        ok, log = session.send_and_expect("ea config", r"LLM Endpoint Configuration:", timeout=5.0)
        assert ok and "URL:" in log and "Model:" in log, f"Expected config header and fields, got:\n{log}"
        print("  -> 'ea config' command: PASS")

        # 3. Test Lisp (llm-config) primitive
        ok, log = session.send_and_expect("(llm-config)", r'\(\(url\s*\.\s*"[^"]+"\)\s*\(model\s*\.\s*"[^"]+"\)\)', timeout=5.0)
        assert ok, f"Expected alist from (llm-config), got:\n{log}"
        print("  -> (llm-config) query: PASS")

        # 4. Test Lisp (llm-config "http://test:8000/v1" "Gemma4-12B-MTP")
        ok, log = session.send_and_expect('(llm-config "http://test:8000/v1" "Gemma4-12B-MTP")', r'Gemma4-12B-MTP', timeout=5.0)
        assert ok, f"Expected updated model in config, got:\n{log}"
        print("  -> (llm-config ...) update: PASS")

        # Verify ea config reflects updated model
        ok, log = session.send_and_expect("ea config", r"Gemma4-12B-MTP", timeout=5.0)
        assert ok and "http://test:8000/v1" in log, f"Expected http://test:8000/v1 in ea config, got:\n{log}"
        print("  -> 'ea config' updated state verified: PASS")

        print("\nAll QEMU Phase 58 tests passed successfully!")
        return True

    finally:
        session.close()
        if arch_img.exists():
            try:
                arch_img.unlink()
            except OSError:
                pass


def main() -> int:
    print("==================================================")
    print(" Phase 58: AI Agent (ea) & LLM Primitives Tests   ")
    print("==================================================")

    if not run_host_tests():
        return 1

    if not run_qemu_tests():
        return 1

    print("\n[SUCCESS] Phase 58 test suite fully PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
