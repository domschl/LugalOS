#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""Write a LugalOS build to an ESP32-C6's flash, so it boots by itself.

45.4.3, plan/phase45_esp32c6.md. tools/c6run.py --kernel runs the kernel from RAM
for development (the flash half written, the RAM half delivered over USB each
time); this is the other thing: both halves into flash, the way a board is left
running. After it, a power cycle boots LugalOS with nothing attached but power.

    tools/c6flash.py build/esp32c6              write stage 2 and the OS image, verify, reset
    tools/c6flash.py build/esp32c6 --only osimage
    tools/c6flash.py build/esp32c6 --no-reset

The map is read from <build>/flash.manifest (cmake/flash_layout_esp32c6.cmake is
the one definition), never typed here:

    stage2   0x000000   the RAM half: boot code, .data -- the image the boot ROM
                        loads into SRAM and jumps to
    osimage  0x020000   .text + .rodata, executed in place through the flash MMU

Stage 2 is made here from lugalos-ram.elf with esptool's elf2image, which gives it
the ROM's image format (header, segments, checksum). The flash parameters in that
header (DIO, 80 MHz, 8 MB) are the ones this board's own factory image used.
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c6run  # noqa: E402  (find_port, esptool)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("build", help="a LugalOS build directory (build/esp32c6)")
    ap.add_argument("--port")
    ap.add_argument("--only", choices=["stage2", "osimage"], help="write just one image")
    ap.add_argument("--no-reset", action="store_true", help="leave the chip in the bootloader")
    a = ap.parse_args()

    man = {}
    for line in open(os.path.join(a.build, "flash.manifest")):
        f = line.split()
        if len(f) == 4:
            man[f[0]] = (int(f[1], 0), int(f[2], 0), f[3])
    port = c6run.find_port(a.port)

    # Stage 2: the RAM half as the ROM's image format. Flash parameters go in its header.
    ram_elf = os.path.join(a.build, "lugalos-ram.elf")
    stage2 = os.path.join(a.build, man["stage2"][2])
    r = c6run.esptool("elf2image", "--flash-mode", "dio", "--flash-freq", "40m",
                      "--flash-size", "8MB", "-o", stage2, ram_elf)
    if r.returncode != 0:
        sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-600:])
    if os.path.getsize(stage2) > man["stage2"][1]:
        sys.exit("stage 2 is %d bytes; its flash partition is %d" % (os.path.getsize(stage2), man["stage2"][1]))

    args = []
    for name in ("stage2", "osimage"):
        if a.only and a.only != name:
            continue
        base, size, art = man[name]
        path = os.path.join(a.build, art)
        if os.path.getsize(path) > size:
            sys.exit("%s is %d bytes; its flash partition is %d" % (art, os.path.getsize(path), size))
        args += ["%#x" % base, path]
        print("  %-8s %#08x  %s (%d bytes)" % (name, base, art, os.path.getsize(path)))

    cmd = ["--port", port, "write-flash", "--flash-mode", "dio", "--flash-freq", "40m", "--flash-size", "8MB"] + args
    r = c6run.esptool(*cmd, timeout=600)
    sys.stdout.write(r.stdout[-400:])
    if r.returncode != 0:
        sys.exit("write-flash failed:\n" + r.stderr[-600:])
    # The stamp c6run --kernel keeps is about the *flash half it last wrote*; this wrote it too.
    try:
        os.remove(os.path.join(a.build, ".c6run.flashed"))
    except OSError:
        pass
    print("done%s." % ("" if not a.no_reset else " (left in the bootloader)"))


if __name__ == "__main__":
    main()
