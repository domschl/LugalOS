#!/usr/bin/env python3
"""Prove R5's netif over the air: an authenticated 9P session to a board
that is on the network via WiFi, not a wire.

The gateway suite (test_gateway.py) already covers 9P-over-TCP in depth;
this deliberately does not repeat it. What is new here is the transport
underneath -- PIO gSPI, an uploaded firmware blob, WPA2 -- so this asks the
questions that distinguish "the radio carries frames" from "the radio
carries a few frames":

  * ICMP at all, and under a burst
  * an authenticated 9P session (auth is the same gate as on the wire)
  * a directory read, which is the multi-hundred-byte reply shape that
    breaks first when a transport is marginal
  * a sustained soak, because a radio that works for a second and a radio
    that works for fifteen minutes are different claims

Run it against a board that has already been brought up by hand:

    lsh> wifi probe
    lsh> wifi join <ssid> <psk>
    lsh> lisp
         (net-config "192.168.178.21" "255.255.255.0" "192.168.178.1")
    lsh> p9key 000102030405060708090a0b0c0d0e0f
    lsh> net listen 564

    uv run test_wifi.py 192.168.178.21 [--soak-minutes 15]

There is no console interaction here on purpose: everything below goes over
the air, so a pass means the network path worked, not that a serial cable
did.
"""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))

import p9lib  # noqa: E402
from p9lib import Session, connect_tcp  # noqa: E402

DEFAULT_PORT = 564
DEFAULT_KEY = "000102030405060708090a0b0c0d0e0f"


class Board:
    def __init__(self, host: str, port: int, key: str):
        self.host = host
        self.port = port
        # p9lib wants the raw key, not its hex spelling.
        self.key = bytes.fromhex(key)


def session(b: Board, timeout: float = 20.0) -> Session:
    return Session(connect_tcp(b.host, b.port, timeout=timeout), key=b.key)


def ping(host: str, count: int, interval: str = "0.3") -> tuple[int, str]:
    """Returns (loss_percent, summary_line)."""
    r = subprocess.run(
        ["ping", "-c", str(count), "-i", interval, "-W", "2000", host],
        capture_output=True, text=True,
    )
    loss, rtt = 100, ""
    for line in r.stdout.splitlines():
        if "packet loss" in line:
            try:
                loss = int(float(line.split("%")[0].split()[-1]))
            except (ValueError, IndexError):
                pass
        if "round-trip" in line or "rtt" in line:
            rtt = line.strip()
    return loss, rtt


def test_icmp(b: Board) -> tuple[str, bool, str]:
    name = "icmp: 20 echoes"
    loss, rtt = ping(b.host, 20)
    if loss != 0:
        return name, False, f"{loss}% loss"
    return name, True, rtt or "20/20"


def test_auth_required(b: Board) -> tuple[str, bool, str]:
    """The auth gate is not transport-specific, but a driver that mangles
    payloads could make a refusal look like a success. Cheap to confirm."""
    name = "auth: unauthenticated attach refused"
    c = connect_tcp(b.host, b.port, timeout=20.0)
    try:
        c.version()
        try:
            c.attach(1, aname="/", uname="lugal")
        except p9lib.P9Error as e:
            if "auth" in str(e).lower():
                return name, True, str(e)[:80]
            return name, False, f"refused, but not for authentication: {e}"
        return name, False, "attach succeeded WITHOUT authentication"
    finally:
        c.close()


def test_9p_read(b: Board) -> tuple[str, bool, str]:
    name = "9p: authenticated session, /proc/version"
    s = session(b)
    try:
        v = s.read("/proc/version").decode(errors="replace").strip()
        if "LugalOS" not in v:
            return name, False, f"unexpected /proc/version: {v[:80]!r}"
        return name, True, v
    finally:
        s.close()


def test_directory_read(b: Board) -> tuple[str, bool, str]:
    """Several hundred bytes of stat entries in one reply -- the size that
    exposes a transport which is fine for short messages."""
    name = "9p: directory read (multi-entry Rread)"
    s = session(b)
    try:
        root = [e.name for e in s.listdir("/")]
        proc = [e.name for e in s.listdir("/proc")]
        if "proc" not in root:
            return name, False, f"/ has no proc: {root}"
        if "version" not in proc:
            return name, False, f"/proc has no version: {proc}"
        return name, True, f"/ has {len(root)} entries, /proc {len(proc)}"
    finally:
        s.close()


def test_repeated_sessions(b: Board) -> tuple[str, bool, str]:
    """Ten sessions in a row. A transport that leaks a buffer or loses a
    reply usually survives one and not ten.

    Paced deliberately. net/tcp.c keeps TCP_MAX_CONNS = 2 slots and holds a
    closed connection in TIME_WAIT for TCP_TIME_WAIT_MS = 2000 ms, so
    hammering connect() in a loop exhausts both slots and the board
    correctly refuses -- which is the stack behaving as designed, not the
    radio failing. An earlier version of this test did exactly that and
    reported a ConnectionRefusedError as a transport fault."""
    name = "9p: 10 consecutive sessions"
    gap = 2.2   # just over TIME_WAIT, so a slot is always free
    for i in range(10):
        try:
            s = session(b)
        except ConnectionRefusedError:
            # Both slots still in TIME_WAIT; give them a moment rather
            # than calling it a failure.
            time.sleep(gap)
            s = session(b)
        try:
            s.read("/proc/version")
        except Exception as e:  # noqa: BLE001
            return name, False, f"session {i + 1} failed: {e}"
        finally:
            s.close()
        if i < 9:
            time.sleep(gap)
    return name, True, "10/10, paced past TIME_WAIT"


def test_soak(b: Board, minutes: float) -> tuple[str, bool, str]:
    """R5's own criterion. Alternates ICMP and a real 9P read so that both
    the data path and the stack above it stay exercised, rather than
    proving only that the radio still associates."""
    name = f"soak: {minutes:g} minutes of traffic"
    deadline = time.time() + minutes * 60
    pings = pings_lost = sessions = failures = 0
    last_err = ""
    while time.time() < deadline:
        loss, _ = ping(b.host, 3, interval="0.2")
        pings += 3
        pings_lost += 3 * loss // 100
        try:
            s = session(b, timeout=25.0)
            try:
                s.read("/proc/version")
                sessions += 1
            finally:
                s.close()
        except Exception as e:  # noqa: BLE001
            failures += 1
            last_err = str(e)[:120]
        time.sleep(5)
    detail = (f"{pings - pings_lost}/{pings} echoes, "
              f"{sessions} sessions, {failures} failed")
    if failures or pings_lost:
        return name, False, detail + (f"; last error: {last_err}" if last_err else "")
    return name, True, detail


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="the board's IP address on the WiFi network")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--key", default=DEFAULT_KEY, help="9P auth key, as hex")
    ap.add_argument("--soak-minutes", type=float, default=0,
                    help="also run the sustained soak, for this many minutes")
    args = ap.parse_args()

    b = Board(args.host, args.port, args.key)

    try:
        socket.create_connection((b.host, b.port), timeout=6.0).close()
    except OSError as e:
        print(f"[!] {b.host}:{b.port} is not accepting connections ({e}).")
        print("    On the board: wifi probe / wifi join / (net-config ...) /")
        print("                  p9key <hex> / net listen 564")
        return 1

    tests = [test_icmp, test_auth_required, test_9p_read,
             test_directory_read, test_repeated_sessions]

    failed = 0
    for t in tests:
        try:
            name, ok, detail = t(b)
        except Exception as e:  # noqa: BLE001
            name, ok, detail = t.__name__, False, f"raised {type(e).__name__}: {e}"
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))
        failed += not ok

    if args.soak_minutes > 0:
        name, ok, detail = test_soak(b, args.soak_minutes)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} -- {detail}")
        failed += not ok

    total = len(tests) + (1 if args.soak_minutes > 0 else 0)
    print(f"\nResult: {total - failed} / {total} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
