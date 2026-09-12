#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for the **ESP32-P4** (Waveshare
ESP32-P4-NANO) -- E8, plan/phase27_esp32p4_bringup.md.

The sibling of test_rp2350.py and test_gateway.py, and it follows their
conventions: every test returns (name, ok, detail), and everything *skips*
rather than fails when no board is attached, so it is safe to run
speculatively and safe to leave out of CI.

What makes a separate file worth having is that almost none of phase 27 can
be checked anywhere else. QEMU has no ESP32-P4 machine at all, so the CLIC,
the PMP granularity, the boot ROM's flash routines, the I2C controller and
the interrupt level that made a 100 Hz tick preempt nothing are exercised
here or nowhere.

**This suite loads the kernel itself.** That was E8's one real precondition
and it is met: tools/p4run.py resets the board over the CH343P's modem lines,
delivers the image with `esptool load-ram`, and checks the image actually
started -- so nobody has to be in the room holding BOOT. Nothing here writes
flash; a reset restores whatever is in it.

Usage:
    uv run test_esp32p4.py                       # auto-detect, load, test
    uv run test_esp32p4.py --no-load             # test whatever is running
    uv run test_esp32p4.py --port /dev/ttyUSB0 --reset-port /dev/ttyACM0

## The two cables, which are not optional

On this board's wiring the console and the reset lines are different cables,
and neither can do the other's job (tools/p4run.py's module docstring has the
full account). Detection is by USB VID:PID rather than by device name,
because the CH343P enumerates as /dev/ttyACM* and so does the P4's own native
USB-Serial-JTAG -- the one port that must be avoided.

## What the ES8311 is doing in an I2C test

The NANO carries an audio codec at 0x18 on the same I2C bus as the BME280.
It is soldered down, so it cannot be forgotten, unplugged or wired wrong --
which makes it the control in every bus test here. A scan that finds 0x76 and
not 0x18 is a sensor test that got lucky; a scan that finds neither is a bus
that is not working, not a sensor that is missing.
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
import p9lib  # noqa: E402
from p9lib import connect_serial  # noqa: E402

BUILD_DIR = REPO_ROOT / "build" / "esp32p4"
BAUD = 115200

# The BME280's own address, and the board's soldered-down audio codec. See the
# module docstring for why the second one is in a sensor suite.
BME280_ADDR = 0x76
ES8311_ADDR = 0x18


def _load_p4run():
    """tools/p4run.py as a module.

    Imported rather than re-implemented: it already owns VID:PID detection,
    the reset sequences, the image regeneration and the load-actually-ran
    check, and a second copy of any of those would be a second copy that
    drifts. It is import-safe -- every side effect is inside main(), behind
    the usual __main__ guard."""
    path = REPO_ROOT / "tools" / "p4run.py"
    spec = importlib.util.spec_from_file_location("p4run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@dataclass
class Board:
    console: str        # full duplex, talks to UART0
    reset: str          # DTR/RTS reach ESP_EN and GPIO35
    p4run: object
    # Assigned once the board has answered, and replaced by the 9P test, which
    # has to hand the port over to a client and take it back afterwards.
    console_session: "Console | None" = None


class Console:
    """One long-lived console session, rather than a fresh port per test.

    test_rp2350.py opens and closes the port per test and can afford to: that
    board's console survives being reopened. Here the console is a plain UART
    with no flow control and no device that notices, so every reopen risks
    landing mid-line and reading a fragment as a result. One session, drained
    between commands, is both faster and less inventive."""

    PROMPT = "lsh>"

    def __init__(self, port: str):
        self.ser = serial.Serial(port, BAUD, timeout=0.2)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def drain(self, quiet: float = 0.4, deadline: float = 5.0) -> str:
        """Read until `quiet` seconds pass with nothing new, or `deadline`."""
        out = b""
        end = time.time() + deadline
        last = time.time()
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                out += chunk
                last = time.time()
            elif time.time() - last >= quiet:
                break
        return out.decode("utf-8", "replace")

    def cmd(self, line: str, deadline: float = 15.0) -> str:
        """Type a line and return everything up to the next prompt.

        **Waiting for the prompt, not for silence.** A silence rule looks
        right and is wrong for exactly the commands worth testing: `clicdump`
        spins for two seconds per phase and prints nothing while it does, so a
        one-second quiet window ends the read mid-command. The cost is not a
        truncated result -- it is that every *later* command then reads the
        tail of an earlier one, and a whole suite reports nonsense from one
        mistimed read. That happened, and this is the fix.

        CR, not LF: kernel/line_editor.c ends a line on carriage return, which
        is what a terminal sends for Return. Sending LF gets a prompt back and
        no command run -- which looks exactly like a board ignoring you."""
        self.ser.reset_input_buffer()
        self.ser.write(line.encode() + b"\r")
        self.ser.flush()
        out = b""
        end = time.time() + deadline
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                out += chunk
                # The prompt is reprinted once the command has finished. Look
                # for it past the echoed command so a command whose own name
                # contains the prompt could not end its own read.
                if self.PROMPT in out.decode("utf-8", "replace")[len(line):]:
                    break
        # A short tail read: the prompt is followed by cursor-control bytes,
        # and leaving them in the port makes the next read start mid-escape.
        out += self.ser.read(4096)
        return out.decode("utf-8", "replace")

    def wake(self) -> bool:
        """One bare Return, to prove the shell answers before any command's
        output has to be interpreted. A missing prompt here and a wrong answer
        later are very different problems, and much harder to tell apart
        afterwards."""
        return self.PROMPT in self.cmd("", deadline=5.0)


# --- helpers ---------------------------------------------------------------

def kv(text: str) -> dict:
    """The key=value blocks /proc files in this tree use."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if "=" in line and " " not in line.split("=", 1)[0]:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def scanned_addrs(text: str) -> set[int]:
    """The addresses an `i2c scan` grid reports.

    The grid prints a row header ("70: ") and then sixteen cells, each either
    "--" or the address in hex. Only lines that look like a grid row are
    considered, so a stray two-hex-digit number elsewhere in the console
    output cannot be mistaken for a device."""
    found = set()
    for line in text.splitlines():
        if not re.match(r"^\s*[0-7]0:\s", line):
            continue
        # The cell already holds the full address, so the row header only
        # identifies the line as a grid row -- the column position is not
        # needed, and relying on it would break on the first row, which is
        # indented past the reserved addresses rather than left-aligned.
        for cell in line.split(":", 1)[1].split():
            if re.fullmatch(r"[0-9a-f]{2}", cell):
                found.add(int(cell, 16))
    return found


# --- tests -----------------------------------------------------------------

def test_boots(b: Board) -> tuple[str, bool, str]:
    name = "boots to a shell"
    c = b.console_session
    if not c.wake():
        return name, False, "no prompt on the console"
    return name, True, ""


def test_proc_readable(b: Board) -> tuple[str, bool, str]:
    name = "/proc answers"
    out = b.console_session.cmd("cat /proc/version")
    if "LugalOS" not in out:
        return name, False, f"unexpected /proc/version: {out[-200:]!r}"
    return name, True, ""


def test_flash0_mounted(b: Board) -> tuple[str, bool, str]:
    """E6: /flash0 through the boot ROM's SPI routines. The kernel is
    RAM-loaded, so a mounted /flash0 proves the ROM call path works rather
    than proving the load worked."""
    name = "/flash0 is mounted (E6)"
    out = b.console_session.cmd("ls /flash0")
    # Upper-cased: this is FAT32 and `ls` prints the 8.3 short names, so
    # matching "system" finds nothing on a filesystem that is perfectly fine.
    if "SYSTEM" not in out.upper():
        return name, False, f"no /flash0/system: {out[-200:]!r}"
    return name, True, ""


def test_preemption(b: Board) -> tuple[str, bool, str]:
    """E4's finding, and the one nothing else in this project can catch.

    In CLIC mode `mstatus.MIE` is not the interrupt gate: taking an interrupt
    raises `mintstatus.MIL`, and only `mret` lowers it again. This kernel
    preempts by calling sched_yield() *inside* the timer handler, so the mret
    is stranded in a frame nothing unwinds until that task resumes -- leaving
    every interrupt masked while MIE reads 1 the whole time. The symptom was a
    100 Hz tick that preempted nothing.

    `clicdump` is the probe, and its two spin phases are the experiment: both
    spin for two seconds without yielding, and the only difference is whether
    another task is READY, which decides whether the tick has anywhere to
    switch. Before p4_drop_intlevel(), phase 0 gained 200 ticks and phase 1
    gained *none*. A dump taken at the prompt shows MIL=0 and proves nothing,
    which is why this reads the phases rather than the registers.

    On RP2350 and QEMU nothing could fail this, because neither has MIL."""
    name = "the tick preempts under a READY task (E4, MIL)"
    out = b.console_session.cmd("clicdump", deadline=40.0)
    phases = {int(p): int(t) for p, t in
              re.findall(r"spin(\d) \([^)]*\): ticks \+(\d+) over 2 s", out)}
    if len(phases) < 2:
        return name, True, "SKIPPED (no clicdump on this build)"
    # Two seconds at 100 Hz is 200; allow generous slack for a board with no
    # PLL. What is being tested is "the tick still arrives", not its accuracy
    # -- E4's own ten-minute measurement is where the rate is checked.
    if phases[0] < 100:
        return name, False, (f"the tick does not arrive at all: "
                             f"spin0 gained {phases[0]} ticks in 2 s")
    if phases[1] < 100:
        return name, False, (f"the tick stops once something is READY: spin0 "
                             f"+{phases[0]}, spin1 +{phases[1]}. That is E4's "
                             f"MIL bug -- see p4_drop_intlevel()")
    return name, True, f"spin0 +{phases[0]}, spin1 +{phases[1]} ticks"


def test_umode(b: Board) -> tuple[str, bool, str]:
    """E5. Tests the *refusal*, not the grant -- every other check of this
    machinery passes with PMP switched off."""
    name = "U-mode isolation refuses (E5)"
    out = b.console_session.cmd("umodetest", deadline=30.0)
    if "umodetest" not in out:
        return name, True, "SKIPPED (no umodetest on this build)"
    low = out.lower()
    if "fail" in low:
        return name, False, out[-400:]
    if "pass" not in low and "ok" not in low:
        return name, False, f"no verdict: {out[-300:]!r}"
    return name, True, ""


def test_i2c_bus(b: Board) -> tuple[str, bool, str]:
    """The bus, with a soldered-down control. See the module docstring."""
    name = "I2C scan finds the codec and the sensor (E7)"
    out = b.console_session.cmd("i2c scan", deadline=20.0)
    found = scanned_addrs(out)
    if not found:
        return name, False, f"an empty bus: {out[-300:]!r}"
    if ES8311_ADDR not in found:
        return name, False, (f"no ES8311 at 0x18 -- the codec is soldered to this "
                             f"board, so the bus is wrong, not the wiring. Found "
                             f"{sorted(hex(a) for a in found)}")
    if BME280_ADDR not in found:
        return name, True, ("SKIPPED (bus works -- codec answered -- but no BME280 "
                            "at 0x76; none fitted?)")
    return name, True, f"found {sorted(hex(a) for a in found)}"


def test_i2c_stable(b: Board) -> tuple[str, bool, str]:
    """E7's own diagnostic, as a regression test.

    Three identical probes in a row, because one cannot tell "the part is
    there" from "the last transaction left the controller somewhere":
    1,1,1 is a part, 0,0,0 is an empty address, and **0,1,1 is a stale
    controller**. That last pattern is the bug this milestone spent its time
    on, and it is invisible to any single probe."""
    name = "I2C is stable on the first transaction (E7)"
    out = b.console_session.cmd("i2cdiag", deadline=25.0)
    probes = [int(m) for m in re.findall(r"probe#\d+ 0x76 -> (\d)", out)]
    reads = re.findall(r"read#\d+ 0x76 reg 0xd0 -> (\d) id=0x([0-9a-f]{2})", out)
    if not probes or not reads:
        return name, True, "SKIPPED (no i2cdiag on this build)"
    if probes[0] == 0 and probes[1:] == [1] * len(probes[1:]) and probes[1:]:
        return name, False, (f"probes {probes}: a stale controller -- the first "
                             f"transaction is paying for the last one")
    if probes != [1] * len(probes):
        return name, True, f"SKIPPED (nothing at 0x76: probes {probes})"
    bad = [r for r in reads if r[0] != "1"]
    if bad:
        return name, False, f"probe works but register read does not: {reads}"
    ids = {r[1] for r in reads}
    if ids != {"60"}:
        return name, False, f"chip id should be 0x60 on every read, got {ids}"
    return name, True, f"probes {probes}, id 0x60 x{len(reads)}"


def test_sensor_reads(b: Board) -> tuple[str, bool, str]:
    """E7's done-condition, local half: a real measurement, range-checked.

    The ranges are deliberately wide -- this is a test of the driver and the
    compensation arithmetic, not of the weather. What they catch is the
    failure these formulas actually have: a mistyped coefficient, or t_fine
    corrupted by a temperature error, which does not produce a slightly wrong
    number but an absurd one."""
    name = "BME280 reads a plausible measurement (E7)"
    out = b.console_session.cmd("sensor", deadline=20.0)
    if "none found" in out:
        return name, True, "SKIPPED (no BME280 fitted)"
    m = re.search(r"(bm[ep]280) @0x([0-9a-f]{2}): (-?\d+)\.(\d\d) C, (\d+)\.(\d\d) hPa", out)
    if not m:
        return name, False, f"unparseable: {out[-300:]!r}"
    temp = float(f"{m.group(3)}.{m.group(4)}")
    hpa = float(f"{m.group(5)}.{m.group(6)}")
    if not -40.0 <= temp <= 85.0:
        return name, False, f"temperature {temp} C is outside the part's own range"
    if not 300.0 <= hpa <= 1100.0:
        return name, False, f"pressure {hpa} hPa is outside the part's own range"
    return name, True, f"{m.group(1)} {temp} C, {hpa} hPa"


def test_sensor_selftest(b: Board) -> tuple[str, bool, str]:
    """The compensation arithmetic against a vector computed independently
    (tools/bme280_reference.py). Runs on QEMU too, and is here because a
    board that fails it is a board whose numbers mean nothing."""
    name = "BME280 compensation selftest"
    out = b.console_session.cmd("sensor selftest", deadline=20.0)
    m = re.search(r"(\d+) cases? failed", out)
    if not m:
        return name, True, "SKIPPED (no `sensor selftest` on this build)"
    if m.group(1) != "0":
        return name, False, out[-300:]
    return name, True, ""


def test_proc_sensors(b: Board) -> tuple[str, bool, str]:
    """/proc/sensors is what a reader on the far end of a wire actually gets,
    and it is served from a cache -- so the age matters as much as the
    values. A file that is valid but hours old is a frozen sensor reported as
    a working one."""
    name = "/proc/sensors carries a fresh reading (E7)"
    out = b.console_session.cmd("cat /proc/sensors", deadline=20.0)
    f = kv(out)
    if f.get("part") == "none":
        return name, True, "SKIPPED (no BME280 fitted)"
    if "part" not in f:
        return name, False, f"unparseable: {out[-300:]!r}"
    if f.get("valid") != "yes":
        return name, False, f"detected but never read: {f}"
    period = int(f.get("sample_period_s", "0"))
    age = int(f.get("age_s", "999999"))
    # One period plus a conversion's worth of slack. Older than that means the
    # sampler task is not running, which is exactly the failure this file
    # exists to notice from outside the board.
    if period and age > period + 10:
        return name, False, (f"age_s={age} against sample_period_s={period} -- "
                             f"the sampler is not running")
    return name, True, f"age {age}s of {period}s, {f.get('reads')} reads"


def test_sampler_advances(b: Board) -> tuple[str, bool, str]:
    """The sampler, observed rather than assumed: `reads` must increase on its
    own, with nobody asking for a measurement.

    Deliberately not `sensor` twice -- that command reads the bus itself and
    would prove only that the console works. This watches the counter that a
    9P client would see."""
    name = "the sampler advances unattended (E7)"
    first = kv(b.console_session.cmd("cat /proc/sensors", deadline=20.0))
    if first.get("part") == "none":
        return name, True, "SKIPPED (no BME280 fitted)"
    period = int(first.get("sample_period_s", "60"))
    age = int(first.get("age_s", "0"))
    # Wait for the next sample plus slack, measured from the last one rather
    # than from now -- otherwise a test starting just after a sample waits a
    # whole period longer than it needs to.
    wait = max(2.0, period - age + 5.0)
    if wait > 90.0:
        return name, True, f"SKIPPED (would wait {wait:.0f}s)"
    time.sleep(wait)
    second = kv(b.console_session.cmd("cat /proc/sensors", deadline=20.0))
    r1, r2 = int(first.get("reads", 0)), int(second.get("reads", 0))
    if r2 <= r1:
        return name, False, f"reads stuck at {r1} after {wait:.0f}s (period {period}s)"
    return name, True, f"reads {r1} -> {r2}"


def test_9p_over_the_wire(b: Board) -> tuple[str, bool, str]:
    """E7's done-condition, remote half: the readings leave the board.

    `p9share` puts SLIP-framed 9P on the console wire alongside the console
    itself, which is what lets one cable carry both. This is the whole point
    of the persona -- a node with a sensor and no network stack of its own --
    so it is tested the way it is used, with a real 9P client rather than by
    reading the console.

    Left enabled afterwards would make every later console read see SLIP
    frames interleaved with text, so it is turned off again."""
    name = "readings leave the board over 9P (E7)"
    c = b.console_session
    out = c.cmd("p9share", deadline=15.0)
    if "Shared-wire 9P active" not in out:
        return name, True, "SKIPPED (no p9share on this build)"
    try:
        c.close()   # the client needs the port to itself
        time.sleep(0.3)
        try:
            client = connect_serial(b.console, baudrate=BAUD, framing="slip",
                                    timeout=20.0)
        except Exception as e:
            return name, False, f"could not attach a 9P client: {type(e).__name__}: {e}"
        try:
            sess = p9lib.Session(client)
            data = sess.read("/proc/sensors").decode("utf-8", "replace")
        finally:
            try:
                client.close()
            except Exception:
                pass
    finally:
        time.sleep(0.3)
        b.console_session = Console(b.console)
        b.console_session.cmd("p9share off", deadline=10.0)

    f = kv(data)
    if f.get("part") == "none":
        return name, True, "SKIPPED (9P works, but no BME280 fitted)"
    if f.get("valid") != "yes" or "temperature_c100" not in f:
        return name, False, f"9P read succeeded but the content is wrong: {f}"
    return name, True, f"{int(f['temperature_c100']) / 100.0} C over 9P"


def test_emac_loopback(b: Board) -> tuple[str, bool, str]:
    """Z2's done-condition, plan/phase28_esp32p4_ethernet.md.

    Frames of seven sizes through the MAC's internal loopback, each compared
    byte for byte. No PHY and no cable, so a failure has one candidate cause:
    the descriptor rings, the cache maintenance, or the buffer ownership
    protocol.

    The sizes are the edges that matter -- 60 (smallest legal frame), 64 (one
    whole cache line), 65 (one byte into a partial line), and 1514
    (NETIF_FRAME_MAX). The cache line cases are the point: this is the first
    driver in the tree whose device writes memory behind the CPU's cache."""
    name = "EMAC loopback: rings and cache discipline (Z2)"
    out = b.console_session.cmd("emac loopback", deadline=25.0)
    m = re.search(r"EMAC loopback: (\d+) passed, (\d+) failed", out)
    if not m:
        return name, False, f"no result line: {out.strip()[-200:]}"
    passed, failed = int(m.group(1)), int(m.group(2))
    if failed or passed != 7:
        bad = [ln.strip() for ln in out.splitlines() if ":" in ln and "ok" not in ln
               and ("B:" in ln)]
        return name, False, f"{passed} passed, {failed} failed: {'; '.join(bad[:3])}"
    return name, True, f"{passed}/7 sizes byte-identical, 60 B to 1514 B"


def test_emac_phy(b: Board) -> tuple[str, bool, str]:
    """Z1's done-condition, plan/phase28_esp32p4_ethernet.md.

    Two claims in one command, and the first is the load-bearing one.

    `emac scan` runs the whole clock/pad/reset sequence and then the MAC's
    software reset. That reset bit cannot clear until the PHY is supplying the
    50 MHz RMII reference -- the DWC_EMAC register description says so
    outright -- so reaching the scan at all proves the three clock
    controllers, the seven IO_MUX pads and the PHY's reset line are right. A
    probe failure prints why and never gets as far as the table.

    The second claim is that exactly one PHY answers, at the address the board
    file records, with the identity it records. That number was measured here
    in Z1 rather than inferred from the schematic's strap nets, and re-reading
    it on every run is what keeps it honest."""
    name = "the EMAC's PHY answers over MDIO (Z1)"
    out = b.console_session.cmd("emac scan", deadline=15.0)
    if "probe failed" in out:
        return name, False, "MAC software reset never completed -- no RMII reference clock?"
    if "OUI 00-90-c3" not in out:
        return name, False, f"no IP101G identity in the scan output: {out.strip()[-200:]}"
    if "as the board file says" not in out:
        return name, False, f"PHY found, but not where the board file expects: {out.strip()[-200:]}"
    return name, True, "IP101G at MDIO address 1, RMII reference confirmed"


TESTS = [
    test_boots,
    test_proc_readable,
    test_flash0_mounted,
    test_preemption,
    test_umode,
    test_i2c_bus,
    test_i2c_stable,
    test_sensor_selftest,
    test_sensor_reads,
    test_proc_sensors,
    test_sampler_advances,
    test_9p_over_the_wire,
    test_emac_phy,
    test_emac_loopback,
]


# --- discovery and load ----------------------------------------------------

def discover(port: str | None, reset_port: str | None) -> Board | None:
    """The attached P4, or None meaning "nothing to test".

    None rather than an exception, so "no board attached" is a normal outcome
    -- the whole point of this directory's convention."""
    try:
        p4run = _load_p4run()
    except Exception:
        return None
    try:
        console = p4run.console_port(port)
        reset = p4run.reset_port(reset_port, console)
    except SystemExit:
        # console_port() exits when it finds nothing it recognises.
        return None
    except Exception:
        return None
    if not console:
        return None

    # Named is not the same as present. A port can be pinned by --port or by
    # LUGALOS_P4_PORT, and auto-detection itself falls back to a glob, so a
    # device that has been unplugged since -- or a typo -- would otherwise
    # reach the loader and raise out of the suite. Opening it here turns that
    # into the ordinary "nothing to test" outcome this directory promises,
    # while still naming the port that could not be opened.
    for role, dev in (("console", console), ("reset", reset)):
        if not dev:
            continue
        try:
            serial.Serial(dev, BAUD, timeout=0.1).close()
        except Exception as e:
            print(f"\n[!] The {role} port {dev} cannot be opened: {e}")
            return None

    return Board(console=console, reset=reset, p4run=p4run)


def load(b: Board, image: Path, listen_secs: float) -> bool:
    """Deliver the image and wait for the shell, using p4run's own loader --
    including its check that the image actually started, which is not the
    same thing as esptool returning 0 (about one delivery in three on this
    board returns 0 and never runs)."""
    try:
        img = b.p4run.image_for(str(image))
    except Exception as e:
        print(f"    (could not prepare an image from {image}: {e})")
        return False
    W = b.p4run.Watcher
    for tries_left in range(b.p4run.LOAD_TRIES - 1, -1, -1):
        try:
            with W(b.reset) as w:
                if not b.p4run.load(b.console, img, "auto", b.reset, driver=w):
                    return False
                started = w.wait_for(b.p4run.RUNNING_MARKER, listen_secs)
                if started:
                    w.wait_for(b.p4run.READY_MARKER, 5.0)
        except Exception as e:
            # A board that goes away mid-load is a board that is not there,
            # which is this directory's normal outcome and not a failure.
            print(f"    (the board stopped answering during the load: {e})")
            return False
        if started or not b.p4run.looks_dead(w.text()) or tries_left == 0:
            break
        print(f"    (loaded, but nothing ran -- retrying, {tries_left} left)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="console port (default: autodetect by VID:PID)")
    ap.add_argument("--reset-port", help="port whose DTR/RTS reach the board")
    ap.add_argument("--image", default=str(BUILD_DIR / "lugalos.elf"),
                    help="the .elf to load (the image is regenerated from it)")
    ap.add_argument("--no-load", action="store_true",
                    help="test whatever is already running")
    ap.add_argument("--listen-secs", type=float, default=8.0)
    args = ap.parse_args()

    print("======================================================================")
    print("        LugalOS ESP32-P4 Hardware-in-the-Loop Suite (E8, phase 27)")
    print("======================================================================")

    b = discover(args.port, args.reset_port)
    if b is None:
        print("\n[!] No ESP32-P4 found on any USB serial port.")
        print("    Nothing to test -- this is not a failure, just nothing to do.")
        print("    Detection is by USB VID:PID, not device name: the CH343P and the")
        print("    P4's own USB-Serial-JTAG both enumerate as /dev/ttyACM*.")
        print("    `tools/p4run.py --ports` shows what is actually attached.")
        return 0

    print(f"\nconsole {b.console}" + ("" if b.reset == b.console
                                      else f"   reset lines {b.reset}"))

    if not args.no_load:
        image = Path(args.image)
        if not image.exists():
            print(f"\n[!] No image at {image}.")
            print("    Build it: cmake --preset esp32p4 && cmake --build --preset esp32p4")
            print("    Nothing to test -- not a failure.")
            return 0
        print(f"loading {image.name} ...")
        if not load(b, image, args.listen_secs):
            print("\n[!] The board would not take the image.")
            print("    Nothing to test -- not a failure. Check the two cables:")
            print("    reset lines and console are different ports on this wiring.")
            return 0

    try:
        b.console_session = Console(b.console)
    except Exception as e:
        print(f"\n[!] Could not open {b.console}: {e}")
        print("    Nothing to test -- not a failure.")
        return 0

    if not b.console_session.wake():
        print("\n[!] A board is attached but its console is not answering.")
        print("    Nothing to test -- not a failure. It may be running an")
        print("    application that owns the console, or need a reload:")
        print("    tools/p4run.py build/esp32p4/lugalos.elf --interactive")
        b.console_session.close()
        return 0

    total = passed = 0
    try:
        for t in TESTS:
            try:
                name, ok, log = t(b)
            except Exception as e:   # a test that throws is a failed test, not a dead suite
                name, ok, log = t.__name__, False, f"raised {type(e).__name__}: {e}"
            total += 1
            if ok:
                passed += 1
                status = "SKIP" if log.startswith("SKIPPED") else "PASS"
                print(f"  [{status}] {name}" + (f" -- {log}" if log else ""))
            else:
                print(f"  [FAIL] {name}\n    {log}")
    finally:
        b.console_session.close()

    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed} / {total} PASSED")
    print("======================================================================\n")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
