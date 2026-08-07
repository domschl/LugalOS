"""Shared helpers for hardware-in-the-loop RP2350 testing (A3b:
plan/phase5_distributed_design.md's M5/T3 -- link_usb_cdc and p9share
verified against real silicon, not just QEMU). There is no QEMU equivalent
for link_usb_cdc (ACM1/EP4 has no host-side USB device model in this
project's QEMU invocations), so this directory is the only place that code
path is exercised at all.

Run via `uv run test_rp2350.py` from this directory (`uv sync` first pulls
in pyserial from the committed lockfile). Every test here is written to be
*skipped*, not failed, when no RP2350 is attached -- see discover_ports().
"""

from __future__ import annotations

import glob
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import serial

# tests/p9lib.py lives one directory up; it's the same independent 9P
# client tests/runner.py uses against QEMU, reused as-is here rather than
# duplicated -- protocol bugs should only ever need fixing in one place.
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests"))
import p9lib  # noqa: E402


class SerialSocketAdapter:
    """Adapts a pyserial `Serial` to the minimal socket-like interface
    p9lib.P9Client expects (sendall/recv/settimeout/close).

    recv(n) honors the real socket.recv() contract of "at most n bytes,
    possibly fewer" -- p9lib's _recv_exact() buffers by looping until it
    has exactly what it asked for, which silently breaks (a struct.unpack
    on a too-long buffer) if a mock over-delivers past `n`. Worth calling
    out because it's exactly what an earlier, ad hoc version of this
    adapter got wrong."""

    def __init__(self, ser: serial.Serial) -> None:
        self._s = ser

    def sendall(self, data: bytes) -> None:
        self._s.write(data)
        self._s.flush()

    def recv(self, n: int) -> bytes:
        data = self._s.read(1)
        if not data:
            return b""
        extra = min(self._s.in_waiting, n - 1)
        if extra > 0:
            data += self._s.read(extra)
        return data

    def settimeout(self, t: float) -> None:
        self._s.timeout = t

    def close(self) -> None:
        self._s.close()


def warm_up_9p(client: p9lib.P9Client, adapter: SerialSocketAdapter,
                attempts: int = 5, attempt_timeout: float = 1.0, settle_timeout: float = 5.0) -> bool:
    """The first 9P frame sent to a freshly (re)opened CDC-ACM port
    occasionally gets silently dropped by the device -- observed in testing
    against real RP2350 hardware, most likely a DATA0/DATA1 toggle
    assumption mismatch between the host driver's fresh `open()` and the
    device's own toggle state, which has persisted since USB enumeration
    and has no protocol-level reason to resync just because a *host-side*
    file descriptor closed and reopened (no bus reset occurs). Tversion is
    cheap and idempotent (it resets the server's fid table either way, by
    design -- see fs/9p.c), so retrying it a few times with a short timeout
    is a safe, low-risk way past a dropped first frame without chasing the
    firmware-level root cause blind, matching how tests/runner.py's own
    QEMU-facing tests already retry a not-yet-ready socket/chardev rather
    than assuming a fixed boot delay is always enough.

    Returns True once a real Rversion comes back; leaves `adapter`'s
    timeout at `settle_timeout` for the caller's actual work either way."""
    for _ in range(attempts):
        adapter.settimeout(attempt_timeout)
        try:
            client.version()
            adapter.settimeout(settle_timeout)
            return True
        except p9lib.P9Error:
            continue
    adapter.settimeout(settle_timeout)
    return False


def drain(ser: serial.Serial, quiet: float = 0.4, deadline: float = 3.0) -> bytes:
    """Reads until `quiet` seconds pass with nothing new arriving, or
    `deadline` total seconds elapse. For human-readable console output
    (shell banners, help text, ...) only -- 9P frames go through
    SerialSocketAdapter + p9lib instead, which know their own exact framing
    and don't need to guess "is more coming?" from silence."""
    out = b""
    end = time.time() + deadline
    last = time.time()
    while time.time() < end:
        ser.timeout = 0.1
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            last = time.time()
        elif time.time() - last > quiet:
            break
    return out


@dataclass
class Rp2350Ports:
    console: str          # ACM0 -- interactive lsh shell
    net: str               # ACM1 -- link_usb_cdc's 9P endpoint
    uart: str | None        # CP2101/CP2102 dongle, if present (p9share needs this)


def _candidate_ports() -> tuple[list[str], list[str]]:
    """Returns (acm_like, uart_dongle_like) candidate device paths, covering
    both macOS (`/dev/tty.usbmodem*`, `/dev/tty.usbserial-*`) and Linux
    (`/dev/ttyACM*`, `/dev/ttyUSB*`) naming conventions."""
    acm = sorted(set(glob.glob("/dev/tty.usbmodem*") + glob.glob("/dev/ttyACM*")))
    uart = sorted(set(glob.glob("/dev/tty.usbserial-*") + glob.glob("/dev/ttyUSB*")))
    return acm, uart


# A complete, self-contained Tversion frame (msize=4096, "9P2000", tag=1),
# used to probe an unidentified port -- see _probe_port()'s docstring for
# why it has to be a *whole* frame, not a bare newline.
_PROBE_TVERSION_FRAME = bytes.fromhex("13000000640100001000000600395032303030")
assert 0x0D not in _PROBE_TVERSION_FRAME and 0x0A not in _PROBE_TVERSION_FRAME


def _probe_port(port: str) -> str | None:
    """Classifies a candidate CDC-ACM port as "console" or "net" without
    risking corruption of either role.

    Step 1 sends a complete Tversion frame and checks for an Rversion-shaped
    reply. This has to be a *whole*, self-contained frame rather than a bare
    probe byte: link_usb_cdc's plain length-prefixed framing (drivers/
    usb_cdc.c) has no SLIP-style resync-on-garbage mechanism, so a partial
    or unrelated write landing on the net port desyncs its frame parser
    until the next USB bus reset -- discovered the hard way testing this
    very script, where an earlier version's bare `\\r\\n` probe left the
    net port permanently wedged. A complete frame either gets consumed and
    answered (net) or sits there as harmless line noise (console, and this
    exact frame is known not to contain \\r or \\n, so it can't accidentally
    submit a garbled command either).

    Step 2 (only reached if step 1 got no 9P-shaped reply) sends a bare
    newline and checks for a shell prompt -- safe here specifically because
    step 1 already ruled out this port being the net port.

    Returns None if the port couldn't be classified (or couldn't even be
    opened -- in use, permissions, ...)."""
    try:
        with serial.Serial(port, 115200, timeout=1) as s:
            s.dtr = True
            time.sleep(0.3)
            s.reset_input_buffer()

            s.write(_PROBE_TVERSION_FRAME)
            s.flush()
            time.sleep(0.6)
            data = s.read(s.in_waiting or 1)
            if len(data) >= 5 and data[0] == 0x13 and data[4] == 0x65:  # Rversion
                return "net"

            s.reset_input_buffer()
            s.write(b"\r\n")
            s.flush()
            time.sleep(0.5)
            data = s.read(s.in_waiting or 1)
            if b"lsh>" in data or b"\x1b[?25l" in data:
                return "console"
            return None
    except (OSError, serial.SerialException):
        return None


def discover_ports(console: str | None = None, net: str | None = None,
                    uart: str | None = None) -> Rp2350Ports | None:
    """Auto-detects the console (ACM0) and net (ACM1, link_usb_cdc) ports by
    behavioral probing, plus an optional UART/CP2102 dongle port (needed
    only for the p9share test). Any of the three can be pinned explicitly
    -- e.g. if more than one RP2350-shaped device is attached, or probing
    guesses wrong -- to skip auto-detection for that one port.

    Returns None (meaning "no RP2350 found, skip hardware tests") rather
    than raising, so callers can treat "no hardware attached" as a normal,
    expected outcome instead of an error."""
    acm_candidates, uart_candidates = _candidate_ports()

    if not console or not net:
        for p in acm_candidates:
            if p in (console, net):
                continue
            role = _probe_port(p)
            if role == "console" and not console:
                console = p
            elif role == "net" and not net:
                net = p

    if not console or not net:
        return None

    if uart is None and uart_candidates:
        uart = uart_candidates[0]

    return Rp2350Ports(console=console, net=net, uart=uart)
