#!/usr/bin/env python3
"""45.2, plan/phase45_esp32c6.md: what does the linked blob use of its OS?

Reads build/esp32c6-blob-spike/{blob.elf,blob.map} produced by link.sh and the
IDF's wifi_os_adapter.h, and prints

  1. size by library (text/rodata/data/bss) from the link map;
  2. which wifi_osi_funcs_t entries the *reachable* blob code calls, found by
     disassembling the linked image: the blob reaches its OS through the
     pointer g_osi_funcs_p (a ROM data symbol), so every `lw reg, OFF(base)`
     where `base` was loaded from that pointer is a use of entry OFF/4.

(2) is a static, intraprocedural, straight-line reading: it follows the
pointer through register copies within a function and gives up at a call. It
therefore finds a *lower bound*; the entries it cannot see are listed as
"not seen", not "unused". The dynamic answer is 45.3's job.
"""

import os
import re
import subprocess
import sys
from collections import defaultdict

OUT = "build/esp32c6-blob-spike"
IDF = os.environ.get("IDF_ROOT") or next(
    p for p in (os.path.expanduser("~/Source/gith/esp/esp-idf"),
                os.path.expanduser("~/gith/esp/esp-idf")) if os.path.isdir(p))
HDR = IDF + "/components/esp_wifi/include/esp_private/wifi_os_adapter.h"
OBJDUMP = "riscv64-elf-objdump"
G_OSI_FUNCS_P = 0x4087FF6C   # esp32c6.rom.ld: ROM data symbol


def osi_fields():
    """Field names in order, with the #ifs of the header evaluated for the C6."""
    defs = {"CONFIG_IDF_TARGET_ESP32C6": 1, "CONFIG_SOC_WIFI_HE_SUPPORT": 1}
    names, stack = [], []
    inside = False
    for line in open(HDR):
        s = line.strip()
        if "wifi_osi_funcs_t" in s and s.startswith("}"):
            break
        if s.startswith("typedef struct"):
            inside = True
            continue
        if not inside:
            continue
        if s.startswith("#if"):
            expr = s[3:].strip()
            ids = re.findall(r"CONFIG_\w+", expr)
            ok = eval(re.sub(r"CONFIG_\w+", lambda m: str(defs.get(m.group(0), 0)),
                              expr.replace("||", " or ").replace("&&", " and ")
                                   .replace("!", " not ")))
            stack.append(bool(ok))
            continue
        if s.startswith("#endif"):
            stack.pop()
            continue
        if s.startswith("#"):
            continue
        if not all(stack):
            continue
        m = re.search(r"\(\s*\*\s*(\w+)\s*\)", s) or re.match(r"\w+\s+(_\w+);", s)
        if m:
            names.append(m.group(1))
    return names


IRAM = defaultdict(int)     # input sections the blob marks as IRAM-worthy


def sizes_by_lib():
    """Per-archive totals of what survived --gc-sections, from the map."""
    cur = None
    tot = defaultdict(lambda: defaultdict(int))
    pending = None
    started = False
    for line in open(OUT + "/blob.map"):
        # Everything before this marker is the list of *discarded* input
        # sections; counting those reports what was thrown away as kept.
        if not started:
            started = line.startswith("Linker script and memory map")
            continue
        m = re.match(r"^ (\.\S+)\s*$", line)       # long section name on its own line
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ (\.\S+)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(\S.*)$", line)
        if m:
            sect, size, src = m.group(1), int(m.group(2), 16), m.group(3)
        elif pending:
            m = re.match(r"^\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(\S.*)$", line)
            if not m:
                pending = None
                continue
            sect, size, src = pending, int(m.group(1), 16), m.group(2)
            pending = None
        else:
            continue
        lib = re.search(r"/(lib\w+)\.a\(", src)
        if not lib or size == 0:
            continue
        if sect.startswith((".wifi", ".phyiram")) and "iram" in sect:
            IRAM[re.sub(r"\.\d+$", "", sect)] += size   # .wifi0iram.17 -> .wifi0iram
        kind = ("bss" if sect.startswith((".bss", ".sbss")) else
                "data" if sect.startswith((".data", ".sdata")) else
                "rodata" if sect.startswith((".rodata", ".srodata")) else "text")
        tot[lib.group(1)][kind] += size
    return tot


def osi_use(names):
    dis = subprocess.run([OBJDUMP, "-d", "-M", "no-aliases", OUT + "/blob.elf"],
                         capture_output=True, text=True).stdout
    hi, lo = divmod(G_OSI_FUNCS_P, 4096)
    if lo >= 2048:
        hi, lo = hi + 1, lo - 4096
    seen = defaultdict(set)         # entry index -> functions using it
    func = None
    holders = set()                 # registers currently holding the table pointer
    lui = {}                        # register -> value from lui
    for line in dis.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.+)>:", line)
        if m:
            func, holders, lui = m.group(1), set(), {}
            continue
        m = re.match(r"\s*[0-9a-f]+:\s+[0-9a-f]+\s+(\w[\w.]*)\s*(.*)", line)
        if not m:
            continue
        op, args = m.group(1), [a.strip() for a in re.split(r"[,\s]+", m.group(2).split("#")[0].strip()) if a]
        if op in ("jal", "jalr", "c.jal", "c.jalr"):
            # a call clobbers the caller-saved registers; s-registers survive
            holders = {r for r in holders if r.startswith("s")}
            lui = {r: v for r, v in lui.items() if r.startswith("s")}
            continue
        if op == "c.lui":
            op = "lui"
        if op == "lui" and len(args) == 2:
            lui[args[0]] = int(args[1], 0) << 12
            holders.discard(args[0])
            continue
        if op == "c.lw":
            op = "lw"
        mm = re.match(r"(-?\d+)\((\w+)\)", args[1]) if len(args) == 2 else None
        if op == "lw" and mm:
            rd, off, base = args[0], int(mm.group(1)), mm.group(2)
            if base in lui and lui[base] + off == G_OSI_FUNCS_P:
                lui.pop(rd, None); holders.add(rd)      # rd = g_osi_funcs_p (the table's address)
            elif base in holders:
                if off % 4 == 0 and off // 4 < len(names):
                    seen[off // 4].add(func)
                holders.discard(rd); lui.pop(rd, None)
            else:
                holders.discard(rd); lui.pop(rd, None)
            continue
        if op in ("c.mv", "mv") and len(args) == 2:
            if args[1] in holders:
                holders.add(args[0])
            else:
                holders.discard(args[0])
            lui.pop(args[0], None)
            continue
        if op in ("sw", "sb", "sh", "c.sw", "c.swsp", "beq", "bne", "blt", "bge", "bltu", "bgeu",
                  "c.beqz", "c.bnez", "c.j", "c.jr", "jr", "j"):
            continue
        if args:
            holders.discard(args[0]); lui.pop(args[0], None)
    return seen


def main():
    names = osi_fields()
    print("wifi_osi_funcs_t: %d fields for the C6 (incl. _version, _magic)" % len(names))
    print("\n== size of what is reachable, by library (bytes) ==")
    tot = sizes_by_lib()
    rows = []
    for lib, k in sorted(tot.items(), key=lambda kv: -sum(kv[1].values())):
        rows.append((lib, k["text"], k["rodata"], k["data"], k["bss"]))
        print("  %-14s text %7d  rodata %6d  data %5d  bss %6d" % rows[-1])
    print("  %-14s text %7d  rodata %6d  data %5d  bss %6d" % (
        "TOTAL", *(sum(r[i] for r in rows) for i in (1, 2, 3, 4))))
    print("\n== code the blob marks as IRAM-worthy (IDF's linker.lf puts it in IRAM only when") 
    print("   CONFIG_ESP_WIFI_{IRAM,RX_IRAM,SLP_IRAM}_OPT are on; otherwise it runs from flash) ==")
    for sect, n in sorted(IRAM.items()):
        print("  %-18s %6d" % (sect, n))
    print("  %-18s %6d" % ("TOTAL", sum(IRAM.values())))
    seen = osi_use(names)
    print("\n== OSI table entries called by reachable blob code (static lower bound) ==")
    print("  seen %d of %d" % (len(seen), len(names)))
    for i in sorted(seen):
        print("  %3d %-40s %2d fn" % (i, names[i], len(seen[i])))
    print("\n== entries not seen (may still be called through a path this reading misses) ==")
    notseen = [names[i] for i in range(len(names)) if i not in seen]
    print("  " + " ".join(notseen))


if __name__ == "__main__":
    main()
