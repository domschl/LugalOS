#!/usr/bin/env python3
"""The RP2350 partition table for cmake/flash_layout.cmake's map.

    rp2350_ptable.py [--layout cmake/flash_layout_rp2350_terminal.cmake] build/rp2350-wifi/ptable.uf2

Slots A and B are an A/B pair for the bootrom (datasheet 5.1.7): it boots the
higher-versioned valid image, and a try-before-you-buy image only in a flash
update boot. Everything else (the filesystem, the boot records, the identity
sector) is unpartitioned space, readable and writable like the rest. The table
is made by picotool, which is found as the AGENTS.md guide describes.
"""

import json
import pathlib
import re
import shutil
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


def find_picotool() -> str:
    found = shutil.which("picotool")
    if found:
        return found
    for root in ("~/gith/pico", "~/Source/gith/pico"):
        for p in pathlib.Path(root).expanduser().rglob("picotool"):
            if p.is_file() and p.stat().st_mode & 0o111:
                return str(p)
    sys.exit("picotool not found (see AGENTS.md section 2)")


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
    with tempfile.TemporaryDirectory() as d:
        j = pathlib.Path(d) / "pt.json"
        j.write_text(json.dumps(table()))
        r = subprocess.run([find_picotool(), "partition", "create", str(j), str(out)],
                           capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("picotool partition create failed:\n" + r.stdout + r.stderr)
    print("%s: %d bytes" % (out, out.stat().st_size))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
