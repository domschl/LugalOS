#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""Load a RAM image onto an ESP32-P4-NANO and watch its console.

E1, plan/phase27_esp32p4_bringup.md. Self-contained: the shebang runs it under
`uv`, which fetches pyserial into an isolated environment, so there is nothing
to install and nothing to add to the system Python.

    tools/p4run.py --listen                 just watch the console
    tools/p4run.py --probe                  send a string, check it echoes
    tools/p4run.py IMAGE                    load IMAGE into RAM and run it
    tools/p4run.py IMAGE --listen-secs 20   ... and watch for 20s
    tools/p4run.py --reset-test             try to reset the board from software

**Nothing here writes flash.** `esptool load-ram` delivers the image over the
download protocol into L2MEM and jumps to it; a reset restores whatever is in
flash. That is deliberate for the whole of E1 -- see the header comment in
tools/minimal_esp32p4.c.

## Ports

Two USB sockets, and they are not interchangeable:

  * **the CH343P bridge** -- a real UART on the P4's UART0 (GPIO37/38, the
    ROM's own pins). Linux: /dev/ttyUSB*. macOS: /dev/cu.usbserial-*.
    **Everything should go through this one.**
  * **the native USB-Serial-JTAG** -- Linux: /dev/ttyACM*. macOS:
    /dev/cu.usbmodem*. Resetting the chip through it *may* leave UART0
    emitting bytes that decode at no baud rate we could find; the evidence is
    mixed and unresolved (see E1 in the plan). Detected only so this script
    can warn when it is the only port present.

## --reset-test, and why it is the first thing to run on a new host

The board has the standard auto-reset circuit: U6 (EMH4T2R) wires the
bridge's RTS to ESP_EN and DTR to GPIO35. If the host's USB-serial driver
carries modem-control lines, esptool can reset the board into download mode
with no buttons, and `tests/hw/` can eventually run unattended.

On macOS with the built-in CH34x driver it does not work -- four polarity and
timing combinations produced no reset, 2026-09-05. Linux's ch341 driver is
expected to do better. `--reset-test` answers that in about ten seconds by
pulsing the lines and watching for the ROM's banner.
"""

import argparse
import glob
import os
import subprocess
import sys
import threading
import time

import serial

CONSOLE_GLOBS = ["/dev/ttyUSB*", "/dev/cu.usbserial-*"]
JTAG_GLOBS = ["/dev/ttyACM*", "/dev/cu.usbmodem*"]
BAUD = 115200


def find_port(globs):
    for g in globs:
        hits = sorted(glob.glob(g))
        if hits:
            return hits[0]
    return None


def console_port(explicit):
    if explicit:
        return explicit
    env = os.environ.get("LUGALOS_P4_PORT")
    if env:
        return env
    p = find_port(CONSOLE_GLOBS)
    if p:
        return p
    j = find_port(JTAG_GLOBS)
    if j:
        sys.exit(
            "Only the native USB-Serial-JTAG port was found (%s).\n"
            "That is the wrong socket: driving the board through it may "
            "corrupt UART0 (E1, unresolved).\nPlug in the CH343P cable, or "
            "pass --port to override deliberately." % j
        )
    sys.exit("No serial port found. Looked for: %s" % ", ".join(CONSOLE_GLOBS))


def open_console(port):
    """Open without asserting DTR/RTS.

    Both lines reach the board through U6 -- RTS to ESP_EN, DTR to GPIO35 --
    so a library that helpfully asserts them on open would hold the chip in
    reset, or strap it into download mode, for as long as we are watching."""
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, BAUD, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False
    return s


class Watcher:
    """Collects console output on a thread while something else happens."""

    def __init__(self, port):
        self.s = open_console(port)
        self.buf = bytearray()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.is_set():
            self.buf.extend(self.s.read(4096))

    def __enter__(self):
        self.s.reset_input_buffer()
        self._t.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._t.join(timeout=2)
        self.s.close()

    def text(self):
        return bytes(self.buf).decode("utf-8", "replace")


def image_for(path):
    """Accept an .elf and produce the .img beside it; pass an .img through.

    This exists because of a mistake worth not repeating. On 2026-09-05 the
    loading was done by ad-hoc scripts that took an *image* path, while only
    the build script regenerated images -- so after every source edit the
    board was silently re-loaded with a stale binary from half an hour
    earlier. The symptoms (a program that echoed but never printed, ignoring
    changes that should have made it print) were misdiagnosed twice, once as
    a hung drain loop and once as a corrupted UART, and both wrong diagnoses
    were written down as findings before the timestamps were checked.

    Taking the ELF and regenerating unconditionally removes the whole class:
    there is no longer a stale artifact to load."""
    path = os.path.abspath(path)
    if not path.endswith(".elf"):
        return path
    img = path[:-4] + ".img"
    r = esptool("elf2image", "-o", img, path)
    if r.returncode != 0:
        sys.exit("elf2image failed:\n" + (r.stderr or r.stdout)[-500:])
    print("image: %s (regenerated from %s)" % (os.path.basename(img), os.path.basename(path)))
    return img


def esptool(*args, timeout=180):
    return subprocess.run(
        ["uv", "tool", "run", "--from", "esptool", "esptool", "--chip", "esp32p4", *args],
        capture_output=True, text=True, timeout=timeout,
    )


def load(port, img, reset):
    """Get the chip into download mode and deliver the image.

    `reset` picks how download mode is entered: "auto" lets esptool drive the
    reset lines, "none" assumes the board is already there (the BOOT+RESET
    buttons). "auto" is tried first and falls through to polling, so that a
    host whose driver *does* carry the modem lines needs no buttons and one
    whose driver does not still works."""
    before = "default-reset" if reset == "auto" else "no-reset"
    deadline = time.time() + (10 if reset == "auto" else 180)
    told = False
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        r = esptool("--port", port, "--before", before, "--after", "no-reset",
                    "--connect-attempts", "1", "--no-stub", "load-ram", img)
        if r.returncode == 0:
            print("loaded (attempt %d, --before %s)" % (attempt, before))
            return True
        if reset == "auto" and time.time() >= deadline:
            print("automatic reset did not get us into download mode; "
                  "falling back to the buttons.")
            before, reset = "no-reset", "none"
            deadline = time.time() + 180
        if not told and before == "no-reset":
            print(">>> Put the board in download mode: hold BOOT, tap RESET, "
                  "release BOOT.\n>>> Polling for up to 3 minutes...")
            told = True
        time.sleep(0.7)
    print("FAILED to load. Last esptool error:\n" + (r.stderr or r.stdout)[-400:])
    return False


def cmd_reset_test(port):
    """Can this host reset the board without the buttons?"""
    seqs = [
        ("classic (RTS then DTR)", [(False, True, 0.15), (True, False, 0.10), (False, False, 0.05)]),
        ("reset only (RTS pulse)", [(False, True, 0.15), (False, False, 0.05)]),
        ("inverted (DTR then RTS)", [(True, False, 0.15), (False, True, 0.10), (False, False, 0.05)]),
        ("both asserted first", [(True, True, 0.15), (False, True, 0.10), (False, False, 0.05)]),
    ]
    any_hit = False
    for name, seq in seqs:
        s = open_console(port)
        for dtr, rts, dwell in seq:
            s.dtr, s.rts = dtr, rts
            time.sleep(dwell)
        s.reset_input_buffer()
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < 1.5:
            buf.extend(s.read(4096))
        s.close()
        txt = bytes(buf).decode("utf-8", "replace")
        hit = "ESP-ROM" in txt
        any_hit |= hit
        boot = [l for l in txt.splitlines() if l.startswith("rst:")]
        print("%-26s %5d bytes  ROM banner: %-5s %s"
              % (name, len(buf), hit, boot[0] if boot else ""))
    print()
    if any_hit:
        print("This host CAN reset the board from software. Use --reset auto, "
              "and note it in plan/phase27_esp32p4_bringup.md -- it is what "
              "E8's unattended hardware suite needs.")
    else:
        print("No sequence reset the board: this host's USB-serial driver is "
              "not carrying the modem-control lines. The BOOT+RESET buttons "
              "are the mechanism here (as on macOS, 2026-09-05).")
    return 0 if any_hit else 1


def cmd_probe(port):
    """Send a string and check it comes back with this tree's echo signature."""
    s = open_console(port)
    s.reset_input_buffer()
    time.sleep(0.2)
    s.write(b"PROBE123\r")
    s.flush()
    time.sleep(1.0)
    got = s.read(4096)
    s.close()
    print("sent 9 bytes: %r" % b"PROBE123\r")
    print("got %d bytes: %r" % (len(got), got))
    # minimal_esp32p4.c turns a received '\r' into '\n' then '\r', so a correct
    # echo is TEN bytes, not nine. A plain wire loopback returns nine.
    if got == b"PROBE123\n\r":
        print("VERDICT: our program is running (10 bytes, '\\n' before '\\r' -- "
              "its own echo semantics, which a loopback cannot produce).")
        return 0
    if b"PROBE123" in got:
        print("VERDICT: something echoes, but not with our signature. "
              "A wire loopback, or different firmware.")
        return 1
    print("VERDICT: silent. Board may be running the factory app, wedged, or "
          "in download mode.")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?",
                    help="an .elf (the image is regenerated from it) or an .img")
    ap.add_argument("--port", help="console port (default: autodetect)")
    ap.add_argument("--reset", choices=["auto", "none"], default="auto")
    ap.add_argument("--listen-secs", type=float, default=8.0)
    ap.add_argument("--listen", action="store_true", help="watch the console and exit")
    ap.add_argument("--probe", action="store_true", help="echo test")
    ap.add_argument("--reset-test", action="store_true", help="can we reset from software?")
    a = ap.parse_args()

    port = console_port(a.port)
    print("console: %s @ %d" % (port, BAUD))

    if a.reset_test:
        return cmd_reset_test(port)
    if a.probe:
        return cmd_probe(port)
    if a.listen or not a.image:
        with Watcher(port) as w:
            time.sleep(a.listen_secs)
        print("=== %d bytes in %.0fs ===" % (len(w.buf), a.listen_secs))
        print(w.text())
        return 0

    if not load(port, image_for(a.image), a.reset):
        return 1
    # Listen only after the loader has released the port. The program's banner
    # is emitted into that gap and is normally lost, which is why
    # minimal_esp32p4.c re-announces itself rather than speaking once.
    with Watcher(port) as w:
        time.sleep(a.listen_secs)
    print("=== %d bytes in %.0fs, nothing sent ===" % (len(w.buf), a.listen_secs))
    print(w.text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
