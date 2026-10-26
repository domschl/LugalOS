#!/usr/bin/env python3
"""K210 silicon truth probe host (65.1).

SRAM-loads tools/minimal_k210.elf through the BROM ISP protocol (via
kflash -- the reference implementation we replace in 65.2), captures
the probe's RPT lines, and derives the numbers the probe cannot
compute alone (the mtime frequency, from the host's own wall clock).

Never opens the port naively: the Maix Bit's CH552 drives the K210
reset/boot lines from DTR/RTS, so the open order and control-signal
states are part of the measurement (recorded in the output).
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time

import serial

DEFAULT_ELF = "build/k210/minimal_k210.elf"
# Baud rates to try for the post-boot console. kflash leaves the BROM
# console at the ISP rate (default 1.5M) before jumping to our image,
# and the probe does not reprogram the divisor, so 1.5M is tried first.
CANDIDATE_BAUDS = [1500000, 115200, 921600, 460800]
CAPTURE_SECONDS = 12.0


def find_port(override: str | None) -> str:
    if override:
        return override
    for pat in ("*1a86*", "*USB2.0-Serial*"):
        hits = sorted(glob.glob(f"/dev/serial/by-id/{pat}"))
        if hits:
            return os.path.realpath(hits[0])
    sys.exit("no CH552/CH340 (1a86:) serial adapter found in /dev/serial/by-id")


def find_kflash() -> str:
    return shutil.which("kflash") or os.path.expanduser("~/.local/bin/kflash")


def kflash_sram_boot(port: str, elf: str) -> None:
    cmd = [find_kflash(), "-s", "-B", "bit", "-p", port, elf]
    print("$", " ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    tail = [ln for ln in (proc.stdout + proc.stderr).splitlines() if ln.strip()]
    for ln in tail[-6:]:
        print("   kflash|", ln)
    if proc.returncode != 0:
        raise RuntimeError(f"kflash failed (rc={proc.returncode})")


def _drain_ready(ser: serial.Serial, wait: float = 0.3) -> bool:
    t_end = time.monotonic() + wait
    buf = b""
    while time.monotonic() < t_end:
        buf += ser.read(4096)
        if b"K210-PROBE READY" in buf:
            return True
        time.sleep(0.01)
    return False


def capture(port: str, elf: str, baud: int) -> tuple[dict[str, str], list[tuple[float, bytes]], bytes]:
    """Boot the probe and capture its report at the given baud.

    Raises if the banner never appears (wrong baud, port held, board
    in the wrong boot state).
    """
    kflash_sram_boot(port, elf)
    # open with DTR/RTS de-asserted *before* the open call: pyserial
    # would otherwise pulse them asserted and the CH552 maps them to
    # the K210 boot/reset pins.
    ser = serial.Serial(baudrate=baud, timeout=0.1)
    ser.port = port
    ser.dtr = False
    ser.rts = False
    ser.open()
    # poke the probe until it says READY, then let it report
    got_ready = False
    t_poke = time.monotonic()
    while time.monotonic() - t_poke < 4.0:
        ser.write(b"g")
        time.sleep(0.1)
        ser.read(4096)
        if _drain_ready(ser):
            got_ready = True
            break
    chunks: list[bytes] = []
    stamps: list[tuple[float, bytes]] = []
    t_end = time.monotonic() + CAPTURE_SECONDS
    while time.monotonic() < t_end:
        data = ser.read(4096)
        if data:
            chunks.append(data)
            stamps.append((time.monotonic(), data))
        else:
            time.sleep(0.002)
    ser.close()
    if not got_ready:
        raise RuntimeError(f"probe never reported READY at {baud} baud")
    blob = b"".join(chunks)
    if b"K210-PROBE" not in blob:
        raise RuntimeError(f"no probe banner at {baud} baud "
                           f"({len(blob)} bytes captured)")
    rpt: dict[str, str] = {}
    for line in blob.splitlines():
        s = line.decode("ascii", "replace").strip()
        if s.startswith("RPT "):
            parts = s[4:].split(" ", 1)
            rpt[parts[0]] = parts[1] if len(parts) > 1 else ""
    return rpt, stamps, blob


def marker_time(stamps: list[tuple[float, bytes]], blob: bytes,
                marker: bytes) -> float | None:
    idx = blob.find(marker)
    if idx < 0:
        return None
    acc = 0
    for t, chunk in stamps:
        if acc + len(chunk) >= idx + 1:
            return t
        acc += len(chunk)
    return None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--baud", type=int, default=None,
                    help="skip the baud scan and use this one")
    args = ap.parse_args()

    port = find_port(args.port)
    print(f"port: {port}\nkflash: {find_kflash()}\nelf: {args.elf}")

    last_err: Exception | None = None
    for baud in ([args.baud] if args.baud else CANDIDATE_BAUDS):
        print(f"\n=== attempt at {baud} baud ===")
        try:
            rpt, stamps, blob = capture(port, args.elf, baud)
        except Exception as exc:  # noqa: BLE001 - report and retry
            last_err = exc
            print(f"  failed: {exc}")
            time.sleep(0.5)
            continue

        print(f"\n---- report ({baud} baud) ----")
        for k in sorted(rpt):
            print(f"  {k:35s} {rpt[k]}")

        t0 = marker_time(stamps, blob, b"MK0")
        t1 = marker_time(stamps, blob, b"MK1")
        m0 = re.search(rb"MK0 (\d+)", blob)
        m1 = re.search(rb"MK1 (\d+)", blob)
        if t0 and t1 and m0 and m1 and t1 > t0:
            mt0, mt1 = int(m0.group(1)), int(m1.group(1))
            hz = (mt1 - mt0) / (t1 - t0)
            print(f"\n  wall between markers      {1000*(t1-t0):.1f} ms")
            print(f"  mtime ticks between     {mt1} - {mt0} = {mt1 - mt0}")
            print(f"  => mtime frequency      ~{hz/1e6:.3f} MHz")
        else:
            print("  (markers not found)")
        print("\nPROBE CAPTURE OK")
        return

    sys.exit(f"all attempts failed: {last_err}")


if __name__ == "__main__":
    main()
