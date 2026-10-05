#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""Load a RAM image onto an ESP32-C6 and watch its console.

45.1, plan/phase45_esp32c6.md. The C6 counterpart of tools/p4run.py, and much
smaller, because the Waveshare ESP32-C6-Zero is simpler to talk to than the
P4-NANO: its USB-C port goes straight to the chip's USB-Serial/JTAG
peripheral (303a:1001), so there is one cable and one port for reset, loading
and console, and esptool resets the chip itself over it. None of the P4's
CH34x modem-line hazards apply (see AGENTS.md section 3) -- but this script is
still the one way to talk to the board, so that a hazard discovered later is
fixed in one place.

    tools/c6run.py --listen                  just watch the console
    tools/c6run.py IMAGE.elf                 load into RAM and run, watch 8 s
    tools/c6run.py IMAGE.elf --listen-secs 20
    tools/c6run.py --ports                   show what was found

**Nothing here writes flash.** `esptool load-ram` delivers the image into HP
SRAM and jumps to it; a reset restores whatever is in flash.

Env: LUGALOS_C6_PORT overrides autodetection.
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

import serial

# USB-Serial/JTAG enumerates with this product string on Linux.
BYID = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00"


def find_port(explicit):
    p = explicit or os.environ.get("LUGALOS_C6_PORT")
    if p:
        return p
    found = sorted(glob.glob(BYID))
    if not found:
        sys.exit("no USB-Serial/JTAG device found (looked for %s)" % BYID)
    if len(found) > 1:
        sys.exit("several candidates, pick one with --port:\n  " + "\n  ".join(found))
    return found[0]


def esptool(*args, timeout=120):
    exe = shutil.which("esptool")
    cmd = [exe] if exe else ["uv", "tool", "run", "--from", "esptool", "esptool"]
    return subprocess.run(cmd + ["--chip", "esp32c6", *args],
                          capture_output=True, text=True, timeout=timeout)


def image_for(path):
    """ELF in, image out, regenerated unconditionally -- the lesson of
    tools/p4run.py image_for(): never load a stale artifact."""
    path = os.path.abspath(path)
    if not path.endswith(".elf"):
        return path
    img = path[:-4] + ".img"
    r = esptool("elf2image", "-o", img, path)
    if r.returncode != 0:
        sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-600:])
    print("image: %s (regenerated from %s)" % (os.path.basename(img), os.path.basename(path)))
    return img


def load(port, img, tries=3):
    # --no-stub: the stub flasher would live in the same SRAM we are about to
    # write. --after no-reset: a reset would discard what we just loaded.
    for n in range(1, tries + 1):
        r = esptool("--port", port, "--after", "no-reset", "--no-stub", "load-ram", img)
        if r.returncode == 0:
            print("loaded (attempt %d)" % n)
            return True
        time.sleep(0.7)
    print("FAILED to load:\n" + (r.stderr or r.stdout)[-600:])
    return False


def listen(port, secs):
    # On USB-Serial/JTAG, RTS high while DTR is low is the chip's reset line
    # (esptool's "UnixTightReset"). pyserial applies DTR before RTS on open,
    # and the kernel raises both on open, so asking for DTR *low* passes
    # through (DTR=0, RTS=1) and resets the board -- found 2026-10-05: the
    # loaded program was reset with `rst:0x15 (USB_UART_HPSYS)` the moment the
    # monitor opened the port. DTR high / RTS low never passes through it.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = True
    s.rts = False
    s.open()
    out = bytearray()
    end = time.time() + secs
    try:
        while time.time() < end:
            d = s.read(256)
            if d:
                out += d
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        s.close()
    return out.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?", help=".elf or .img to load into RAM and run")
    ap.add_argument("--port")
    ap.add_argument("--listen", action="store_true", help="watch the console and exit")
    ap.add_argument("--listen-secs", type=float, default=8.0)
    ap.add_argument("--ports", action="store_true")
    ap.add_argument("--expect", default="[C6_MINIMAL]", help="text the program must print (default: %(default)s)")
    a = ap.parse_args()

    if a.ports:
        print("\n".join(sorted(glob.glob(BYID))) or "(none)")
        return
    port = find_port(a.port)
    if a.image:
        if not load(port, image_for(a.image)):
            sys.exit(1)
    elif not a.listen:
        ap.error("give an IMAGE, or --listen")
    text = listen(port, a.listen_secs)
    if a.image and a.expect not in text:
        sys.exit("\nloaded, but the program's banner never appeared")


if __name__ == "__main__":
    main()
