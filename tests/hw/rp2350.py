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
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import serial

# host/p9lib is the promoted, product-grade home of the 9P client this file
# used to duplicate a copy of (tests/p9lib.py, now retired) -- both this file
# and tests/runner.py import it from there, so protocol bugs only ever need
# fixing in one place.
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
import p9lib  # noqa: E402
from p9lib import SerialSocketAdapter, warm_up_9p  # noqa: E402,F401


def drain(ser: serial.Serial, quiet: float = 0.4, deadline: float = 3.0,
          kick_after: float | None = None) -> bytes:
    """Reads until `quiet` seconds pass with nothing new arriving, or
    `deadline` total seconds elapse. For human-readable console output
    (shell banners, help text, ...) only -- 9P frames go through
    SerialSocketAdapter + p9lib instead, which know their own exact framing
    and don't need to guess "is more coming?" from silence.

    `kick_after`, if set, is a real firmware quirk workaround, not a made-up
    knob: the RP2350's USB peripheral is Full-Speed-only silicon, and when
    it sits behind a High-Speed hub (as it does on at least one dev
    machine), a bulk IN transfer the device has armed after the console has
    been otherwise quiet for a while (in the worst observed case, after
    priostress's multi-second uninterrupted CPU spin) can sit host-side
    unpolled seemingly indefinitely -- waiting alone, even up to 40+
    seconds, was observed to never recover it. Sending *anything* new on
    the OUT direction reliably does, in every trial tried, within a few
    hundred ms -- empirically consistent with the host driver only
    re-examining a device's other endpoints in response to fresh I/O on it,
    not on its own schedule. So instead of gambling on an ever-larger
    passive deadline, if `kick_after` seconds pass with nothing new, send
    one bare newline to nudge the host and keep waiting -- fast on the
    common path, and self-healing on the slow one instead of just slower."""
    out = b""
    end = time.time() + deadline
    last = time.time()
    last_kick = time.time()
    while time.time() < end:
        ser.timeout = 0.1
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            last = time.time()
            last_kick = time.time()  # the kick's own echo counts; don't immediately re-kick
        elif kick_after is not None and time.time() - last_kick > kick_after:
            # Re-kicks every kick_after seconds rather than just once: a
            # single kick has resolved every case seen so far, but there is
            # no proof it always will, and `quiet` (above kick_after) still
            # bounds how many of these actually happen before giving up.
            ser.write(b"\n")
            ser.flush()
            last_kick = time.time()
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
    answered (net) or sits there as line noise (console, and this exact frame
    is known not to contain \\r or \\n, so it can't accidentally submit a
    garbled command either).

    "Harmless line noise" undersold it once: the frame is harmless as *data*
    but it occupies kernel/console.c's 128-byte pushback ring until something
    reads it, and on an appliance persona nothing does -- the clock owns the
    console for as long as it runs. Six probes' worth filled that ring, and a
    full ring used to mean Ctrl-C could never be latched again, i.e. the
    appliance became impossible to exit (found on hardware 2026-08-24; fixed
    in console_pump(), which now latches an interrupt from a non-consuming
    peek rather than from inside its drain loop). Still worth knowing that
    this probe leaves bytes behind on whichever port turns out to be the
    console.

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


def local_build_id() -> str | None:
    """The build id the local tree would produce, read from whichever build
    directory cmake last generated it into. Returns None if no build exists.

    Exists because "the board is running older firmware" previously had no
    direct answer: it was only detectable by noticing that some feature under
    test was missing, which costs a whole flash-and-measure cycle to work out.
    """
    for candidate in (REPO_ROOT / "build" / "rp2350" / "lugalos_build_id.h",):
        if candidate.exists():
            m = re.search(r'#define\s+LUGALOS_BUILD_ID\s+"([^"]+)"', candidate.read_text())
            if m:
                return m.group(1)
    return None
