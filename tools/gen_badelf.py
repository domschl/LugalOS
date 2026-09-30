#!/usr/bin/env python3
"""gen_badelf.py - writes the malformed ELF the B12 test execs.

tests/runner.py's "ELF Loader Rejects Malformed Program Headers (B12)" runs
`exec /sd0/badelf.bin` and expects a clean diagnostic rather than an
out-of-bounds read. The file is a syntactically valid ELF32 RISC-V header whose
e_phoff points far past the end of the file and whose e_phnum claims 100
program headers -- the exact defect B12 recorded.

Generated at build time, not checked in. It used to be a checked-in
tools/sd_root/badelf.bin, except it never was: `*.bin` in .gitignore swallowed
it, so the file existed only in the working tree it was made in, and every
fresh clone failed B12 with "Failed to open '/sd0/badelf.bin'" (found on a new
machine, 2026-09-30, plan/phase36_rp2350_lcd7_terminal.md 36.0a). Twelve
lines of struct.pack are a better artifact than a binary nobody can review.

    python3 tools/gen_badelf.py <output path>
"""

import struct
import sys

EM_RISCV = 243
ET_EXEC = 2


def badelf() -> bytes:
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)  # ELF32, little-endian, v1
    return ident + struct.pack(
        "<HHIIIIIHHHHHH",
        ET_EXEC,       # e_type
        EM_RISCV,      # e_machine
        1,             # e_version
        0x00010000,    # e_entry
        0x00100000,    # e_phoff: 1 MB into a 52-byte file
        0,             # e_shoff
        0,             # e_flags
        52,            # e_ehsize
        32,            # e_phentsize
        100,           # e_phnum: a loop bound nothing in the file backs
        40,            # e_shentsize
        0,             # e_shnum
        0,             # e_shstrndx
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: gen_badelf.py <output path>")
    with open(sys.argv[1], "wb") as f:
        f.write(badelf())
