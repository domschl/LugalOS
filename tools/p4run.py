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

## Ports, and the two jobs they do

This script needs two things from the wiring, and they are not always the
same cable:

  * a **console**: full-duplex bytes to and from the P4's UART0 (GPIO37/38,
    the ROM's own pins). Loading needs both directions -- esptool talks.
  * a **reset line**: DTR/RTS reaching the board's U6 (EMH4T2R), which wires
    RTS to ESP_EN and DTR to GPIO35. This is what makes the BOOT and RESET
    buttons unnecessary.

Ports cannot be told apart by name. On Linux the CH343P bridge enumerates
through `cdc_acm` as /dev/ttyACM*, and so does the P4's own native
USB-Serial-JTAG -- the very port that must be avoided. Detection is therefore
by USB VID:PID, and the roles can be split with --port / --reset-port or the
LUGALOS_P4_PORT / LUGALOS_P4_RESET_PORT environment variables.

On the development board as wired 2026-09-05 they *are* split, and the split
is not a preference:

  * CH343P (1a86:55d3), /dev/ttyACM0 -- its RTS resets the board and its RX
    carries UART0 output, but the host-to-board TX path is dead. esptool's
    own words: "Download mode successfully detected, but getting no sync
    reply: The serial TX path seems to be down."
  * CP2102 (10c4:ea60), /dev/ttyUSB0 -- an external bridge wired to UART0.
    Full duplex, syncs at 921600, no modem lines to the board at all.

So: reset over the CH343P, talk over the CP2102. A stock board with a working
CH343P uses that one port for both, which is what the defaults do.

## --reset-test, and why it is the first thing to run on a new host

If the host's driver carries the modem-control lines, esptool can reset the
board into download mode with no buttons, and `tests/hw/` can eventually run
unattended. `--reset-test` answers that in about ten seconds by driving the
lines and watching for the ROM's banner -- both a plain reset (into whatever
is in flash) and a strapped reset (into download mode), because E8 needs the
second one and only the second one is hard.

macOS, built-in CH34x driver: no sequence produced a reset, 2026-09-05.
Linux, cdc_acm: both work. See plan/phase27_esp32p4_bringup.md.
"""

import argparse
import glob
import os
import subprocess
import sys
import threading
import time

import serial

BAUD = 115200

# USB identities rather than device-name globs. On Linux the CH343P and the
# P4's native USB-Serial-JTAG both arrive as /dev/ttyACM*, so a glob cannot
# distinguish the port we want from the one we must not touch.
CH34X = {(0x1A86, 0x55D3), (0x1A86, 0x55D4), (0x1A86, 0x7523)}   # CH343P/CH9102/CH340
CP210X = {(0x10C4, 0xEA60)}                                       # CP2102
FTDI = {(0x0403, 0x6001), (0x0403, 0x6015)}
JTAG = {(0x303A, 0x1001), (0x303A, 0x1002)}                       # the socket to avoid

# Console candidates, best first. The CP210x/FTDI adapters come first because
# a host that has one has had it wired to UART0 deliberately; the CH343P is
# the stock on-board bridge and the fallback.
CONSOLE_PREF = [CP210X, FTDI, CH34X]


def usb_ports():
    """Every serial port with a USB identity, as (device, vid, pid, desc)."""
    from serial.tools import list_ports
    out = []
    for p in sorted(list_ports.comports(), key=lambda x: x.device):
        if p.vid is None:
            continue
        out.append((p.device, p.vid, p.pid, p.description or ""))
    return out


def _pick(ports, idsets):
    for ids in idsets:
        for dev, vid, pid, _ in ports:
            if (vid, pid) in ids:
                return dev
    return None


def _legacy_glob():
    for g in ("/dev/cu.usbserial-*", "/dev/ttyUSB*"):
        hits = sorted(glob.glob(g))
        if hits:
            return hits[0]
    return None


def console_port(explicit):
    """The port that carries bytes both ways."""
    if explicit:
        return explicit
    env = os.environ.get("LUGALOS_P4_PORT")
    if env:
        return env
    ports = usb_ports()
    p = _pick(ports, CONSOLE_PREF)
    if p:
        return p
    p = _legacy_glob()
    if p:
        return p
    j = _pick(ports, [JTAG])
    if j:
        sys.exit(
            "Only the native USB-Serial-JTAG port was found (%s).\n"
            "That is the wrong socket: driving the board through it may "
            "corrupt UART0 (E1, unresolved).\nPlug in the CH343P or a UART "
            "bridge, or pass --port to override deliberately." % j
        )
    sys.exit("No USB serial port found. Seen: %s" % (
        ", ".join("%s %04x:%04x" % (d, v, i) for d, v, i, _ in ports) or "none"))


def reset_port(explicit, console):
    """The port whose DTR/RTS reach ESP_EN and GPIO35.

    Defaults to the CH343P when one is present -- on this board it is the only
    bridge wired to U6 -- and otherwise to the console port, which is the
    stock single-cable arrangement."""
    if explicit:
        return explicit
    env = os.environ.get("LUGALOS_P4_RESET_PORT")
    if env:
        return env
    p = _pick(usb_ports(), [CH34X])
    return p or console


def _drive(s, seq):
    for dtr, rts, dwell in seq:
        s.dtr, s.rts = dtr, rts
        time.sleep(dwell)


# RTS reaches ESP_EN and DTR reaches GPIO35, both through U6's transistor
# pair, so asserting a line pulls the board's pin low.
#
#   RUN      -- hold EN low, release it, leave GPIO35 alone: boots flash.
#   DOWNLOAD -- hold EN low, then release EN while GPIO35 is held low: the
#               ROM samples the strap and waits for a download instead.
SEQ_RUN = [(False, True, 0.15), (False, False, 0.05)]
SEQ_DOWNLOAD = [(False, True, 0.15), (True, False, 0.15), (False, False, 0.05)]


def pulse(port, seq, listen=1.5, watch=None):
    """Drive a reset sequence and return whatever the board says afterwards.

    `watch` is the port to read from, which is not always the port being
    driven: on a board whose reset lines and console are different cables,
    the ROM banner comes back on the console."""
    s = open_console(port)
    same = watch is None or watch == port
    r = s if same else open_console(watch)
    try:
        # Flush BEFORE driving, never after. The ROM's banner is emitted
        # within a few tens of milliseconds of EN being released -- inside
        # the sequence's own final dwell -- so a flush placed after the drive
        # discards the one line that says whether the reset happened and
        # which mode it chose. That read as "this host cannot reset the
        # board" on a host that resets it perfectly.
        r.reset_input_buffer()
        _drive(s, seq)
        buf = bytearray()
        t0 = time.time()
        while time.time() - t0 < listen:
            buf.extend(r.read(4096))
    finally:
        if not same:
            r.close()
        s.close()
    return bytes(buf).decode("utf-8", "replace")


def enter_download(port, watch=None):
    """Reset the board into the ROM's download mode. True if it said so."""
    txt = pulse(port, SEQ_DOWNLOAD, listen=1.5, watch=watch)
    return "waiting for download" in txt or "DOWNLOAD" in txt


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

    def drive(self, seq):
        """Drive a reset sequence on the port this watcher already holds.

        Opening a second handle to drive the lines would work on Linux and
        is a race everywhere else; the watcher owns the port, so the reset
        goes through it."""
        _drive(self.s, seq)

    def wait_for(self, needle, secs):
        t0 = time.time()
        while time.time() - t0 < secs:
            if needle in self.text():
                return True
            time.sleep(0.05)
        return False

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


# How many times to deliver the image before giving up.
#
# esptool reporting success is not the same thing as the image running: with
# `--no-stub load-ram` on this board, roughly one delivery in three returns 0
# and then nothing ever executes -- the capture holds the ROM's own banner and
# not one byte more. That was a real cost during E7
# (plan/phase27_esp32p4_bringup.md), where a "the sensor was not detected at
# boot" result and "the board never booted" look identical from a grep.
#
# So the success condition is the program's own first words, not the loader's
# exit code, and a load that does not produce them is retried.
LOAD_TRIES = 3

# What this kernel prints before anything else can go wrong. Absent from a
# board that did not start; absent, too, from an image that is not this
# kernel -- which is why looks_dead() below has to be consulted as well,
# rather than treating a missing marker as a failed load.
RUNNING_MARKER = "LugalOS"

# And what it prints when it is ready to be typed at. Proof of life and
# readiness are eight seconds and a whole boot apart, and conflating them
# types the first command into a line editor that does not exist yet -- which
# comes back as a command that ran and printed nothing, the most misleading
# result this script can produce.
READY_MARKER = "lsh>"


def looks_dead(text):
    """True when the capture holds the boot ROM's output and nothing after it.

    The distinction that matters is between "the image ran and said something
    we do not recognise" (a bare test image: leave it alone) and "the image
    never ran" (retry). The ROM always announces itself and then reports
    waiting for the download; anything printed *after* that came from the
    program, whatever it is."""
    tail = text.rsplit("waiting for download", 1)[-1]
    # The download itself echoes binary noise onto this receive-only port, so
    # count only what could plausibly be a program talking.
    return sum(c.isalpha() for c in tail) < 16


def load(port, img, reset, rport=None, driver=None, baud=None):
    """Get the chip into download mode and deliver the image.

    Download mode is entered by driving the reset lines here rather than by
    letting esptool do it with --before default-reset. That is not
    duplication: esptool drives the lines on the same port it then talks on,
    and on this board those are two different cables. Doing it ourselves is
    also what makes the reset port configurable at all.

    `reset` picks the mechanism: "auto" drives the lines, "none" assumes the
    board is already in download mode (the BOOT+RESET buttons). "auto" falls
    back to the buttons, so a host whose driver does not carry the modem
    lines still works."""
    rport = rport or port
    deadline = time.time() + 180
    told = False
    attempt = 0
    r = None
    while time.time() < deadline:
        attempt += 1
        if reset == "auto":
            if driver is not None:
                driver.drive(SEQ_DOWNLOAD)
                ok = driver.wait_for("waiting for download", 2.0)
            else:
                ok = enter_download(rport, watch=port)
            if not ok:
                print("reset lines did not produce download mode on %s; "
                      "falling back to the buttons." % rport)
                reset = "none"
                continue
        elif not told:
            print(">>> Put the board in download mode: hold BOOT, tap RESET, "
                  "release BOOT.\n>>> Polling for up to 3 minutes...")
            told = True
        baud_args = ("--baud", str(baud)) if baud else ()
        r = esptool("--port", port, *baud_args,
                    "--before", "no-reset", "--after", "no-reset",
                    "--connect-attempts", "1", "--no-stub", "load-ram", img)
        if r.returncode == 0:
            print("loaded (attempt %d, reset %s via %s)" % (attempt, reset, rport))
            return True
        time.sleep(0.7)
    print("FAILED to load. Last esptool error:\n"
          + ((r.stderr or r.stdout)[-400:] if r else "(never got to download mode)"))
    return False


def cmd_reset_test(port, rport):
    """Can this host reset the board without the buttons?

    Two questions, and the second is the one that matters. Booting flash on
    demand is convenient; resetting *into download mode* on demand is what
    lets a test suite load a fresh image with nobody in the room, and it is
    the harder of the two because it needs DTR as well as RTS.

    The verdict is read from the ROM's own banner, which names the boot mode
    it chose -- `boot:0x307 (DOWNLOAD(USB/UART0/SPI))` against the ordinary
    flash boot. Guessing from silence was what made the first version of this
    test ambiguous."""
    results = []
    for name, seq in (("run (RTS pulse)", SEQ_RUN),
                      ("download (RTS + DTR strap)", SEQ_DOWNLOAD)):
        txt = pulse(rport, seq, listen=2.0, watch=port)
        rst = [l for l in txt.splitlines() if l.startswith("rst:")]
        booted = "ESP-ROM" in txt or bool(rst)
        dl = "waiting for download" in txt
        results.append((name, booted, dl))
        print("%-28s %5d bytes  reset: %-5s  %s"
              % (name, len(txt), booted, rst[0] if rst else
                 ("(no rst: line)" if not booted else "")))
    print()
    can_reset = any(b for _, b, _ in results)
    can_download = results[-1][2]
    if can_download:
        print("This host can reset the board into DOWNLOAD MODE from software.\n"
              "Loading needs no buttons (--reset auto is the default), and E8 "
              "can have an unattended hardware suite.")
        return 0
    if can_reset:
        print("This host can reset the board, but could not strap it into "
              "download mode.\nRESET works, BOOT does not: check that DTR "
              "reaches GPIO35 on %s." % rport)
        return 1
    print("No sequence reset the board: this host's USB-serial driver is not "
          "carrying the modem-control lines on %s.\nThe BOOT+RESET buttons are "
          "the mechanism here (as on macOS, 2026-09-05)." % rport)
    return 1


def cmd_ports():
    """What is plugged in, and which role each port was given."""
    ports = usb_ports()
    if not ports:
        print("no USB serial ports found")
        return 1
    known = {**{k: "CH34x bridge" for k in CH34X},
             **{k: "CP210x bridge" for k in CP210X},
             **{k: "FTDI bridge" for k in FTDI},
             **{k: "native USB-Serial-JTAG (avoid)" for k in JTAG}}
    for dev, vid, pid, desc in ports:
        print("  %-16s %04x:%04x  %-30s %s"
              % (dev, vid, pid, known.get((vid, pid), "unknown"), desc))
    return 0


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
    # minimal_esp32p4.c turns a received '\r' into '\n' then '\r', so its echo
    # is TEN bytes for nine, with the '\n' first. A plain wire loopback
    # returns the nine it was given and cannot invent the '\n'.
    #
    # Searched for, not compared against: the heartbeat is unconditional (for
    # good reason -- see minimal_esp32p4.c), so the echo almost always arrives
    # with a beat line wrapped around it. An equality test here reported
    # "not our signature" about a reply that was sitting in plain sight.
    if b"PROBE123\n\r" in got:
        print("VERDICT: our program is running ('\\n' before '\\r' -- its own "
              "echo semantics, which a loopback cannot produce).")
        return 0
    if b"PROBE123" in got:
        print("VERDICT: something echoes, but not with our signature. "
              "A wire loopback, or different firmware.")
        return 1
    print("VERDICT: silent. Board may be running the factory app, wedged, or "
          "in download mode.")
    return 1


def run_cmds(port, lines, wait):
    """Type lines at the board's console and print what comes back.

    The point is to make a shell testable without a human. tests/hw/ can call
    this; so can anyone checking a milestone's own done-condition, which for
    E2 is literally "a shell prompt, `ls /proc`, and `cat /proc/meminfo`".

    CR, not LF: kernel/line_editor.c takes carriage return as the end of a
    line, which is what a terminal sends when Return is pressed. Sending LF
    instead gets a prompt back and no command run, which looks exactly like a
    board that is ignoring you."""
    out = []
    with Watcher(port) as w:
        # A bare Return first: it costs one prompt and proves the console is
        # answering before any command's output has to be interpreted. A
        # missing prompt here and a wrong answer below are very different
        # problems, and telling them apart afterwards is much harder.
        w.s.write(b"\r")
        time.sleep(wait)
        for line in lines:
            mark = len(w.buf)
            w.s.write(line.encode() + b"\r")
            time.sleep(wait)
            out.append((line, bytes(w.buf[mark:]).decode("utf-8", "replace")))
    for line, text in out:
        print("--- $ %s" % line)
        print(text)
    return 0


def interactive(port):
    """Relay this terminal to the board until Ctrl-] .

    Deliberately not a dependency on picocom or screen: this script already
    owns the port-identification problem (see the module docstring), and
    telling someone to run picocom on a port whose name is not stable across
    hosts undoes exactly the thing --ports exists to fix."""
    import termios
    import tty
    import select as _select

    s = open_console(port)
    print("--- interactive on %s @ %d; Ctrl-] to exit ---" % (port, BAUD))
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        while True:
            r, _, _ = _select.select([fd, s.fileno()], [], [], 0.1)
            if fd in r:
                ch = os.read(fd, 1)
                if ch == b"\x1d":          # Ctrl-]
                    break
                s.write(ch)
            if s.fileno() in r:
                data = s.read(4096)
                if data:
                    os.write(1, data)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        s.close()
        print("\n--- closed ---")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", nargs="?",
                    help="an .elf (the image is regenerated from it) or an .img")
    ap.add_argument("--port", help="console port (default: autodetect)")
    ap.add_argument("--reset-port",
                    help="port whose DTR/RTS reach ESP_EN and GPIO35 "
                         "(default: the CH34x bridge, else the console port)")
    ap.add_argument("--reset", choices=["auto", "none"], default="auto")
    ap.add_argument("--baud", type=int,
                    help="baud rate for the load transfer only, e.g. 921600. "
                         "The console stays at %d, because the program being "
                         "loaded sets its own rate and the ROM's is only in "
                         "force until it does. Worth using from E2 onward: a "
                         "kernel image is ~227 KB against E1's 1.2 KB, which "
                         "is about 40 s at the default rate." % BAUD)
    ap.add_argument("--cmd", action="append", default=[], metavar="LINE",
                    help="after loading (or with --listen), send LINE to the "
                         "console and print what comes back. Repeatable, in "
                         "order. This is what lets a shell be exercised "
                         "without a human at the keyboard -- see --interactive "
                         "for the human case.")
    ap.add_argument("--cmd-wait", type=float, default=2.0,
                    help="seconds to collect output after each --cmd (default 2)")
    ap.add_argument("--interactive", action="store_true",
                    help="after loading, relay the terminal to the console "
                         "port until Ctrl-] is pressed")
    ap.add_argument("--listen-secs", type=float, default=8.0)
    ap.add_argument("--listen", action="store_true", help="watch the console and exit")
    ap.add_argument("--probe", action="store_true", help="echo test")
    ap.add_argument("--reset-test", action="store_true", help="can we reset from software?")
    ap.add_argument("--ports", action="store_true", help="list USB serial ports and exit")
    ap.add_argument("--run", action="store_true",
                    help="reset the board into whatever is in flash, and listen")
    a = ap.parse_args()

    if a.ports:
        return cmd_ports()

    port = console_port(a.port)
    rport = reset_port(a.reset_port, port)
    print("console: %s @ %d%s"
          % (port, BAUD, "" if rport == port else "   reset lines: %s" % rport))

    if a.reset_test:
        return cmd_reset_test(port, rport)
    if a.run:
        print(pulse(rport, SEQ_RUN, listen=a.listen_secs, watch=port))
        return 0
    if a.probe:
        return cmd_probe(port)
    if not a.image and a.interactive:
        return interactive(port)
    if not a.image and a.cmd:
        return run_cmds(port, a.cmd, a.cmd_wait)
    if a.listen or not a.image:
        with Watcher(port) as w:
            time.sleep(a.listen_secs)
        print("=== %d bytes in %.0fs ===" % (len(w.buf), a.listen_secs))
        print(w.text())
        return 0

    img = image_for(a.image)

    # When the reset lines and the console are different cables, the reset
    # port's RX is still wired to UART0 -- so it can be held open and read
    # *through* the load, and the program's first words survive.
    #
    # That matters more than it sounds. The banner and the CSR dump are
    # printed once, in the instant after the ROM jumps to us, while esptool
    # still owns the port it loaded over. On a single-cable host they are
    # simply lost, which is why minimal_esp32p4.c re-announces itself on a
    # heartbeat. Here they are not lost, and the CSR dump is the whole
    # observational point of E1.
    if rport != port:
        for tries_left in range(LOAD_TRIES - 1, -1, -1):
            with Watcher(rport) as w:
                if not load(port, img, a.reset, rport, driver=w, baud=a.baud):
                    return 1
                t0 = time.time()
                started = w.wait_for(RUNNING_MARKER, a.listen_secs)
                if started:
                    # Started is not ready: wait out the rest of the window
                    # for the prompt, and if it never comes, still give the
                    # board the time the caller asked for.
                    left = a.listen_secs - (time.time() - t0)
                    if left > 0 and not w.wait_for(READY_MARKER, left):
                        time.sleep(max(0.0, a.listen_secs - (time.time() - t0)))
                else:
                    # Give a program with no marker of ours (a bare test
                    # image) the full listen window it was going to get.
                    time.sleep(max(0.0, a.listen_secs - (time.time() - t0)))
            if started or not looks_dead(w.text()) or tries_left == 0:
                break
            print("loaded, but the image never started (%d bytes, ROM output "
                  "only) -- reloading, %d attempt(s) left."
                  % (len(w.buf), tries_left))
        print("=== %d bytes on %s, across the load ===" % (len(w.buf), rport))
        print(w.text())
        # The watcher above reads the reset port, which on this wiring is
        # receive-only. Anything that has to *type* has to do it on the
        # console port, and only after the watcher has let go of nothing --
        # the two are different cables, so there is no handover to get wrong.
        if a.cmd:
            run_cmds(port, a.cmd, a.cmd_wait)
        if a.interactive:
            return interactive(port)
        return 0

    if not load(port, img, a.reset, rport, baud=a.baud):
        return 1
    # Single cable: listen only after the loader has released the port. The
    # banner is emitted into that gap and is normally lost.
    with Watcher(port) as w:
        time.sleep(a.listen_secs)
    print("=== %d bytes in %.0fs, nothing sent ===" % (len(w.buf), a.listen_secs))
    print(w.text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
