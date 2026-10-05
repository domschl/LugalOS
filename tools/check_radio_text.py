#!/usr/bin/env python3
"""45.3b, plan/phase45_esp32c6.md: is the radio shim's code self-contained?

The shim (drivers/radio/osi_impl.c, uprintf.c, nvs_ram.c and kernel/uheap.c) is
linked into a U-mode domain's own text. A call from there to anything outside
that text -- a libc function, a compiler helper (on rv32 every 64-bit shift,
multiply-high or divide is one: __lshrdi3, __udivdi3, ...), a kernel function --
is an instruction access fault at run time, and a fault in the radio is a dead
radio. The compiler will not say so; the linker is happy to resolve them.

This reads the object files and fails if any of them has a call or a data
reference to a symbol that is not defined in one of the four, or in the
allow-list of U-mode-safe externals. Run after a build:

    tools/check_radio_text.py [build_dir ...]        (default: every build/*/ that has them)
"""
import glob
import os
import re
import subprocess
import sys

OBJS = ["drivers/radio/osi_impl.c.obj", "drivers/radio/uprintf.c.obj",
        "drivers/radio/nvs_ram.c.obj", "kernel/uheap.c.obj"]
# Symbols a radio object may reference that are not in the four objects. Today:
# none -- the shim reaches the kernel by ecall, never by symbol.
ALLOWED = set()


def nm(path, flag):
    out = subprocess.run(["riscv64-elf-nm", flag, path], capture_output=True, text=True).stdout
    return {l.split()[-1] for l in out.splitlines() if l.strip()}


def check(build):
    objs = [os.path.join(build, "CMakeFiles", "lugalos.elf.dir", o) for o in OBJS]
    objs = [o for o in objs if os.path.exists(o)]
    if len(objs) != len(OBJS):
        return None
    defined = set()
    for o in objs:
        defined |= nm(o, "--defined-only")
    bad = {}
    for o in objs:
        for sym in nm(o, "--undefined-only"):
            if sym in defined or sym in ALLOWED:
                continue
            bad.setdefault(sym, []).append(os.path.basename(o))
    return bad


def main():
    builds = sys.argv[1:] or sorted(glob.glob("build/*/"))
    rc, checked = 0, 0
    for b in builds:
        bad = check(b)
        if bad is None:
            continue
        checked += 1
        if bad:
            rc = 1
            print("FAIL %s: the radio shim references symbols outside itself:" % b)
            for sym, who in sorted(bad.items()):
                print("   %-24s from %s" % (sym, ", ".join(sorted(set(who)))))
        else:
            print("ok   %s: the radio shim references nothing outside itself" % b)
    if not checked:
        print("no build with the radio objects found (build rv32 or rv64 first)")
        return 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
