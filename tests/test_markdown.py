#!/usr/bin/env python3
"""Phase 61 Test Suite: Restricted Markdown Engine.

Verifies:
1. Native host test harness under ASan/UBSan (tests/host/markdown_host.c)
   - Block parsing: headings, code blocks, math blocks, lists, quotes, alerts, tables, footnotes, TOC.
   - Inline span tokenization: bold, italic, bold+italic, code, links, images, footnotes, math, entities.
   - No-HTML rule: raw HTML tags preserved literally.
   - Slug generation and URL/anchor resolution.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


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


def main() -> int:
    if not run_host_tests():
        return 1
    print("[Phase 61] All host tests passed successfully!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
