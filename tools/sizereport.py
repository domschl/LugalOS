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


def find_readelf(nm):
    """readelf beside the nm in use (riscv64-elf-nm -> riscv64-elf-readelf)."""
    if nm.endswith("nm"):
        cand = nm[:-2] + "readelf"
        if shutil.which(cand) or Path(cand).exists():
            return cand
    for c in ("riscv64-elf-readelf", "riscv64-unknown-elf-readelf", "readelf"):
        if shutil.which(c):
            return c
    sys.exit("no readelf found")


def symbol_value(nm, elf, name):
    out = subprocess.run([nm, elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def symbol_types(readelf, elf):
    """{(name, addr): 'OBJECT'|'FUNC'} from the ELF's own symbol types.

    38.4: this used to filter on nm's type letter (b/d), and on RP2350 nm
    types every .data symbol `t` -- .data shares an executable PT_LOAD with
    .ramfunc -- so initialised statics were not counted at all (329 b, 68 t,
    zero d; plan/open_issues.md). The symbol's own type does not depend on
    where the linker put it."""
    out = subprocess.run([readelf, "-sW", elf], capture_output=True, text=True).stdout
    types = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(":"):
            continue
        if parts[3] in ("OBJECT", "FUNC"):
            try:
                types[(parts[7], int(parts[1], 16))] = parts[3]
            except ValueError:
                pass
    return types


def short_src(src):
    src = src.rsplit(":", 1)[0]
    for marker in ("lugalos/",):
        if marker in src:
            src = src.split(marker, 1)[1]
    return src


def collect(nm, elf, readelf=None):
    """Per-file static RAM, from the symbol table with line info.

    Three figures:
      per_file / total -- data objects in SRAM (_ram_start.._ram_end), plus
                          ram_code: functions placed in SRAM (.ramfunc and
                          the like). Both are heap nobody else can have, so
                          both are in `total`, the number the check guards.
      bulk_per_file / bulk_total -- data objects in BULK_BSS
                          (_bulk_bss_start.._bulk_bss_end): PSRAM on a board
                          that has one (38.4). Checked separately, so growth
                          in one memory never hides behind the other.
    Only symbols inside those windows count: linker-script symbols like
    _flash_start carry nonsense sizes and must not be summed."""
    ram_start = symbol_value(nm, elf, "_ram_start")
    ram_end = symbol_value(nm, elf, "_ram_end")
    if ram_start is None or ram_end is None:
        sys.exit(f"{elf}: no _ram_start/_ram_end -- is this a LugalOS image?")
    bulk_start = symbol_value(nm, elf, "_bulk_bss_start") or 0
    bulk_end = symbol_value(nm, elf, "_bulk_bss_end") or 0
    if not (bulk_end > bulk_start and not (ram_start <= bulk_start < ram_end)):
        bulk_start = bulk_end = 0       # absent, or inside SRAM (counted there)

    types = symbol_types(readelf or find_readelf(nm), elf)
    out = subprocess.run([nm, "-S", "-l", elf], capture_output=True, text=True).stdout
    per_file, total, ram_code = {}, 0, 0
    bulk_per_file, bulk_total = {}, 0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 4:
            continue
        try:
            addr, size = int(parts[0], 16), int(parts[1], 16)
        except ValueError:
            continue
        if size == 0:
            continue
        kind = types.get((parts[3], addr))
        src = short_src(parts[4]) if len(parts) >= 5 else "(no line info)"
        if ram_start <= addr < ram_end:
            if kind == "OBJECT":
                per_file[src] = per_file.get(src, 0) + size
                total += size
            elif kind == "FUNC":
                ram_code += size
        elif bulk_start <= addr < bulk_end and kind == "OBJECT":
            bulk_per_file[src] = bulk_per_file.get(src, 0) + size
            bulk_total += size

    kernel_end = symbol_value(nm, elf, "_kernel_end")
    heap_end = symbol_value(nm, elf, "_heap_end")
    heap = (heap_end - kernel_end) if (kernel_end and heap_end) else 0
    return {"per_file": per_file, "total": total + ram_code, "ram_code": ram_code,
            "bulk_per_file": bulk_per_file, "bulk_total": bulk_total,
            "heap_bytes": heap, "heap_pages": heap // 4096}


def report_files(cur_files, base_files, show_delta):
    names = sorted(set(cur_files) | set(base_files), key=lambda n: -cur_files.get(n, 0))
    for n in names:
        cur = cur_files.get(n, 0)
        old = base_files.get(n, 0)
        if cur == 0 and old == 0:
            continue
        d = cur - old
        mark = "" if not show_delta else (f"{d:+d}" if d else "")
        print(f"{cur:9d}  {mark:>8}  {n}")


def report(data, baseline=None):
    b = baseline or {}
    print(f"{'bytes':>9}  {'delta':>8}  source")
    print("-" * 64)
    report_files(data["per_file"], b.get("per_file", {}), baseline is not None)
    code = data.get("ram_code", 0)
    dc = code - b.get("ram_code", code)
    print(f"{code:9d}  {(f'{dc:+d}' if baseline and dc else ''):>8}  (code placed in SRAM: .ramfunc and the like)")
    print("-" * 64)
    d = data["total"] - b.get("total", data["total"])
    print(f"{data['total']:9d}  {(f'{d:+d}' if baseline else ''):>8}  == static RAM total ==")
    print(f"{data['heap_bytes']:9d}  {'':>8}  == heap "
          f"({data['heap_pages']} pages of 4096) ==")
    if data.get("bulk_total") or b.get("bulk_total"):
        print()
        report_files(data.get("bulk_per_file", {}), b.get("bulk_per_file", {}), baseline is not None)
        print("-" * 64)
        db = data["bulk_total"] - b.get("bulk_total", data["bulk_total"])
        print(f"{data['bulk_total']:9d}  {(f'{db:+d}' if baseline else ''):>8}  == BULK_BSS total (PSRAM) ==")


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
        bulk_grew = data.get("bulk_total", 0) - baseline.get("bulk_total", 0)
        if grew > 0:
            print(f"\n[sizereport] FAIL: static RAM grew by {grew} bytes "
                  f"({grew / 4096:.1f} heap pages).")
            print("[sizereport] On RP2350 this is heap nothing else can have. If the")
            print("[sizereport] growth is intended, re-baseline with --update.")
            return 1
        if bulk_grew > 0:
            print(f"\n[sizereport] FAIL: BULK_BSS grew by {bulk_grew} bytes -- PSRAM the bulk")
            print("[sizereport] page zone no longer has. If intended, re-baseline with --update.")
            return 1
        print(f"\n[sizereport] OK: static RAM {grew:+d} bytes, BULK_BSS {bulk_grew:+d} bytes vs baseline.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
