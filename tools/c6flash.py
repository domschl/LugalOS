#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial"]
# ///
"""Write a LugalOS build to an ESP32-C6's flash, so it boots by itself.

tools/c6run.py --kernel runs the kernel from RAM for development (the flash
half written, the RAM half delivered over USB each time); this is the other
thing: a board left running, with nothing attached but power.

    tools/c6flash.py build/esp32c6              the loader, the boot records, slot A; verify, reset
    tools/c6flash.py build/esp32c6 --only slot  slot A only (the loader and the records kept)
    tools/c6flash.py build/esp32c6 --only loader the loader only (the records and the slots kept)
    tools/c6flash.py build/esp32c6 --no-reset

The map is read from <build>/flash.manifest (cmake/flash_layout_esp32c6*.cmake is
the one definition), never typed here:

    loader   0x000000   arch/riscv/esp32c6/loader.c: what the boot ROM loads;
                        it boots a slot. Written through the header pins only.
    bootrec  0x010000   the boot records: erased, so the board boots slot A
    slot_a   0x020000   the update image (tools/lota.py), laid out as a slot

Slot B is left alone, and updates over 9P (tools/lota.py push) are how it gets
written. The loader and the kernel's RAM half are made here with esptool's
elf2image, which gives them the ROM's image format; the flash parameters in the
loader's header (DIO, 40 MHz, the part's size) tell the ROM how big the chip is.
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c6run  # noqa: E402  (find_port, esptool)
import lota   # noqa: E402  (the update image)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("build", help="a LugalOS build directory (build/esp32c6)")
    ap.add_argument("--port")
    ap.add_argument("--board", help="zero, nano, ... -- a port from ~/.config/lugalos/c6-ports.env")
    ap.add_argument("--only", choices=["loader", "slot"], help="write just the loader, or just slot A")
    ap.add_argument("--no-reset", action="store_true", help="leave the chip in the bootloader")
    a = ap.parse_args()
    build = Path(a.build)

    man = lota.read_manifest(build)
    if "loader" not in man:
        sys.exit(f"{build}/flash.manifest has no loader: rebuild the build directory")
    port = c6run.find_port(a.port, a.board)
    flash_mb = "%dMB" % (int(man["flash"][1], 0) >> 20)

    writes = []
    if a.only in (None, "loader"):
        loader_elf = build / "lugalos-loader.elf"
        loader_img = build / man["loader"][2]
        r = c6run.esptool("elf2image", "--flash-mode", "dio", "--flash-freq", "40m",
                          "--flash-size", flash_mb, "-o", str(loader_img), str(loader_elf))
        if r.returncode != 0:
            sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-600:])
        if loader_img.stat().st_size > int(man["loader"][1], 0):
            sys.exit("the loader does not fit its partition")
        writes.append(("loader", int(man["loader"][0], 0), loader_img))
    if a.only is None:
        # A fresh board: no record, so the loader boots slot A, which comes next.
        erased = build / "bootrec-erased.bin"
        erased.write_bytes(b"\xff" * int(man["bootrec"][1], 0))
        writes.append(("bootrec", int(man["bootrec"][0], 0), erased))
    if a.only in (None, "slot"):
        image, info = lota.make_image(build)
        (build / "lugalos-ota.img").write_bytes(image)
        offsets = (int(man["parts"][0], 0), int(man["parts"][1], 0))
        slot = lota.slot_image(image, offsets)
        if len(slot) > int(man["slot_a"][1], 0):
            sys.exit("the image does not fit slot A")
        slot_path = build / "slot-a.bin"
        slot_path.write_bytes(slot)
        writes.append(("slot_a", int(man["slot_a"][0], 0), slot_path))
        print("  image: %s, build %s, sha256 %s" % (info["board"], info["build_id"], info["sha256"][:16]))

    args = []
    for name, base, path in writes:
        args += ["%#x" % base, str(path)]
        print("  %-8s %#08x  %s (%d bytes)" % (name, base, path.name, path.stat().st_size))
    # A chip that came here through `reboot download` has bit 30 of LP_AON_SYS_CFG set; it is
    # always-on, and a reset over USB-JTAG keeps it, so the board would only ever restart into
    # the ROM's download mode. Cleared first: esptool's reset after write-flash is the one that
    # reliably restarts the chip (one issued later, from its flasher stub, did not).
    r = c6run.esptool("--port", port, "--after", "no-reset",
                      "write-mem", "0x600B1034", "0", "0x40000000", timeout=60)
    if r.returncode != 0:
        print("note: could not clear the force-download bit:", (r.stderr or r.stdout)[-200:])
    cmd = ["--port", port, "write-flash", "--flash-mode", "dio", "--flash-freq", "40m", "--flash-size", flash_mb]
    if a.no_reset:
        cmd = ["--port", port, "--after", "no-reset", "write-flash", "--flash-mode", "dio",
               "--flash-freq", "40m", "--flash-size", flash_mb]
    r = c6run.esptool(*(cmd + args), timeout=600)
    sys.stdout.write(r.stdout[-400:])
    if r.returncode != 0:
        sys.exit("write-flash failed:\n" + r.stderr[-600:])
    # The stamp c6run --kernel keeps is about the *flash half it last wrote*; this wrote it too.
    try:
        os.remove(build / ".c6run.flashed")
    except OSError:
        pass
    print("done%s." % ("" if not a.no_reset else " (left in the bootloader)"))


if __name__ == "__main__":
    main()
