#!/usr/bin/env python3
"""Fail the build if core 1's engine could touch flash -- 36.7,
plan/phase36_rp2350_lcd7_terminal.md §3.2.1.

On the RP2350-LCD-7 core 1 runs the PIO-USB engine (drivers/piousb_rp2350.c)
from RAM, for good. It must never fetch from or read the XIP window: while
core 0 writes flash, XIP is off, and a flash access on core 1 would hang it --
silently, with the keyboard gone. That is a property of the compiled code,
not of any one run, so it is checked here on every build rather than by a
test that happens to write flash at the right moment.

Starting at core1_main, every function reachable by a direct call or jump is
followed, and each must
  * lie in RAM (0x20000000..), i.e. in .ramfunc, and
  * contain no call, jump or computed address that lands in flash
    (0x10000000-0x10ffffff) -- a libc/libgcc call, a .rodata constant, or a
    switch jump table would all show up as one.
Indirect calls cannot be followed; any in the engine is reported as an error
too, since the check could not vouch for their target.

Usage: check_core1_ram.py lugalos.elf [objdump]
"""
import re
import subprocess
import sys

RAM_LO, RAM_HI = 0x20000000, 0x20082000
FLASH_LO, FLASH_HI = 0x10000000, 0x11000000


def main():
    elf = sys.argv[1]
    objdump = sys.argv[2] if len(sys.argv) > 2 else "riscv64-elf-objdump"
    dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if m:
            cur = m.group(2)
            funcs[cur] = (int(m.group(1), 16), [])
            continue
        if cur and line.strip():
            funcs[cur][1].append(line)
    if "core1_main" not in funcs:
        print("[core1-ram] no core1_main in this image: nothing to check")
        return 0

    errors, seen, todo = [], set(), ["core1_main"]
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        addr, body = funcs[name]
        if not (RAM_LO <= addr < RAM_HI):
            errors.append(f"{name} is at 0x{addr:08x}, not in RAM")
        for line in body:
            ins = line.split("\t")
            text = "\t".join(ins[1:]) if len(ins) > 1 else line
            m = re.search(r"\b(jal|j|call|tail|jalr)\b", text)
            tgt = re.search(r"\b([0-9a-f]{8}) <([^>+]+)(\+0x[0-9a-f]+)?>", text)
            if m and m.group(1) == "jalr" and not tgt:
                errors.append(f"{name}: indirect call, target unverifiable: {text.strip()}")
            if tgt:
                a = int(tgt.group(1), 16)
                if FLASH_LO <= a < FLASH_HI:
                    errors.append(f"{name}: reaches flash at 0x{a:08x} <{tgt.group(2)}>: {text.strip()}")
                elif m and tgt.group(3) is None and tgt.group(2) in funcs and tgt.group(2) != name:
                    todo.append(tgt.group(2))
            for a in re.findall(r"# ([0-9a-f]{8})\b", text):
                if FLASH_LO <= int(a, 16) < FLASH_HI:
                    errors.append(f"{name}: computes a flash address 0x{a}: {text.strip()}")
            if re.search(r"\blui\s+\w+,0x1[0-9a-f]{4}\b", text):
                errors.append(f"{name}: lui into the flash window: {text.strip()}")
    if errors:
        print("[core1-ram] FAIL: core 1's engine can reach flash:")
        for e in errors:
            print("  " + e)
        return 1
    print(f"[core1-ram] OK: {len(seen)} functions reachable from core1_main, all in RAM, none touching flash")
    return 0


if __name__ == "__main__":
    sys.exit(main())
