#!/usr/bin/env python3
"""The RP2350 partition table for cmake/flash_layout.cmake's map.

    rp2350_ptable.py [--layout cmake/flash_layout_rp2350_terminal.cmake] build/rp2350-wifi/ptable.uf2

Slots A and B are an A/B pair for the bootrom (datasheet 5.1.7): it boots the
higher-versioned valid image, and a try-before-you-buy image only in a flash
update boot. Everything else (the filesystem, the boot records, the identity
sector) is unpartitioned space, readable and writable like the rest. The table
is made by picotool, which is found as the AGENTS.md guide describes; where
picotool is missing or cannot run on this host (the SDK tools' picotool is an
x86-64 binary), encode() below writes the same table from the datasheet's
block format (5.9.1, 5.9.4) and the SDK's boot/picobin.h.
"""

import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
LAYOUT = REPO / "cmake" / "flash_layout.cmake"


layout_file = LAYOUT


def layout_value(name: str) -> int:
    """From the board's layout file, which builds on the common one: a value it
    sets itself wins, otherwise the common map's."""
    for f in (layout_file, LAYOUT):
        m = re.search(r"^set\(\s*%s\s+(0x[0-9A-Fa-f]+)" % name, f.read_text(), re.M)
        if m:
            return int(m.group(1), 16)
    sys.exit("could not find %s in %s" % (name, layout_file))


def find_picotool() -> str | None:
    found = shutil.which("picotool")
    if found:
        return found
    for root in ("~/gith/pico", "~/Source/gith/pico"):
        for p in pathlib.Path(root).expanduser().rglob("picotool"):
            if p.is_file() and p.stat().st_mode & 0o111:
                return str(p)
    return None


# ---- the table without picotool --------------------------------------------
# Constants from the SDK's boot/picobin.h and the datasheet's section 5.9.

BLOCK_START, BLOCK_END = 0xFFFFDED3, 0xAB123579
ITEM_PARTITION_TABLE = 0x0A          # picobin.h (the datasheet's table says 0x44, ENTRY_POINT's code)
ITEM_LAST = 0xFF
PERM = {"secure": (0x04000000, 0x08000000), "nonsecure": (0x10000000, 0x20000000),
        "bootloader": (0x40000000, 0x80000000)}
FAMILY_FLAG = {"rp2040": 0x4000, "absolute": 0x8000, "data": 0x10000, "rp2350-arm-s": 0x20000,
               "rp2350-riscv": 0x40000, "rp2350-arm-ns": 0x80000}
HAS_ID, HAS_NAME, LINK_A_PARTITION = 0x1, 0x1000, 1
UF2_FAMILY_ABSOLUTE = 0xE48BFF57
SECTOR = 4096


def perm_bits(perms: dict) -> int:
    bits = 0
    for who, rw in perms.items():
        r, w = PERM[who]
        bits |= (r if "r" in rw else 0) | (w if "w" in rw else 0)
    return bits


def size_bytes(text: str) -> int:
    m = re.fullmatch(r"(\d+)([KM]?)", text)
    if not m:
        sys.exit("size %r: expected a number with K or M" % text)
    return int(m.group(1)) * {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2)]


def encode(t: dict) -> bytes:
    """The PARTITION_TABLE block for table(): one item and LAST, a link of 0
    (a loop of one block), no hash or signature (the bootrom asks for neither
    without secure boot)."""
    un = t["unpartitioned"]
    flags = 0
    for f in un["families"]:
        flags |= FAMILY_FLAG[f]
    words = [perm_bits(un["permissions"]) | flags]
    for p in t["partitions"]:
        perms = perm_bits(p["permissions"])
        first = size_bytes(p["start"]) // SECTOR
        last = (size_bytes(p["start"]) + size_bytes(p["size"])) // SECTOR - 1
        pflags = HAS_ID | HAS_NAME
        for f in p["families"]:
            pflags |= FAMILY_FLAG[f]
        if "link" in p:
            kind, target = p["link"]
            if kind != "a":
                sys.exit("link %r: only links to an A partition are encoded here" % p["link"])
            pflags |= (LINK_A_PARTITION << 1) | (target << 3)
        words += [perms | first | (last << 13), perms | pflags, p["id"] & 0xFFFFFFFF, p["id"] >> 32]
        name = p["name"].encode()
        nb = bytes([len(name)]) + name
        nb += b"\0" * (-len(nb) % 4)
        words += list(struct.unpack("<%dI" % (len(nb) // 4), nb))
    item_words = 1 + len(words)
    header = ITEM_PARTITION_TABLE | (item_words << 8) | (len(t["partitions"]) << 24)   # not a singleton
    items = [header] + words
    last = ITEM_LAST | (len(items) << 8)
    block = [BLOCK_START] + items + [last, 0, BLOCK_END]
    data = struct.pack("<%dI" % len(block), *block)
    if len(data) > 0x280:
        sys.exit("partition table block is %d bytes; the bootrom reads at most 640" % len(data))
    return data


def uf2(data: bytes, addr: int = 0x10000000, family: int = UF2_FAMILY_ABSOLUTE) -> bytes:
    """`data` at `addr` as UF2 blocks of 256 bytes, with a family id."""
    chunks = [data[i:i + 256] for i in range(0, len(data), 256)]
    out = b""
    for n, c in enumerate(chunks):
        c = c + b"\xff" * (256 - len(c))
        head = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x00002000, addr + 256 * n, 256, n,
                           len(chunks), family)
        out += head + c + b"\0" * (476 - 256) + struct.pack("<I", 0x0AB16F30)
    return out


def table() -> dict:
    perms = {"secure": "rw", "nonsecure": "rw", "bootloader": "rw"}
    span = layout_value("LUGALOS_SLOT_SPAN")
    a, b = layout_value("LUGALOS_SLOT_A_PHYS"), layout_value("LUGALOS_SLOT_B_PHYS")
    return {
        "version": [1, 0],
        "unpartitioned": {"families": ["absolute"], "permissions": perms},
        "partitions": [
            {"name": "A", "id": 0, "start": "%dK" % (a // 1024), "size": "%dK" % (span // 1024),
             "families": ["rp2350-riscv"], "permissions": perms},
            {"name": "B", "id": 1, "start": "%dK" % (b // 1024), "size": "%dK" % (span // 1024),
             "families": ["rp2350-riscv"], "link": ["a", 0], "permissions": perms},
        ],
    }


def main() -> int:
    global layout_file
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "--layout":
        layout_file = pathlib.Path(args[1])
        args = args[2:]
    if len(args) != 1:
        sys.exit(__doc__)
    out = pathlib.Path(args[0])
    tool = find_picotool()
    r = None
    if tool:
        with tempfile.TemporaryDirectory() as d:
            j = pathlib.Path(d) / "pt.json"
            j.write_text(json.dumps(table()))
            try:
                r = subprocess.run([tool, "partition", "create", str(j), str(out)],
                                   capture_output=True, text=True)
            except OSError as e:          # a binary for another architecture
                print("%s cannot run here (%s); encoding the table itself" % (tool, e))
    if r is not None and r.returncode != 0 and "Exec format" not in r.stderr and "ld-linux" not in r.stderr:
        sys.exit("picotool partition create failed:\n" + r.stdout + r.stderr)
    if r is None or r.returncode != 0:
        if r is not None:
            print("picotool cannot run here; encoding the table itself")
        out.write_bytes(uf2(encode(table())))
    print("%s: %d bytes" % (out, out.stat().st_size))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
