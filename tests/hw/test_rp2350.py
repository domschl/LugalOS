#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for RP2350 (A3b: link_usb_cdc, p9share --
plan/phase5_distributed_design.md's M5/T3). Talks to a real, physically
attached board; every test is *skipped* (not failed) when no RP2350 is
found, so this is safe to run speculatively and safe to leave out of CI.

Usage:
    uv run test_rp2350.py
    uv run test_rp2350.py --console /dev/tty.usbmodemXXXX1 --net /dev/tty.usbmodemXXXX3
    uv run test_rp2350.py --skip-qemu-bridge

Wiring assumed: RP2350 flashed with the current build/rp2350/lugalos.uf2,
connected over USB (enumerates two CDC-ACM ports: console + link_usb_cdc's
net port), optionally also wired to a host CP2101/CP2102 UART adapter for
the p9share test (see README.md's "CP2101 -> RP2350 Wiring" section).
"""

from __future__ import annotations

import argparse
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

import serial

import rp2350
import p9lib  # via rp2350.py's sys.path insert

REPO_ROOT = rp2350.REPO_ROOT


def test_umode_isolation(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """B3 on real silicon: U-mode, PMP-enforced isolation, and the syscall
    boundary.

    Worth running here rather than trusting QEMU, because every one of B3's
    hardware surprises was invisible on QEMU: Hazard3 implements no TOR, uses a
    32-byte granule, and reverses the PMP permission bit order under erratum
    E6. QEMU has none of those properties, so a build that passes there says
    nothing about whether this board is actually enforcing anything.

    Three claims, each with its own failure mode:
      usertest      -- trap cause 8 proves the privilege level really dropped
      isolationtest -- a U-mode store into kernel memory faults; canary intact
      deputytest    -- the kernel refuses a foreign destination pointer, and
                       still accepts one the task owns
    """
    name = "B3: U-mode isolation and syscall boundary on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            for cmd in (b"usertest\n", b"isolationtest\n", b"deputytest\n"):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=0.8, deadline=12.0).decode("utf-8", "replace")

        checks = [
            ("privilege level dropped", "cause: 8 (U-mode" in out),
            ("kernel memory isolated",  "ISOLATED (kernel memory untouched)" in out),
            ("foreign pointer refused", "DEPUTY_REFUSED" in out),
            ("own pointer still works", "OWNBUF_OK" in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-400:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_user_elf(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """B6 on real silicon: a separately linked ELF, loaded from the flash
    filesystem and run in U-mode under a PMP-enforced domain.

    Distinct from test_umode_isolation above, which runs kernel code from a
    kernel section under a domain the kernel built around its own image. Here
    the program arrived as a file, was parsed by the loader, and was placed in
    pages the allocator handed out -- so this is the path an untrusted binary
    actually takes, and until B6 it did not involve U-mode at all: `exec`
    called the loaded bytes directly, at machine privilege.

    Worth running on hardware rather than trusting QEMU for the same reason
    B3 was: the loader's image model exists to satisfy PMP's NAPOT rules, and
    Hazard3 is the core that actually has them -- 32-byte granule, no TOR, and
    the reversed permission bit order of erratum E6. A domain that grants R|X
    on QEMU and X|W here would run the program and silently drop its
    isolation.

    Three claims:
      uhello    -- .rodata, a writable .bss page, a pointer-taking syscall,
                   and a normal return carried out through the exit stub
      uisolate  -- a store outside the domain faults; the program never
                   reaches the line that would report it succeeded
      uspin     -- a timer interrupt reaches user code on this silicon too
    """
    name = "B6: separately linked ELF runs confined in U-mode on real silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            out = ""
            # uspin is *expected* to be silent while it computes -- that is
            # the whole point of its measurement window -- so it needs a quiet
            # threshold longer than the spin. A shorter one does not fail the
            # test honestly, it truncates the capture and reports the marker
            # as missing.
            for cmd, quiet in ((b"exec /flash0/uhello.elf\n", 0.8),
                               (b"exec /flash0/uisolate.elf\n", 0.8),
                               (b"exec /flash0/uspin.elf\n", 3.0)):
                ser.write(cmd)
                ser.flush()
                out += rp2350.drain(ser, quiet=quiet, deadline=30.0).decode("utf-8", "replace")

        checks = [
            ("program text ran",        "UPROG_TEXT_OK" in out),
            ("data page writable",      "UPROG_DATA_OK" in out),
            ("syscall copied a file",   "UPROG_FILE_OK LugalOS" in out),
            ("returned via exit stub",  "returned 7" in out),
            ("out-of-domain store faulted",
             "UISO_ALIVE" in out and "UISO_NOT_ISOLATED" not in out
             and "terminated before it could exit" in out),
            ("user code preempted",
             "USPIN_PREEMPTED" in out and "USPIN_NOT_PREEMPTED" not in out),
        ]
        failed = [label for label, ok in checks if not ok]
        if failed:
            return (name, False, f"failed: {', '.join(failed)}\n{out[-600:]}")
        return (name, True, "; ".join(label for label, _ in checks))
    except Exception as e:
        return (name, False, str(e))


def test_usb_cdc_net_link(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """link_usb_cdc standalone: a host 9P client reads /proc/version over
    ACM1 (plain length-prefixed framing -- no SLIP, matching virtio-console's
    own reasoning: a reliable USB bulk pipe needs no escape/resync
    machinery). /proc/version is synthetic (A1) so this doesn't depend on
    whatever happens to be on the board's physical SD card."""
    name = "link_usb_cdc: 9P /proc/version read over ACM1"
    try:
        with serial.Serial(ports.net, 115200, timeout=5) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            adapter = rp2350.SerialSocketAdapter(ser)
            client = p9lib.P9Client(adapter, framing="raw")
            if not rp2350.warm_up_9p(client, adapter):
                return (name, False, "no response from ACM1 after repeated Tversion attempts")
            data = client.cat("/proc/version")
            ok = b"LugalOS" in data
            return (name, ok, "" if ok else f"unexpected content: {data!r}")
    except Exception as e:
        return (name, False, str(e))


def test_firmware_freshness(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """Checks the board is running the firmware this tree would build.

    Reported as a WARNING rather than a failure: a deliberately older board is
    a legitimate thing to test against. But a silent mismatch is not -- an
    earlier session lost a full flash-and-measure cycle to a stale UF2 whose
    only symptom was one command being missing, which looked like a broken
    feature rather than a stale board."""
    name = "Firmware freshness: board build id matches the local build"
    want = rp2350.local_build_id()
    if want is None:
        return (name, True, "SKIPPED: no build/rp2350/ build to compare against")
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()
            ser.write(b"cat /proc/buildid\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=0.5, deadline=5.0).decode("utf-8", errors="replace")

        m = re.search(r"(\d+\.\d+\.\d+)\s+(\S+)", out.replace("cat /proc/buildid", ""))
        if not m:
            return (name, True,
                    "SKIPPED: board predates /proc/buildid -- it is definitely older than this "
                    f"tree (expected build {want}); reflash build/rp2350/lugalos.uf2")
        got = m.group(2)
        if got != want:
            return (name, True,
                    f"WARNING: board is running build {got}, local tree builds {want} -- "
                    "reflash build/rp2350/lugalos.uf2 if the tests below look wrong")
        return (name, True, f"build {got}")
    except Exception as e:
        return (name, False, str(e))


def test_pmp_probe(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """B3 prep (D2): read this silicon's actual PMP configuration.

    D2 resolved to "PMP early, NOMMU leads" -- RV32/RP2350 gets U-mode plus
    PMP enforcement in B3, ahead of Sv39 in B5. How many isolated servers a
    NOMMU node can host is bounded by the implemented region count, and the
    RISC-V privileged spec permits 0, 16 or 64 entries, so this is a property
    of Hazard3 that has to be measured rather than assumed. QEMU's RV32 model
    reports 16 entries at 4-byte granularity; there is no reason to expect
    real Hazard3 silicon to match, which is the entire point of running it
    here.

    This is a *measurement*, so it asserts only the two things B3 genuinely
    requires -- that PMP exists at all, and that nothing is already locked at
    boot (a locked entry cannot be reprogrammed until reset). The numbers
    themselves are printed for the plan to record, not compared against a
    hardcoded expectation that would just encode today's guess.
    """
    name = "PMP: probe entry count and granularity on real Hazard3 silicon"
    try:
        with serial.Serial(ports.console, 115200, timeout=2) as ser:
            ser.dtr = True
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"pmpinfo\n")
            ser.flush()
            out = rp2350.drain(ser, quiet=0.5, deadline=5.0)

        text = out.decode("utf-8", errors="replace")
        m = re.search(r"PMP: writable=(\d+) active=(\d+) min_region=(\d+) bytes locked=(\w+)", text)
        if not m:
            # Distinguish "the board is running older firmware" from "the probe
            # is broken". The former is by far the more likely cause the first
            # time this test is run, and it needs a completely different fix,
            # so guessing wrong here wastes real debugging time.
            if "Unbound symbol: pmpinfo" in text or "Unknown command" in text:
                return (name, False,
                        "board firmware predates the `pmpinfo` command -- reflash it with the "
                        "current build/rp2350/lugalos.uf2 and re-run. (LugalOS does not implement "
                        "the 1200-baud BOOTSEL touch, so this flash is manual: hold BOOTSEL while "
                        "connecting, then copy the .uf2 to the mounted volume.)")
            if "PMP: unavailable" in text:
                return (name, False,
                        "board reports PMP unavailable -- an M-mode build should have access; "
                        f"got:\n{text[:300]}")
            return (name, False, f"no PMP report in console output:\n{text[:300]}")

        writable = int(m.group(1))
        active = int(m.group(2))        # currently in use -- writable but not free
        min_region = int(m.group(3))
        locked = m.group(4) == "yes"
        free = writable - active        # what B3 can actually take without deciding what loses access

        detail = (f"writable={writable} active={active} free={free} "
                  f"min_region={min_region}B locked={locked}")

        if writable == 0:
            return (name, False, f"no writable PMP entries -- B3 has no enforcement mechanism ({detail})")
        if free <= 1:
            return (name, False,
                    f"only {free} free PMP region(s) -- B3 needs at least one per isolated task "
                    f"plus one for the kernel ({detail})")
        if locked:
            return (name, False, f"a PMP entry is already locked at boot; B3 cannot reprogram it ({detail})")

        # Informational, not a failure: B3's design has to fit this budget.
        print(f"    ...measured: {detail}; isolatable tasks for B3 = {free - 1} "
              f"(+{active} reclaimable)")
        return (name, True, detail)
    except Exception as e:
        return (name, False, str(e))


def test_uart_demux_shared_wire(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """p9share (A3b UART demux) standalone: arms the demux over the physical
    UART, drives one real SLIP-framed 9P transaction, then sends a
    plain-text console command over the SAME connection and checks for a
    real shell response -- the actual claim A3b makes (console and 9P
    coexist on one wire), verified against real hardware rather than only
    QEMU's stdio-socket regression test. Restores console-only mode
    afterward (`p9share off`) so re-running this doesn't leave the board in
    a different state than it found it."""
    name = "p9share: 9P + console coexist on the physical UART"
    if not ports.uart:
        return (name, True, "SKIPPED: no UART/CP2102 dongle detected")

    try:
        with serial.Serial(ports.uart, 115200) as ser:
            time.sleep(0.3)
            ser.reset_input_buffer()

            ser.write(b"p9share\n")
            ser.flush()
            rp2350.drain(ser)  # discard banner

            ser.reset_input_buffer()
            adapter = rp2350.SerialSocketAdapter(ser)
            client = p9lib.P9Client(adapter, framing="slip")
            if not rp2350.warm_up_9p(client, adapter):
                return (name, False, "no response over shared UART after repeated Tversion attempts")
            data = client.cat("/proc/version")
            if b"LugalOS" not in data:
                return (name, False, f"9P transaction over shared wire failed: {data!r}")

            ser.reset_input_buffer()
            ser.write(b"help\n")
            ser.flush()
            console_out = rp2350.drain(ser, quiet=0.5, deadline=4.0)
            ok = b"p9share" in console_out

            ser.reset_input_buffer()
            ser.write(b"p9share off\n")
            ser.flush()
            rp2350.drain(ser)

            return (name, ok, "" if ok else f"console unresponsive after 9P traffic: {console_out[:200]!r}")
    except Exception as e:
        return (name, False, str(e))


def test_qemu_bridge(ports: rp2350.Rp2350Ports) -> tuple[str, bool, str]:
    """T3 itself: bridges RP2350's ACM1 to a live QEMU RV64 node's
    virtio-console chardev (a plain byte relay -- both ends already speak
    the same length-prefixed framing, no re-framing needed) and runs
    (p9-remote-cat ...) from *inside the QEMU guest's own Lisp REPL* against
    a marker file this test just wrote to the RP2350's /ram0 over the
    console. Using a freshly written, uniquely-named marker (rather than a
    pre-existing file on the board's physical SD card) makes this
    reproducible regardless of what's actually on that card."""
    name = "T3: QEMU guest reads a real file from RP2350 hardware over the bridge"

    qemu_bin = shutil.which("qemu-system-riscv64")
    if not qemu_bin:
        return (name, True, "SKIPPED: qemu-system-riscv64 not found in PATH")

    rv64_elf = REPO_ROOT / "build" / "rv64" / "lugalos.elf"
    sd_img = REPO_ROOT / "build" / "lugalos_sd.img"
    if not rv64_elf.exists():
        return (name, True, f"SKIPPED: {rv64_elf} not built")

    marker_name = f"hwbridge_{uuid.uuid4().hex[:12]}.txt"
    marker_value = uuid.uuid4().hex
    marker_path = f"/ram0/{marker_name}"

    proc: subprocess.Popen | None = None
    ser: serial.Serial | None = None
    sock: socket.socket | None = None
    stop = threading.Event()
    bridge_threads: list[threading.Thread] = []

    try:
        # 1. Write a unique marker to the board's own RAM disk over the console.
        with serial.Serial(ports.console, 115200, timeout=2) as console:
            time.sleep(0.3)
            console.reset_input_buffer()
            console.write(f"write {marker_path} {marker_value}\n".encode())
            console.flush()
            reply = rp2350.drain(console, quiet=0.5, deadline=4.0)
            if b"=> #t" not in reply:
                return (name, False, f"failed to write marker file on RP2350: {reply[:300]!r}")

        # 2. Boot a QEMU node with a socket-backed virtio-console chardev.
        with tempfile.TemporaryDirectory() as tmpdir:
            sock_path = str(Path(tmpdir) / "rp2350_bridge.sock")
            bridge_img = REPO_ROOT / "build" / "rp2350_hw_bridge_test_sd.img"
            shutil.copyfile(sd_img, bridge_img)

            qemu_cmd = [
                qemu_bin, "-M", "virt", "-nographic", "-bios", "none",
                "-drive", f"file={bridge_img},if=none,format=raw,id=hd0",
                "-device", "virtio-blk-device,drive=hd0",
                "-kernel", str(rv64_elf),
                "-device", "virtio-serial-device",
                "-device", "virtconsole,chardev=p9c",
                "-chardev", f"socket,id=p9c,path={sock_path},server=on,wait=off",
            ]
            proc = subprocess.Popen(qemu_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
            out_lines: list[str] = []
            threading.Thread(target=lambda: [out_lines.append(l) for l in proc.stdout], daemon=True).start()

            time.sleep(3.0)  # let it boot to the shell prompt

            # 3. Bridge RP2350's ACM1 to that socket -- but first, warm the
            # connection up with a throwaway host-side Tversion on this SAME
            # handle (see rp2350.warm_up_9p()'s docstring: the first frame
            # on a freshly opened CDC port can get silently dropped, and
            # QEMU's own p9-remote-cat has no timeout of its own -- it would
            # just hang forever against a dropped first frame instead of
            # failing loudly). Reusing this one handle for both the warm-up
            # and the relay below means there's only one `open()` boundary
            # to worry about, not two.
            ser = serial.Serial(ports.net, 115200, timeout=1.0)
            ser.dtr = True
            time.sleep(0.2)
            warmup_adapter = rp2350.SerialSocketAdapter(ser)
            if not rp2350.warm_up_9p(p9lib.P9Client(warmup_adapter, framing="raw"), warmup_adapter):
                return (name, False, "no response from ACM1 after repeated Tversion attempts")
            ser.timeout = 0.2

            last_err: Exception | None = None
            for _ in range(20):
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.connect(sock_path)
                    break
                except OSError as e:
                    last_err = e
                    time.sleep(0.2)
            if sock is None:
                return (name, False, f"could not connect to QEMU virtio-console socket: {last_err}")

            def serial_to_sock():
                while not stop.is_set():
                    data = ser.read(1)
                    if not data:
                        continue
                    if ser.in_waiting:
                        data += ser.read(ser.in_waiting)
                    try:
                        sock.sendall(data)
                    except OSError:
                        return

            def sock_to_serial():
                sock.settimeout(0.2)
                while not stop.is_set():
                    try:
                        data = sock.recv(4096)
                    except socket.timeout:
                        continue
                    except OSError:
                        return
                    if not data:
                        return
                    ser.write(data)
                    ser.flush()

            t1 = threading.Thread(target=serial_to_sock, daemon=True)
            t2 = threading.Thread(target=sock_to_serial, daemon=True)
            t1.start()
            t2.start()
            bridge_threads.extend([t1, t2])

            # 4. Drive the real cross-wire 9P request from inside the QEMU guest.
            def send(cmd: str) -> None:
                proc.stdin.write(cmd + "\n")
                proc.stdin.flush()

            time.sleep(0.5)
            send("lisp")
            time.sleep(1.0)
            send(f'(p9-remote-cat "{marker_path}")')
            time.sleep(2.0)
            send("exit")
            time.sleep(1.0)

            stop.set()
            text = "".join(out_lines)
            ok = marker_value in text
            log = "" if ok else f"marker not found in QEMU output:\n{text[-1500:]}"
            return (name, ok, log)
    except Exception as e:
        return (name, False, str(e))
    finally:
        # Signal and WAIT for the bridge threads before touching ser/sock --
        # closing a serial handle out from under a thread still blocked in
        # read()/write() on it can abort a USB transfer mid-flight (seen in
        # testing as a "Bad file descriptor" exception from the reader
        # thread), which risks desyncing the endpoint's DATA0/1 toggle state
        # in a way no amount of host-side retrying can recover from -- only
        # a real bus reset (unplug/replug) clears it. Threads poll
        # stop.is_set() at least every 0.2s (both ser and sock have a 0.2s
        # timeout), so a short join is always enough in the non-hung case.
        stop.set()
        for t in bridge_threads:
            t.join(timeout=1.0)
        if ser:
            ser.close()
        if sock:
            sock.close()
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except Exception:
                proc.kill()
        bridge_img = REPO_ROOT / "build" / "rp2350_hw_bridge_test_sd.img"
        bridge_img.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--console", help="Override auto-detected ACM0 console port")
    parser.add_argument("--net", help="Override auto-detected ACM1 net (link_usb_cdc) port")
    parser.add_argument("--uart", help="Override auto-detected CP2102 UART dongle port")
    parser.add_argument("--skip-qemu-bridge", action="store_true", help="Skip the T3 QEMU bridge test")
    args = parser.parse_args()

    print("======================================================================")
    print("            LugalOS RP2350 Hardware-in-the-Loop Test Suite")
    print("======================================================================")

    ports = rp2350.discover_ports(console=args.console, net=args.net, uart=args.uart)
    if ports is None:
        print("\n[!] No RP2350 detected (need two CDC-ACM ports: console + link_usb_cdc net).")
        print("    Nothing to test -- this is not a failure, just nothing to do.")
        return 0

    print(f"\nDetected RP2350: console={ports.console} net={ports.net} uart={ports.uart or '(none)'}")

    tests = [test_firmware_freshness, test_pmp_probe, test_umode_isolation, test_user_elf, test_usb_cdc_net_link, test_uart_demux_shared_wire]
    if not args.skip_qemu_bridge:
        tests.append(test_qemu_bridge)

    total = 0
    passed = 0
    for test in tests:
        name, ok, log = test(ports)
        total += 1
        if ok:
            passed += 1
            status = "SKIP" if log.startswith("SKIPPED") else "PASS"
            print(f"  [{status}] {name}" + (f" -- {log}" if log else ""))
        else:
            print(f"  [FAIL] {name}\n    {log}")

    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed} / {total} PASSED")
    print("======================================================================\n")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
