#!/usr/bin/env python3
"""45.3, plan/phase45_esp32c6.md: can the C6's ROM Wi-Fi code run in U-mode?

The blob calls 304 functions in the chip's mask ROM. If any of the code those
reach executes a CSR instruction, `wfi`, `mret` or `ecall`, it would trap with
illegal-instruction when called from U-mode, and U-mode confinement of the
radio would be dead on arrival. This walks the ROM's control-flow from each
ROM symbol the linked blob references (build/esp32c6-blob-spike/rom_used.txt)
and reports every privileged instruction it can reach.

Input: a ROM image dumped from the chip (`esptool --no-stub dump-mem
0x40000000 0x50000 c6rom.bin`) -- not committed, it is Espressif's ROM; and
build/esp32c6-blob-spike/blob.elf for symbol addresses.

Static and best-effort: direct jumps, branches, calls and auipc+jalr pairs
are followed; indirect jumps/calls through registers are not (reported as a
count). Disassembly is objdump's linear sweep, so a function after embedded
data may be mis-decoded; unresolved targets are counted.
"""
import re, subprocess, sys, collections

ROM_BIN, BLOB = sys.argv[1], "build/esp32c6-blob-spike"
ROM_LO, ROM_HI = 0x40000000, 0x40050000
USER_CSRS = {"cycle", "time", "instret", "cycleh", "timeh", "instreth", "0x802"}

dis = {}
out = subprocess.run(["riscv64-elf-objdump", "-b", "binary", "-m", "riscv:rv32", "-M", "no-aliases",
                      "--adjust-vma=0x40000000", "-D", ROM_BIN], capture_output=True, text=True).stdout
for l in out.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f]+)\s*\t(\S+)\s*(.*)", l)
    if m:
        dis[int(m.group(1), 16)] = (len(m.group(2)) // 2, m.group(3), m.group(4).split("#")[0].strip())

syms = {}
for l in subprocess.run(["riscv64-elf-nm", BLOB + "/blob.elf"], capture_output=True, text=True).stdout.splitlines():
    f = l.split()
    if len(f) == 3 and ROM_LO <= int(f[0], 16) < ROM_HI:
        syms[f[2]] = int(f[0], 16)
used = [n for n in open(BLOB + "/rom_used.txt").read().split() if n in syms]

PRIV = ("wfi", "mret", "sret", "ecall", "ebreak", "c.ebreak", "fence.i")
seen, work, hits = set(), [], collections.defaultdict(set)
unres = 0; indirect = 0
owner = {}
for n in used:
    work.append((syms[n], n))
while work:
    pc, root = work.pop()
    while ROM_LO <= pc < ROM_HI:
        if pc in seen:
            break
        if pc not in dis:
            unres += 1
            break
        seen.add(pc)
        ln, op, args = dis[pc]
        a = [x.strip() for x in args.split(",")] if args else []
        if op.startswith("csr"):
            csr = a[1] if op.endswith("i") or len(a) > 2 else a[0]
            # csrrw rd, csr, rs : csr is a[1]
            csr = a[1] if len(a) > 1 else "?"
            hits[csr].add((pc, op, root))
        elif op in PRIV:
            hits[op].add((pc, op, root))
        nxt = pc + ln
        if op in ("jal", "c.jal", "c.j"):
            tgt = int(a[-1], 16) if a and a[-1].startswith("0x") else None
            if tgt is not None:
                work.append((tgt, root))
            else:
                unres += 1
            if op == "c.jal" or (op == "jal" and a[0] != "zero"):
                pc = nxt; continue          # a call: fall through after return
            break                            # a jump
        if op in ("beq", "bne", "blt", "bge", "bltu", "bgeu", "c.beqz", "c.bnez"):
            tgt = int(a[-1], 16) if a[-1].startswith("0x") else None
            if tgt is not None:
                work.append((tgt, root))
            pc = nxt; continue
        if op in ("jalr", "c.jalr", "c.jr"):
            indirect += 1
            if op == "c.jalr" or (op == "jalr" and a[0] != "zero"):
                pc = nxt; continue
            break                            # return or computed jump
        if op == "auipc" and nxt in dis and dis[nxt][1] == "jalr":
            # resolve auipc+jalr call/jump: target = pc + (imm<<12) + off
            try:
                hi = int(a[1], 0) << 12
                jargs = [x.strip() for x in dis[nxt][2].split(",")]
                off = int(jargs[-1].split("(")[0], 0) if "(" in jargs[-1] else int(jargs[1], 0)
                tgt = (pc + hi + off) & 0xffffffff
                work.append((tgt, root))
            except Exception:
                unres += 1
        pc = nxt

# Which peripheral pages does the reachable ROM code (and the blob) address?
# A `lui` of 0x600xx / 0x6000x-0x600ff in this chip's peripheral space is how
# both build a register address. Page = the lui immediate (4 KB granule).
rom_pages = collections.Counter()
for pc in seen:
    ln, op, args = dis[pc]
    if op in ("lui", "c.lui"):
        try:
            v = int(args.split(",")[1], 0)
        except Exception:
            continue
        if 0x60000 <= v < 0x60100:
            rom_pages[v] += 1
blob_pages = collections.Counter()
for l in open(BLOB + "/blob.dis"):
    m = re.search(r"\tc?\.?lui\t\w+,0x(6[0-9a-f]{4})\b", l)
    if m and 0x60000 <= int(m.group(1), 16) < 0x60100:
        blob_pages[int(m.group(1), 16)] += 1
print("peripheral pages addressed (lui 0x600xx): blob | reachable ROM code")
for v in sorted(set(rom_pages) | set(blob_pages)):
    print("  0x%05x000  %4d | %4d" % (v, blob_pages.get(v, 0), rom_pages.get(v, 0)))
print("ROM functions referenced by the blob: %d; instructions reached: %d" % (len(used), len(seen)))
print("indirect jumps/calls not followed: %d; undecodable targets: %d" % (indirect, unres))
print("privileged instructions reachable:")
if not hits:
    print("  none")
for k, v in sorted(hits.items()):
    roots = sorted({r for _, _, r in v})
    print("  %-12s %3d site(s), reached from %d root(s): %s%s" % (
        k, len(v), len(roots), ", ".join(roots[:4]), " ..." if len(roots) > 4 else ""))
    for pc, op, r in sorted(v)[:3]:
        print("      0x%08x %s  (via %s)" % (pc, op, r))
