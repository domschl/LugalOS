#!/usr/bin/env python3
"""Where LugalOS's static RAM actually goes, per source file.

The link-time floor in linker/rp2350.ld catches a heap regression. This says
*what caused it* -- which is the part that used to take an afternoon of
`nm -S --size-sort` and guesswork every time it came up (three times so far:
phase 9's H4, phase 13's STRING_POOL_SIZE follow-up, and phase 15).

    python3 tools/sizereport.py build/rp2350/lugalos.elf
    python3 tools/sizereport.py build/rp2350/lugalos.elf --check tools/sizereport-rp2350.json
    python3 tools/sizereport.py build/rp2350/lugalos.elf --update tools/sizereport-rp2350.json

--check compares against a recorded baseline and exits non-zero if static RAM
grew. That is deliberately blunt: on RP2350 .bss and the heap are the same
budget (palloc_init() starts the heap at _kernel_end), so growth here is not a
neutral fact about the image, it is heap somebody else no longer gets. Growing
it on purpose means running --update and having the diff reviewed, which is
the conversation this exists to force.
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

NM_CANDIDATES = ["riscv64-elf-nm", "riscv64-unknown-elf-nm", "riscv32-unknown-elf-nm",
                 "llvm-nm", "nm"]


def find_nm(explicit):
    if explicit:
        return explicit
    for c in NM_CANDIDATES:
        if shutil.which(c):
            return c
    sys.exit("no nm found; pass --nm")


def symbol_value(nm, elf, name):
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def collect(nm, elf):
    """Per-file static RAM, from the symbol table with line info.

    Only symbols inside the writable RAM window count: linker-script symbols
    like _flash_start carry nonsense sizes and must not be summed."""
    ram_start = symbol_value(nm, elf, "_ram_start")
    ram_end = symbol_value(nm, elf, "_ram_end")
    if ram_start is None or ram_end is None:
        sys.exit(f"{elf}: no _ram_start/_ram_end -- is this a LugalOS image?")

    out = subprocess.run([nm, "-S", "-l", elf], capture_output=True, text=True).stdout
    per_file, total = {}, 0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 4 or parts[2].lower() not in ("b", "d"):
            continue
        try:
            addr, size = int(parts[0], 16), int(parts[1], 16)
        except ValueError:
            continue
        if not (ram_start <= addr < ram_end) or size == 0:
            continue
        src = parts[4] if len(parts) >= 5 else "(no line info)"
        src = src.rsplit(":", 1)[0]
        for marker in ("lugalos/",):
            if marker in src:
                src = src.split(marker, 1)[1]
        per_file[src] = per_file.get(src, 0) + size
        total += size

    kernel_end = symbol_value(nm, elf, "_kernel_end")
    heap_end = symbol_value(nm, elf, "_heap_end")
    heap = (heap_end - kernel_end) if (kernel_end and heap_end) else 0
    return {"per_file": per_file, "total": total, "heap_bytes": heap,
            "heap_pages": heap // 4096}


def report(data, baseline=None):
    print(f"{'bytes':>9}  {'delta':>8}  source")
    print("-" * 64)
    base_files = (baseline or {}).get("per_file", {})
    names = sorted(set(data["per_file"]) | set(base_files),
                   key=lambda n: -data["per_file"].get(n, 0))
    for n in names:
        cur = data["per_file"].get(n, 0)
        old = base_files.get(n, 0)
        if cur == 0 and old == 0:
            continue
        d = cur - old
        mark = "" if not baseline else (f"{d:+d}" if d else "")
        print(f"{cur:9d}  {mark:>8}  {n}")
    print("-" * 64)
    d = data["total"] - (baseline or {}).get("total", data["total"])
    print(f"{data['total']:9d}  {d:+8d}  == static RAM total ==" if baseline
          else f"{data['total']:9d}  {'':>8}  == static RAM total ==")
    print(f"{data['heap_bytes']:9d}  {'':>8}  == heap "
          f"({data['heap_pages']} pages of 4096) ==")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--nm")
    ap.add_argument("--check", metavar="BASELINE")
    ap.add_argument("--update", metavar="BASELINE")
    args = ap.parse_args()

    nm = find_nm(args.nm)
    data = collect(nm, args.elf)

    if args.update:
        Path(args.update).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
        report(data)
        print(f"\n[sizereport] baseline written to {args.update}")
        return 0

    baseline = None
    if args.check:
        bp = Path(args.check)
        if not bp.exists():
            print(f"[sizereport] no baseline at {bp}; run with --update first")
            report(data)
            return 0
        baseline = json.loads(bp.read_text())

    report(data, baseline)

    if baseline:
        grew = data["total"] - baseline["total"]
        if grew > 0:
            print(f"\n[sizereport] FAIL: static RAM grew by {grew} bytes "
                  f"({grew / 4096:.1f} heap pages).")
            print("[sizereport] On RP2350 this is heap nothing else can have. If the")
            print("[sizereport] growth is intended, re-baseline with --update.")
            return 1
        print(f"\n[sizereport] OK: static RAM {grew:+d} bytes vs baseline.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
