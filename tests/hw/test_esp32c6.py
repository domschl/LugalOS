#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for the **ESP32-C6** (Waveshare ESP32-C6-Zero)
-- 45.4, plan/phase45_esp32c6.md.

The sibling of test_esp32p4.py and test_rp2350.py and it follows their
conventions: every test returns (name, ok, detail), and everything *skips* rather
than fails when no board is attached, so it is safe to run speculatively.

QEMU has no ESP32-C6 machine, so the things this chip does differently from every
other target -- the interrupt matrix and PLIC, the 256-byte-aligned vectored
mtvec, the flash MMU, a PMP with 16 entries at 4-byte granularity, the ROM's data
at the top of SRAM -- are exercised here or nowhere. The portable kernel
(scheduler, locks, kernel objects, the U-mode isolation proofs) is also run, on
the silicon, because what QEMU cannot show is that it behaves the same on a core
that is not QEMU's.

**This suite loads the kernel itself**: tools/c6run.py --kernel writes the
flash-resident half (only if it changed) and delivers the RAM half over the
board's one USB cable; nobody has to press BOOT. Pass --no-load to test whatever
is already running (for instance after tools/c6flash.py).

Usage:
    uv run test_esp32c6.py                 # auto-detect, load, test
    uv run test_esp32c6.py --no-load       # test what is running
    uv run test_esp32c6.py --port /dev/serial/by-id/usb-Espressif_...
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import re
import sys
import time
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build" / "esp32c6"


def _load_c6run():
    """tools/c6run.py as a module: it already owns port detection, the flash-if-
    changed logic and the RAM load, and a second copy would be a second copy that
    drifts. Import-safe (everything is inside main())."""
    path = REPO_ROOT / "tools" / "c6run.py"
    spec = importlib.util.spec_from_file_location("c6run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Console:
    """The board's console. Opened with DTR high and RTS low: on USB-Serial/JTAG
    DTR low while RTS is high is the chip's reset line (plan §45.1)."""

    def __init__(self, port: str):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.1
        self.s.dtr, self.s.rts = True, False
        self.s.open()
        self.log = ""

    def close(self):
        self.s.close()

    def _pump(self, secs: float) -> str:
        end, got = time.time() + secs, ""
        while time.time() < end:
            d = self.s.read(512)
            if d:
                got += d.decode("utf-8", "replace")
        self.log += got
        return got

    def run(self, cmd: str, until: str, timeout: float = 10.0) -> tuple[bool, str]:
        """Type a command, collect output until `until` (a regex) appears."""
        self.s.reset_input_buffer()
        self.s.write(cmd.encode() + b"\r")
        out, end = "", time.time() + timeout
        while time.time() < end:
            out += self._pump(0.2)
            if re.search(until, out):
                return True, out
        return False, out


def _t(name, ok, detail=""):
    return name, bool(ok), detail


def test_boots(c: Console):
    ok, out = c.run("version", r"LugalOS v[\d.]+", 5)
    return _t("boots to a shell and answers `version`", ok, out[-120:])


def test_heap(c: Console):
    ok, out = c.run("cat /proc/meminfo", r"Heap: (\d+) KB managed", 5)
    m = re.search(r"Heap: (\d+) KB managed", out)
    kb = int(m.group(1)) if m else 0
    return _t("heap above the 128 KB floor, SRAM below the ROM's data", ok and kb >= 128 and "496 KB total" in out, "%d KB" % kb)


def test_tick_rate(c: Console):
    ok, out = c.run("intrdump", r"ticks after 200 ms of spinning: \d+ \(\+(\d+)\)", 8)
    m = re.search(r"\(\+(\d+)\)", out)
    n = int(m.group(1)) if m else -1
    return _t("system-timer tick through matrix+PLIC at 100 Hz (20 per 200 ms)", ok and 18 <= n <= 22, "+%d" % n)


def test_preemption(c: Console):
    ok, out = c.run("preempttest", r"\[Preempt\] ticks=\d+ flag=\d", 30)
    return _t("a spinning task is preempted by the tick", ok and "PREEMPTED" in out and "NOT PREEMPTED" not in out, out[-110:])


def test_locks(c: Console):
    ok, out = c.run("lockselftest", r"LOCK_SELFTEST_(OK|FAIL)", 30)
    return _t("lock primitives (16 checks)", ok and "LOCK_SELFTEST_OK" in out, out[-100:])


def test_priostress(c: Console):
    ok, out = c.run("priostress", r"PrioStress\] done=.*", 90)
    return _t("same-tier tasks share the CPU fairly", ok and "FAIR" in out, out[-110:])


def test_pmp(c: Console):
    ok, out = c.run("pmpinfo", r"Free for B3: \d+", 5)
    return _t("PMP: 16 entries, 8-byte minimum region, nothing locked",
              ok and "writable=16" in out and "min_region=8 bytes" in out and "locked=no" in out, out[-130:])


def test_umode(c: Console):
    ok, out = c.run("usertest", r"task ended cleanly|FAIL", 10)
    return _t("a task really drops to U-mode (ecall cause 8)", ok and "cause: 8" in out, out[-100:])


def test_isolation(c: Console):
    ok, out = c.run("isolationtest", r"ISOLATED|NOT ISOLATED|FAIL", 10)
    return _t("a U-mode store into kernel memory faults and the canary survives", ok and "ISOLATED (kernel memory untouched)" in out, out[-110:])


def test_deputy(c: Console):
    ok, out = c.run("deputytest", r"UNTOUCHED|CORRUPTED|FAIL", 10)
    return _t("the kernel refuses to write kernel memory on a U-mode task's behalf", ok and "DEPUTY_REFUSED" in out and "UNTOUCHED" in out, out[-110:])


def test_kobj_kernel(c: Console):
    ok, out = c.run("kobjselftest", r"KOBJSELFTEST_(OK|FAIL)", 60)
    m = re.search(r"(\d+) granted, (\d+) left", out)
    race = bool(m) and int(m.group(1)) > 5 and int(m.group(2)) > 5
    return _t("kernel objects under real tasks, timeouts and a 100-round race that goes both ways",
              ok and "KOBJSELFTEST_OK" in out and race, m.group(0) if m else out[-80:])


def test_kobj_umode(c: Console):
    ok, out = c.run("kobjutest", r"KOBJUTEST_(OK|FAIL)", 30)
    return _t("every kernel-object syscall from a U-mode task (29 checks, boundary included)",
              ok and "KOBJUTEST_OK" in out and out.count("PASS") >= 29, "%d PASS" % out.count("PASS"))


def test_radio_shim(c: Console):
    ok, out = c.run("radioosi", r"RADIOOSI_(OK|FAIL)", 30)
    return _t("the Wi-Fi OS-table shim in a confined U-mode domain (22 checks)",
              ok and "RADIOOSI_OK" in out and out.count("PASS") >= 22, "%d PASS" % out.count("PASS"))


def test_led(c: Console):
    """The frame must complete (the RMT's end-of-transmission flag). That the LED
    shows the right colour only an eye can say; confirmed once, 45.5."""
    ok1, o1 = c.run("led 0 0 20", r"lsh>", 4)
    ok2, o2 = c.run("led off", r"lsh>", 4)
    bad = "never finished" in o1 + o2 or "usage" in o1 + o2
    return _t("a WS2812 frame goes out through the RMT and completes (led R G B / off)", ok1 and ok2 and not bad)


def _wifi_credentials() -> tuple[str, str] | None:
    """(ssid, 64-hex derived PSK) from ~/.config/lugalos/wifi.env (wpa_supplicant.conf style:
    network={ ssid="..." psk=... }), or None. A quoted psk is a passphrase and is derived here
    (PBKDF2-HMAC-SHA1, 4096 rounds, the SSID as salt); the board only ever sees the derived key."""
    path = Path.home() / ".config" / "lugalos" / "wifi.env"
    try:
        text = path.read_text()
    except OSError:
        return None
    ssid = re.search(r'ssid\s*=\s*"([^"]*)"', text)
    psk = re.search(r'psk\s*=\s*("[^"]*"|[0-9a-fA-F]{64})', text)
    if not ssid or not psk:
        return None
    key = psk.group(1)
    if key.startswith('"'):
        key = hashlib.pbkdf2_hmac("sha1", key.strip('"').encode(), ssid.group(1).encode(), 4096, 32).hex()
    return ssid.group(1), key


def test_radio_join(c: Console):
    """Associate with the configured network: WPA2-PSK 4-way handshake through the supplicant.
    The command (which carries the derived key) is never printed. Needs a reboot-fresh radio, so
    it replaces the scan test's run when credentials exist (see TESTS)."""
    cred = _wifi_credentials()
    if cred is None:
        return _t("associate with ~/.config/lugalos/wifi.env (skipped: no credentials)", True, "skipped")
    ok, out = c.run("radio join %s %s" % cred, r"radio: stage \d", 90)
    return _t("WPA2-PSK association through the supplicant", ok and "stage 7" in out,
              "joined" if "stage 7" in out else "; ".join(re.findall(r"radio3\] radio: (?:join|event)[^\n]*", out))[:140])


def test_radio_scan(c: Console):
    """The Wi-Fi blob in its U-mode domain: init, PHY calibration, interrupts through the
    interrupt thread, a scan of all 14 channels. One start per boot, and it needs at least
    one access point in range -- a test that depends on the room, said so in its name."""
    ok, out = c.run("radio", r"radio: stage \d", 60)
    m = re.search(r"scan found (\d+) access points", out)
    n = int(m.group(1)) if m else 0
    fired = re.search(r"fired (\d+) times", out)
    return _t("Wi-Fi scan through the blob: interrupts delivered, access points found (needs one in range)",
              ok and "stage 6" in out and n >= 1 and bool(fired) and int(fired.group(1)) > 0,
              "%d APs, %s irqs" % (n, fired.group(1) if fired else "?"))


def test_radio_scan_or_join(c: Console):
    """The radio starts once per boot, so one test covers both: with credentials it scans *and*
    joins (the scan runs first either way), without them just scans."""
    if _wifi_credentials() is None:
        return test_radio_scan(c)
    return test_radio_join(c)


def test_still_alive(c: Console):
    """A kernel that boots and then resets (the flash-boot watchdogs, a crash) looks
    perfect to every quick test above and dies minutes later -- the P4's phase 32
    learned that the expensive way. So wait until the kernel's own clock says it has
    survived a minute, and check it is still counting."""
    t = 0.0
    for _ in range(3):
        ok, out = c.run("intrdump", r"\[\s*(\d+\.\d+)\] \[INTC\] ticks=", 8)
        m = re.search(r"\[\s*(\d+\.\d+)\] \[INTC\] ticks=", out)
        t = float(m.group(1)) if m else 0.0
        if not ok or t > 65:
            break
        time.sleep(min(70.0 - t, 65.0))
    return _t("no watchdog or crash reset in a minute (the kernel clock keeps counting)", t > 65, "up %.0f s" % t)


TESTS = [test_boots, test_heap, test_tick_rate, test_preemption, test_locks, test_priostress,
         test_pmp, test_umode, test_isolation, test_deputy, test_kobj_kernel, test_kobj_umode,
         test_radio_shim, test_led, test_radio_scan_or_join, test_still_alive]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port")
    ap.add_argument("--no-load", action="store_true", help="test whatever is running")
    a = ap.parse_args()

    c6run = _load_c6run()
    try:
        port = c6run.find_port(a.port)
    except SystemExit as e:
        print("SKIP: %s" % e)
        return 0

    if not a.no_load:
        if not (BUILD_DIR / "lugalos-ram.elf").exists():
            print("SKIP: build/esp32c6 is not built (ninja -C build/esp32c6)")
            return 0
        c6run.kernel_flash_if_changed(port, str(BUILD_DIR))
        if not c6run.load(port, c6run.image_for(str(BUILD_DIR / "lugalos-ram.elf"))):
            print("FAIL: could not load the kernel")
            return 1
        time.sleep(1.5)

    c = Console(port)
    c._pump(1.0)
    failed = 0
    try:
        for fn in TESTS:
            name, ok, detail = fn(c)
            print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail.strip().replace("\n", " ")[:110]) if detail else ""))
            failed += not ok
    finally:
        c.close()
    print("\n%d / %d passed" % (len(TESTS) - failed, len(TESTS)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
