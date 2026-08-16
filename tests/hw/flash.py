#!/usr/bin/env python3
"""Flash a connected RP2350 without touching the BOOTSEL button.

Uses the "1200-baud touch" the firmware implements (drivers/usb_cdc.c): open
the console CDC port at 1200 baud, drop DTR, and the device reboots itself
into BOOTSEL via the bootrom. The board then enumerates as a USB mass-storage
volume, the UF2 is copied onto it, and it reboots into the new firmware.

    uv run flash.py                      # flash build/rp2350/lugalos.uf2
    uv run flash.py --uf2 path/to.uf2
    uv run flash.py --verify             # also check /proc/buildid afterwards

Bootstrap note: this only works if the firmware *already on the board*
implements the touch. Flashing onto a board that predates it -- or one whose
touch is broken -- needs one manual BOOTSEL press, after which this takes
over. The script says so explicitly rather than hanging.
"""

from __future__ import annotations

import argparse
import glob
import re
import shutil
import sys
import time
from pathlib import Path

import serial

import rp2350

REPO_ROOT = rp2350.REPO_ROOT
DEFAULT_UF2 = REPO_ROOT / "build" / "rp2350" / "lugalos.uf2"


def bootsel_volumes() -> list[str]:
    """Mounted volumes that look like an RP2350 in BOOTSEL mode.

    The label differs across silicon and bootrom versions (RP2350, RPI-RP2,
    RP2-…), so this matches loosely rather than pinning one name and silently
    failing on a board that presents a different one.

    Three auto-mount conventions covered: macOS (/Volumes/<label>),
    /media/<user>/<label> (some Linux distros' older automounters), and
    /run/media/<user>/<label> (udisks2's default on most current
    distros -- Arch among them). Found missing the third the first time
    this ran against a non-macOS host with the board already mounted: the
    mount was real and correct, this glob just never looked there. """
    out = []
    for v in (glob.glob("/Volumes/*") + glob.glob("/media/*/*")
              + glob.glob("/run/media/*/*")):
        base = Path(v).name.upper()
        if base.startswith("RP") or base.startswith("RPI-") or "PICO" in base:
            out.append(v)
    return out


def touch_1200(port: str) -> None:
    """Open at 1200 baud and drop DTR. The device detaching mid-call is the
    expected outcome, so serial errors here are progress, not failure."""
    try:
        s = serial.Serial(port, 1200)
        time.sleep(0.2)
        s.dtr = False
        time.sleep(0.3)
        s.close()
    except Exception:
        pass


def wait_for_bootsel(timeout: float = 15.0) -> str | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        vols = bootsel_volumes()
        if vols:
            # Give the mount a moment to settle before writing to it.
            time.sleep(0.7)
            return vols[0]
        time.sleep(0.3)
    return None


def wait_for_console(timeout: float = 25.0) -> str | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        ports = rp2350.discover_ports()
        if ports is not None:
            return ports.console
        time.sleep(0.5)
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--uf2", default=str(DEFAULT_UF2), help="UF2 image to flash")
    ap.add_argument("--console", help="Override auto-detected console port")
    ap.add_argument("--verify", action="store_true",
                    help="Read /proc/buildid afterwards and compare with the local build")
    args = ap.parse_args()

    uf2 = Path(args.uf2)
    if not uf2.exists():
        print(f"[!] No such UF2: {uf2}")
        return 1
    print(f"[*] Image: {uf2} ({uf2.stat().st_size} bytes)")

    vol = bootsel_volumes()
    if vol:
        print(f"[*] Board is already in BOOTSEL ({vol[0]}) -- skipping the touch")
        vol = vol[0]
    else:
        ports = rp2350.discover_ports(console=args.console)
        if ports is None:
            print("[!] No RP2350 console port found, and no BOOTSEL volume mounted.")
            print("    Connect the board, or hold BOOTSEL while plugging it in.")
            return 1
        print(f"[*] Touching {ports.console} at 1200 baud to request BOOTSEL...")
        touch_1200(ports.console)
        vol = wait_for_bootsel()
        if not vol:
            print("[!] The board did not enter BOOTSEL.")
            print("    Most likely its current firmware predates the 1200-baud touch, or")
            print("    implements it against the RP2040-only bootrom symbol (which returns")
            print("    NULL on RP2350 -- see arch/riscv/rp2350/bootrom.c).")
            print("    Flash once manually: hold BOOTSEL while connecting, copy the UF2,")
            print("    and this script will work from then on.")
            return 1
        print(f"[*] BOOTSEL volume mounted: {vol}")

    shutil.copyfile(uf2, Path(vol) / uf2.name)
    print("[*] UF2 copied; board is rebooting into the new firmware...")

    console = wait_for_console()
    if not console:
        print("[!] Board did not re-enumerate within the timeout (it may still be fine).")
        return 1
    print(f"[*] Console back at {console}")

    if args.verify:
        want = rp2350.local_build_id()
        time.sleep(1.0)
        try:
            with serial.Serial(console, 115200, timeout=2) as ser:
                ser.dtr = True
                time.sleep(0.4)
                ser.reset_input_buffer()
                ser.write(b"cat /proc/buildid\n")
                ser.flush()
                out = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", "replace")
            m = re.search(r"(\d+\.\d+\.\d+)\s+(\S+)", out.replace("cat /proc/buildid", ""))
            got = m.group(2) if m else None
            if got == want:
                print(f"[*] Verified: board reports build {got}")
            else:
                print(f"[!] Build id mismatch: board={got} local={want}")
                return 1
        except Exception as e:
            print(f"[!] Verification failed: {e}")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
