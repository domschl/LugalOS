#!/usr/bin/env python3
"""
embed_page.py - turns one web page into a C array in the firmware image.

The setup page cannot live on /flash0: an unclaimed node must serve it whatever state its filesystems are in
(blank, stale, full), so it is part of the image itself, read from flash like any other constant.

Usage: embed_page.py <output.c> <page.html> <symbol>
Defines `const char <symbol>[]` (NUL-terminated) and `const unsigned <symbol>_len` (without the NUL).
"""

import sys
from pathlib import Path


def main() -> int:
    out, page, sym = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    data = page.read_bytes()
    lines = [f"/* generated from {page.name} by tools/embed_page.py */", f"const char {sym}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    lines.append("    0x00")
    lines.append("};")
    lines.append(f"const unsigned {sym}_len = {len(data)};")
    new = "\n".join(lines) + "\n"
    if not out.exists() or out.read_text() != new:
        out.write_text(new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
