#!/usr/bin/env python3
"""R6 on real silicon, over the air, against a real reference clock.

The QEMU test (`tests/runner.py`, "NTP Client: Epoch, Fixed Point, And A
Reply That Lies") already covers the arithmetic and the anti-forgery check
against a server it invents. What it cannot cover is the thing this suite
exists for: a *real* stratum-1 on the far side of a *real* radio, where the
round trip is measured in milliseconds rather than in QEMU's scheduling, and
where `long` is 32 bits.

That last detail is why this file exists at all. The QEMU test passed on RV64
while the same build on this board printed a truncated, confidently-signed
offset beside a clock it had just set correctly -- because RV32 and RP2350
have a 32-bit `long` and a first sync is an offset of decades. The QEMU test
now asserts the reported figure too, so that specific bug is caught in
software; this file is what would have caught it first.

What it asks, in the order the answers stop being interesting:

  * a sync against the reference succeeds, and reports stratum 1 with a
    reference id -- a GPS-disciplined server says something like "PPS0" or
    "GPS", which is the four-ASCII-character refid path a stratum-2 server
    would never exercise
  * a *second* sync immediately after reports a small offset. This is the
    real assertion: it proves the first one was actually applied and that
    the arithmetic has the right sign, neither of which "the command did not
    error" can tell you
  * the round trip is sane, which is the honest measure of what the radio
    costs a time query
  * a server that is not there times out rather than hanging

Run it against a board that is already on the network (identity record, or
brought up by hand):

    uv run test_ntp.py --server 192.168.178.23

The reference must be a real NTP server. There is no point pointing this at
the gateway if the gateway is not one -- the failure would be indistinguish-
able from a broken client, which is the mistake this docstring exists to
prevent.
"""

from __future__ import annotations

import argparse
import re
import sys
import time

import serial

import rp2350

DEFAULT_SERVER = "192.168.178.23"

# "  offset     : +9 ms" / "+1775 d 01:02:03.004" / "+27.500 s"
OFFSET_RE = re.compile(r"offset\s*:\s*([+-])(.+?)\s{2,}")
TRIP_RE = re.compile(r"round trip\s*:\s*(\d+)\s*ms")
STRATUM_RE = re.compile(r"stratum\s+(\d+)\s+\(([^)]*)\)")


def offset_ms(text: str) -> int | None:
    """Parses the offset back out of the report, in milliseconds.

    Deliberately parses the *rendered* form rather than asking the board for
    a machine-readable one: the rendering is where the 32-bit truncation
    lived, so a test that bypassed it would have missed the only bug this
    code has actually had."""
    m = OFFSET_RE.search(text)
    if not m:
        return None
    sign = -1 if m.group(1) == "-" else 1
    body = m.group(2).strip()

    md = re.fullmatch(r"(\d+) d (\d+):(\d+):(\d+)\.(\d+)", body)
    if md:
        d, h, mi, s, ms = (int(x) for x in md.groups())
        return sign * (((d * 24 + h) * 60 + mi) * 60 + s) * 1000 + sign * ms
    ms_only = re.fullmatch(r"(\d+) ms", body)
    if ms_only:
        return sign * int(ms_only.group(1))
    secs = re.fullmatch(r"(\d+)\.(\d+) s", body)
    if secs:
        return sign * (int(secs.group(1)) * 1000 + int(secs.group(2)))
    return None


def run_ntp(ser: serial.Serial, server: str, deadline: float = 12.0) -> str:
    ser.reset_input_buffer()
    ser.write(f"ntp {server}\n".encode())
    ser.flush()
    return rp2350.drain(ser, quiet=0.8, deadline=deadline,
                        kick_after=4.0).decode("utf-8", "replace")


def test_reference_sync(ports: rp2350.Rp2350Ports, server: str) -> tuple[str, bool, str]:
    name = f"ntp: sync against the reference at {server}"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            out = run_ntp(ser, server)
    except Exception as e:  # noqa: BLE001
        return (name, False, str(e))

    if "clock set" not in out:
        return (name, False, f"no sync: {out[-400:]}")
    m = STRATUM_RE.search(out)
    if not m:
        return (name, False, f"no stratum reported: {out[-300:]}")
    stratum, refid = int(m.group(1)), m.group(2)
    if stratum != 1:
        return (name, False,
                f"stratum {stratum}, expected 1 -- is {server} really the reference?")
    if refid.startswith("via "):
        return (name, False,
                f"stratum 1 but the refid is an address ({refid}); a reference "
                "clock's id is four ASCII characters")
    return (name, True, f"stratum {stratum} ({refid})")


def test_second_sync_is_small(ports: rp2350.Rp2350Ports, server: str,
                              limit_ms: int) -> tuple[str, bool, str]:
    """The assertion that actually proves the client works.

    A first sync can report anything and still leave the clock wrong -- the
    step might not have been applied, or applied with the wrong sign. A
    second sync moments later has to report a *small* offset, and can only
    do so if the first one landed."""
    name = f"ntp: a second sync reports under {limit_ms} ms"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            run_ntp(ser, server)          # settle the clock
            time.sleep(1.0)
            out = run_ntp(ser, server)
    except Exception as e:  # noqa: BLE001
        return (name, False, str(e))

    off = offset_ms(out)
    if off is None:
        return (name, False, f"could not parse the offset: {out[-400:]}")
    if abs(off) > limit_ms:
        return (name, False,
                f"{off:+d} ms after an immediately preceding sync -- the first "
                f"one did not take, or the sign is wrong")
    trip = TRIP_RE.search(out)
    trip_ms = int(trip.group(1)) if trip else -1
    if trip_ms < 0 or trip_ms > 2000:
        return (name, False, f"implausible round trip: {trip_ms} ms")
    return (name, True, f"offset {off:+d} ms, round trip {trip_ms} ms")


def test_unreachable_server(ports: rp2350.Rp2350Ports,
                            addr: str) -> tuple[str, bool, str]:
    """A server that is not there must cost seconds, not the session."""
    name = f"ntp: {addr} (nothing there) fails instead of hanging"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            t0 = time.time()
            out = run_ntp(ser, addr, deadline=20.0)
            elapsed = time.time() - t0
    except Exception as e:  # noqa: BLE001
        return (name, False, str(e))

    if "clock set" in out:
        return (name, False, f"it claims to have synced against nothing: {out[-300:]}")
    if "ntp:" not in out:
        return (name, False, f"no diagnosis printed: {out[-300:]}")
    if elapsed > 15.0:
        return (name, False, f"took {elapsed:.1f}s -- too close to a hang")
    reason = next((ln.strip() for ln in out.splitlines()
                   if ln.strip().startswith("ntp:") and "asking" not in ln), "?")
    return (name, True, f"{reason} after {elapsed:.1f}s")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=DEFAULT_SERVER,
                    help=f"a real NTP server to sync against (default {DEFAULT_SERVER})")
    ap.add_argument("--unreachable", default="192.168.178.99",
                    help="an address on the local segment with nothing on it")
    ap.add_argument("--limit-ms", type=int, default=250,
                    help="how large a second consecutive sync's offset may be")
    ap.add_argument("--console", help="Override auto-detected console port")
    args = ap.parse_args()

    print("======================================================================")
    print("        LugalOS NTP Client Against A Real Reference Clock (R6)")
    print("======================================================================")

    ports = rp2350.discover_ports(console=args.console)
    if ports is None:
        print("\n[!] No RP2350 detected. Nothing to test -- not a failure.")
        return 0
    print(f"\nDetected RP2350: console={ports.console}")
    print(f"Reference: {args.server}\n")

    results = [
        test_reference_sync(ports, args.server),
        test_second_sync_is_small(ports, args.server, args.limit_ms),
        test_unreachable_server(ports, args.unreachable),
    ]

    failed = 0
    for name, ok, detail in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
        if detail:
            print(f"         {detail}")
        failed += 0 if ok else 1

    print("\n----------------------------------------------------------------------")
    print(f"Result: {len(results) - failed} / {len(results)} Tests PASSED")
    print("======================================================================")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
