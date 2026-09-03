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
import os
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


def ports_by_usb_descriptor() -> Rp2350Ports | None:
    """Identify the two CDC interfaces from the USB descriptor rather than by
    asking them anything.

    Behavioural probing is the better answer when it works, because it proves
    the port does what its name claims. It stops working exactly when it is
    most needed: an appliance persona whose application owns the console
    answers nothing, and a console whose 128-byte pushback ring has filled
    (which the probe itself contributes to -- see _probe_port()) answers
    nothing even after Ctrl-C. In both cases every tool built on discovery
    concluded there was no board attached, and flashing needed the port named
    by hand.

    The kernel already knows. udev's /dev/serial/by-id/ links carry the USB
    interface number, and this firmware's descriptor is fixed:
    interface 0 is the console, interface 2 is the 9P/net link
    (drivers/usb_cdc.c's dual-ACM layout). That is a property of the image, not
    of whether the board feels like replying.

    Linux only -- macOS has no by-id tree -- and None when the links are
    absent, so callers keep whatever fallback they had."""
    console = net = None
    for link in sorted(glob.glob("/dev/serial/by-id/*LugalOS*")):
        target = os.path.realpath(link)
        if link.endswith("-if00"):
            console = target
        elif link.endswith("-if02"):
            net = target
    if not console:
        return None
    return Rp2350Ports(console=console, net=net, uart=None)


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

    Step 3 (only if step 2 was answered by silence) sends Ctrl-C and retries,
    because an appliance persona's application owns the console and a healthy
    board is then indistinguishable from an absent one.

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

            # Step 3: silence is not the same as absence. On an appliance
            # persona the shell sits behind an application that owns the
            # console for as long as it runs -- the clock is one -- so a
            # perfectly healthy board answers a newline with nothing at all,
            # and every tool built on this function concluded there was no
            # board attached. That is what made flash.py unusable on the clock
            # persona without a manual BOOTSEL press.
            #
            # Ctrl-C is what such an application watches for
            # ([[standardized_interrupt_polling]]). It is safe to send here
            # precisely because step 1 already ruled out this being the net
            # port, and because console_pump() latches an interrupt from a
            # non-consuming peek since 2026-08-24 -- before that the probe
            # frame above could fill the 128-byte pushback ring and make
            # Ctrl-C impossible to latch, which is the bug this comment's
            # step 1 describes.
            #
            # The application releases the console silently, so the shell says
            # nothing until it is given something to echo: the newline after
            # the Ctrl-C is what produces the prompt, and dropping it makes
            # this look like it failed (found on hardware 2026-09-02).
            s.write(b"\x03")
            s.flush()
            time.sleep(0.8)
            s.reset_input_buffer()
            s.write(b"\r\n")
            s.flush()
            time.sleep(0.6)
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
        # Behavioural probing found nothing, which does not mean nothing is
        # there: a board whose console is owned by an application, or whose
        # pushback ring has filled, answers no probe at all. Ask the USB
        # descriptor, which does not depend on the board feeling like
        # replying. See ports_by_usb_descriptor().
        by_id = ports_by_usb_descriptor()
        if by_id:
            console = console or by_id.console
            net = net or by_id.net

    if not console or not net:
        return None

    if uart is None and uart_candidates:
        uart = uart_candidates[0]

    return Rp2350Ports(console=console, net=net, uart=uart)


_config_cache: "dict[str, int] | None" = None


def board_config(console_port: str) -> "dict[str, int]":
    """The ENABLE_* flags the attached board was actually built with, read
    from /proc/config.

    A board persona is not a smaller version of another one -- it is a
    different set of drivers. rp2350-clock builds with SPISD, ST7735, TM1638
    and CHESS off, because on the Pico-Clock-Green baseboard GP10-13 are the
    LED matrix's shift registers rather than an SD bus. A test for a driver
    task that was never compiled has nothing to talk to, and reporting that
    as a failure is not a result: it says "this is broken" about a board that
    is behaving exactly as built.

    That mattered in practice. This suite reported 18/24 on the clock persona
    with failure text reading "board firmware predates the `st7735stats`
    command -- reflash it with the current build/rp2350/lugalos.uf2", which is
    indistinguishable from a real staleness bug and cost a full build-flash-
    measure cycle against the previous commit to rule out (2026-09-03).

    The board has known this all along -- /proc/config reports every flag,
    which is what phase 7's K3 built it for. Only the asking was missing.

    Cached: the flags cannot change without a reflash, and every gated test
    would otherwise open the console again to ask the same question.
    """
    global _config_cache
    if _config_cache is not None:
        return _config_cache

    cfg: "dict[str, int]" = {}
    try:
        with serial.Serial(console_port, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            # The clock persona's application owns the terminal until Ctrl-C
            # hands it back, so ask for it before asking anything else.
            ser.write(b"\x03")
            ser.flush()
            time.sleep(0.8)
            drain(ser, quiet=0.5, deadline=3.0)
            ser.reset_input_buffer()
            ser.write(b"cat /proc/config\n")
            ser.flush()
            out = drain(ser, quiet=1.0, deadline=10.0).decode("utf-8", "replace")
        for key, val in re.findall(r"^(ENABLE_[A-Z0-9_]+)=(\d+)", out, re.M):
            cfg[key] = int(val)
    except Exception:
        # An unreadable /proc/config must not silently disable every gated
        # test -- an empty dict means feature_enabled() answers True and the
        # tests run as they always did, failing honestly if the feature is
        # genuinely absent.
        return {}

    _config_cache = cfg
    return cfg


def feature_enabled(console_port: str, flag: str) -> bool:
    """Whether `flag` (e.g. "ENABLE_SPISD") is on for the attached board.

    Unknown flags read as True on purpose: a board too old to report one, or
    a name that has since been renamed, should run the test and let it fail
    on its own terms rather than skip on a technicality. Silently skipping is
    the one outcome worse than a confusing failure.
    """
    cfg = board_config(console_port)
    return cfg.get(flag, 1) != 0


def local_build_id(build_dir: "Path | None" = None) -> str | None:
    """The build id the local tree would produce. Returns None if no build
    exists.

    Exists because "the board is running older firmware" previously had no
    direct answer: it was only detectable by noticing that some feature under
    test was missing, which costs a whole flash-and-measure cycle to work out.

    `build_dir` should be the directory the image being flashed came from.
    Without it this falls back to build/rp2350, which is only right when that
    is the preset in play -- and there are five RP2350 presets now
    (rp2350, -wifi, -gateway, -clock, -chess). Flashing build/rp2350-wifi's
    UF2 while this read build/rp2350's stale header reported a build-id
    mismatch on a board that had just been flashed correctly, which is worse
    than not checking: it accuses the thing that worked. Seen 2026-09-01.
    """
    candidates = []
    if build_dir is not None:
        candidates.append(Path(build_dir) / "lugalos_build_id.h")
    candidates.append(REPO_ROOT / "build" / "rp2350" / "lugalos_build_id.h")
    for candidate in candidates:
        if candidate.exists():
            m = re.search(r'#define\s+LUGALOS_BUILD_ID\s+"([^"]+)"', candidate.read_text())
            if m:
                return m.group(1)
    return None
