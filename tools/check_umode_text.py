#!/usr/bin/env python3
"""Fail the build if a U-mode driver task's code can leave its own section --
36.8, plan/phase36_rp2350_lcd7_terminal.md.

A PMP-confined task (the RP2350-LCD-7's `lcdterm` and `kbd`) can execute and
read only its own text section. A call into kernel text or libc, a string
literal or constant table in .rodata, or a switch jump table would all fault
the task on first use -- often on a path the tests never walk. So, from the
task's U-mode entry, every function reachable by a direct call or jump must
lie inside SECTION, and no instruction may compute an address in real memory
(flash, SRAM, peripherals) outside it. (Data the task is granted -- its state, a shared block -- is
reached through pointers it is handed, never by absolute address, so there is
nothing legitimate for this to flag.)

Usage: check_umode_text.py lugalos.elf objdump ENTRY SECTION [ENTRY SECTION ...]
"""
import re
import subprocess
import sys

# RP2350 address ranges that are memory: XIP flash, SRAM, and the peripheral
# windows (APB/AHB, and SIO). A computed value outside all of these is a
# constant that merely looks like an address, which `lui` builds all the time.
MEMORY = [(0x10000000, 0x11000000), (0x20000000, 0x20082000),
          (0x40000000, 0x60000000), (0xd0000000, 0xd0010000)]


def is_memory(a):
    return any(lo <= a < hi for lo, hi in MEMORY)


def section_range(elf, objdump, name):
    out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and p[1] == name:
            size, vma = int(p[2], 16), int(p[3], 16)
            return vma, vma + size
    return None


def main():
    elf, objdump, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
    dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in dis.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if m:
            cur = m.group(2)
            funcs[cur] = (int(m.group(1), 16), [])
        elif cur and line.strip():
            funcs[cur][1].append(line)
    failed = False
    for entry, sect in zip(pairs[0::2], pairs[1::2]):
        rng = section_range(elf, objdump, sect)
        if entry not in funcs or not rng or rng[0] == rng[1]:
            print(f"[umode-text] {entry}: not in this image, nothing to check")
            continue
        lo, hi = rng
        errors, seen, todo = [], set(), [entry]
        while todo:
            name = todo.pop()
            if name in seen:
                continue
            seen.add(name)
            addr, body = funcs[name]
            if not (lo <= addr < hi):
                errors.append(f"{name} is at 0x{addr:08x}, outside {sect}")
            for line in body:
                text = "\t".join(line.split("\t")[1:]) or line
                call = re.search(r"\b(jal|j|call|tail|jalr)\b", text)
                tgt = re.search(r"\b([0-9a-f]{8}) <([^>+]+)(\+0x[0-9a-f]+)?>", text)
                if call and call.group(1) == "jalr" and not tgt:
                    errors.append(f"{name}: indirect call, target unverifiable: {text.strip()}")
                if tgt:
                    a = int(tgt.group(1), 16)
                    if not (lo <= a < hi) and (call or is_memory(a)):
                        errors.append(f"{name}: reaches 0x{a:08x} <{tgt.group(2)}> outside {sect}: {text.strip()}")
                    elif call and tgt.group(3) is None and tgt.group(2) in funcs:
                        todo.append(tgt.group(2))
                for a in re.findall(r"# ([0-9a-f]{8})\b", text):
                    if not (lo <= int(a, 16) < hi) and is_memory(int(a, 16)):
                        errors.append(f"{name}: computes 0x{a}, outside {sect}: {text.strip()}")
        if errors:
            failed = True
            print(f"[umode-text] FAIL: {entry} can leave {sect}:")
            for e in errors:
                print("  " + e)
        else:
            print(f"[umode-text] OK: {entry}: {len(seen)} functions, all inside {sect}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
