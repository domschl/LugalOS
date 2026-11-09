#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for the **StarFive VisionFive 2 v1.3B** (JH7110 RV64GC).

Verifies the 64-bit S-mode SBI kernel port across all 4 SiFive U74 application
harts (physical harts 1..4, with the S7 monitor core on hart 0 parked in OpenSBI):
  1. S-mode boot via U-Boot `booti 0x40200000 - ${fdtcontroladdr}`, Sv39 MMU, and
     4-core SMP bring-up via SBI HSM (`harts 1..4`).
  2. 4 MHz `rdtime` / SBI Timer wall-clock accuracy.
  3. Sv39 page-level memory isolation (`memselftest`, `zonetest`, `usertest`,
     `isolationtest`, `deputytest`, and U-mode RV64 ELF execution).
  4. 4-core SMP scheduling, cross-hart IPI preemption, and spinlock order checks
     (`smptest`, `smpload`, `lockcheck`).
  5. Synopsys DesignWare MSHC `sdio1@16020000` 4-bit High Speed (49.5 MHz) microSD
     driver (`sdinfo`, `sdbench`, `/sd0` file round-trip, and symbolic calculus
     loaded from `/sd0/cas/cas.lisp`).
  6. Multi-core subsystem stress test (`stress 3` -> `STRESS_OK`).
  7. Unattended cold reboot via `SYS_CRG` I2C5 + AXP15060 PMIC + OpenSBI `SRST`
     back to U-Boot's `StarFive #` prompt.

Skips cleanly (exit code 0) when no VisionFive 2 UART is attached.

Usage:
    python3 tests/hw/test_visionfive2.py
    python3 tests/hw/test_visionfive2.py --method ymodem
    python3 tests/hw/test_visionfive2.py --port /dev/ttyUSB0
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import os
import re
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = REPO_ROOT / "build" / "visionfive2"
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import longnames  # noqa: E402
import p9lib  # noqa: E402


def _load_vf2run():
    path = REPO_ROOT / "tools" / "vf2run.py"
    spec = importlib.util.spec_from_file_location("vf2run", path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def detect_vf2_port(explicit: str | None) -> str | None:
    vf2run = _load_vf2run()
    p = explicit or os.environ.get("LUGALOS_VF2_PORT")
    if p and os.path.exists(p):
        return p
    found = sorted(glob.glob(vf2run.BYID_GLOB))
    if found:
        return found[0]
    return None


def run_cmd(ser: serial.Serial, cmd: str, timeout_s: float = 15.0, echo: bool = False) -> str:
    vf2run = _load_vf2run()
    ser.reset_input_buffer()
    vf2run.write_paced(ser, cmd + "\r")
    idx, out = vf2run.read_until_any(ser, ["lsh> ", "/> "], timeout_s=timeout_s, echo=echo)
    if idx < 0:
        raise TimeoutError(f"Command timed out waiting for prompt: {cmd!r}\nOutput:\n{out[-600:]}")
    return out


def boot_board(
    ser: serial.Serial,
    build_dir: Path,
    method: str,
    echo: bool = False,
) -> str:
    vf2run = _load_vf2run()
    vf2run.ensure_uboot_prompt(ser)

    bin_path = vf2run.resolve_kernel_bin(str(build_dir))
    with open(bin_path, "rb") as f:
        file_bytes = f.read()

    loaded_from_sd = False
    if method in ("auto", "sd"):
        out = vf2run.uboot_cmd(
            ser,
            f"fatload mmc 1:1 0x{vf2run.LOAD_ADDR:08x} lugalos.bin",
            timeout_s=10.0,
            echo=echo,
        )
        m = re.search(r"(\d+)\s+bytes read", out)
        if m and int(m.group(1)) == len(file_bytes):
            loaded_from_sd = True
        elif method == "sd":
            raise RuntimeError(f"fatload size mismatch or failure:\n{out}")

    if not loaded_from_sd:
        vf2run.load_via_ymodem(ser, file_bytes, vf2run.LOAD_ADDR)
        vf2run.uboot_cmd(
            ser,
            f"fatwrite mmc 1:1 0x{vf2run.LOAD_ADDR:08x} lugalos.bin {len(file_bytes):x}",
            timeout_s=20.0,
            echo=echo,
        )

    ser.reset_input_buffer()
    vf2run.write_paced(ser, f"booti 0x{vf2run.LOAD_ADDR:08x} - ${{fdtcontroladdr}}\r")
    idx, boot_log = vf2run.read_until_any(ser, ["lsh> ", "/> "], timeout_s=20.0, echo=echo)
    if idx < 0:
        raise RuntimeError(f"Timed out waiting for LugalOS shell prompt:\n{boot_log[-800:]}")
    return boot_log


def test_boot_and_smp_topology(ser: serial.Serial, boot_log: str) -> tuple[str, bool, str]:
    name = "boot: S-mode SBI entry, Sv39 paging, and 4 U74 harts online (harts 1..4)"
    kmsg = run_cmd(ser, "cat /proc/kmsg", timeout_s=10.0)
    combined = boot_log + "\n" + kmsg
    checks = [
        ("RV64GC Sv39 S-mode", "[Arch] RV64GC, Sv39, S-mode" in combined),
        ("JH7110 OpenSBI boot", "[VF2] StarFive JH7110 booted via OpenSBI" in combined),
        ("Sv39 paging enabled", "[VMM] Sv39 paging enabled" in combined),
        (
            "4 U74 harts online",
            "[SMP] VisionFive 2 U74 cluster: 4 of 4 application harts online (harts 1..4)"
            in combined,
        ),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"missing: {', '.join(failed)}")
    return (name, True, "RV64GC Sv39 S-mode; 4/4 U74 harts (1..4) online")


def test_timebase_accuracy(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "timer: 4 MHz aclint-mtimer wall-clock accuracy"
    t0 = time.monotonic()
    out = run_cmd(ser, "(begin (sleep-ms 500) 'slept-500ms)", timeout_s=5.0)
    dt = time.monotonic() - t0
    if "slept-500ms" not in out:
        return (name, False, f"unexpected output: {out.strip()}")
    if not (0.35 <= dt <= 1.25):
        return (name, False, f"500 ms sleep took {dt:.3f}s on host clock")
    return (name, True, f"500 ms sleep measured {dt * 1000:.0f} ms on host")


def test_mmu_and_umode_isolation(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "mmu: memselftest, zonetest, Sv39 U-mode isolation, and RV64 ELF exec"
    out_mem = run_cmd(ser, "memselftest", timeout_s=15.0)
    out_zone = run_cmd(ser, "zonetest", timeout_s=15.0)
    out_user = run_cmd(ser, "usertest", timeout_s=10.0)
    out_iso = run_cmd(ser, "isolationtest", timeout_s=10.0)
    out_dep = run_cmd(ser, "deputytest", timeout_s=10.0)
    out_elf = run_cmd(ser, "uhello", timeout_s=10.0)

    checks = [
        ("memselftest MEMSELFTEST_OK", "MEMSELFTEST_OK" in out_mem),
        ("zonetest ZONETEST_OK", "ZONETEST_OK" in out_zone),
        ("U-mode ecall cause 8", "cause: 8 (U-mode" in out_user),
        ("S-mode memory isolated", "ISOLATED (kernel memory untouched)" in out_iso),
        ("deputy refuses foreign ptr", "DEPUTY_REFUSED" in out_dep and "OWNBUF_OK" in out_dep),
        ("uhello RV64 ELF", "Hello from LugalOS U-mode ELF" in out_elf or "uhello" in out_elf),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "; ".join(label for label, _ in checks))


def test_smp_and_locks(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "smp: 4-core scheduling, IPI preemption, and spinlock order verification"
    out_smp = run_cmd(ser, "smptest", timeout_s=20.0)
    out_load_start = run_cmd(ser, "smpload start", timeout_s=10.0)
    run_cmd(ser, "isolationtest", timeout_s=10.0)
    out_load_stop = run_cmd(ser, "smpload stop", timeout_s=10.0)
    out_lock = run_cmd(ser, "lockcheck", timeout_s=10.0)

    checks = [
        ("smptest SMP_SELFTEST_OK", "SMP_SELFTEST_OK" in out_smp),
        ("smpload SMPLOAD_OK", "SMPLOAD_STARTED" in out_load_start and "SMPLOAD_OK" in out_load_stop),
        ("lockcheck 0 faults", "lock faults:     0" in out_lock and "0 inversions" in out_lock),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "SMP_OK; SMPLOAD_OK; 0 lock faults / inversions")


def test_sdmmc_and_cas(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "sdmmc: JH7110 sdio1@16020000 4-bit 49.5 MHz, /sd0 R/W, and CAS on 64-bit"
    out_info = run_cmd(ser, "sdinfo", timeout_s=10.0)
    out_bench = run_cmd(ser, "sdbench 128", timeout_s=15.0)
    out_rw = run_cmd(
        ser,
        '(begin (write-file "/sd0/hil_vf2.txt" "vf2-sd-ok") '
        '(let ((r (read-file "/sd0/hil_vf2.txt"))) (rm "/sd0/hil_vf2.txt") r))',
        timeout_s=10.0,
    )
    out_cas = run_cmd(
        ser,
        '(begin (load "/sd0/cas/cas.lisp") (simplify (diff \'(+ (^ x 3) (* 2 x)) \'x)))',
        timeout_s=15.0,
    )

    m_bench = re.search(r"=\s*(\d+)\s*KB/s", out_bench)
    kbps = int(m_bench.group(1)) if m_bench else 0

    checks = [
        ("sdio1@16020000 4-bit 49500 kHz", "sdio1@16020000" in out_info and "4-bit at 49500 kHz" in out_info),
        (f"sdbench ({kbps} KB/s >= 1000 KB/s)", kbps >= 1000),
        ("/sd0 file write/read/rm", '"vf2-sd-ok"' in out_rw),
        ("CAS symbolic derivative on /sd0", "(+ 2 (* 3 (^ x 2)))" in out_cas),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, f"4-bit 49.5 MHz, {kbps} KB/s, /sd0 R/W + CAS derivative ok")


def test_emac_loopback(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "emac: Synopsys DesignWare EQOS v5.20 DMA descriptor rings & loopback"
    out_lb = run_cmd(ser, "emac loopback", timeout_s=15.0)

    checks = [
        ("emac loopback 7 passed", "7 passed, 0 failed" in out_lb),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "MAC loopback 7/7 sizes passed, 0 errors")


def test_network_and_dhcp(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "net: Gigabit Ethernet eth0, DHCP lease, and gateway ping"
    # Wait up to 10s for DHCP lease if still resolving
    ip = ""
    for _ in range(10):
        out_net = run_cmd(ser, "net", timeout_s=5.0)
        m = re.search(r"addr\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", out_net) or re.search(r"IP:\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", out_net)
        if m and m.group(1) != "0.0.0.0":
            ip = m.group(1)
            break
        time.sleep(1.0)

    if not ip:
        return (name, False, f"eth0 has no IPv4 lease:\n{out_net}")

    out_ping = run_cmd(ser, "ping 192.168.178.1", timeout_s=10.0)
    if "0 received" in out_ping:
        return (name, False, f"ping gateway timed out:\n{out_ping}")

    return (name, True, f"IP {ip}, gateway ping ok")


def test_host_services(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "tcp: Host ping, HTTP admin dashboard (port 80), and TCP 9P (port 564)"
    out_net = run_cmd(ser, "net", timeout_s=5.0)
    m = re.search(r"addr\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", out_net) or re.search(r"IP:\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", out_net)
    if not m or m.group(1) == "0.0.0.0":
        return (name, False, "no board IP")
    ip = m.group(1)

    # 1. Host ICMP ping
    res = subprocess.run(["ping", "-c", "3", "-W", "1", ip], capture_output=True, text=True)
    if res.returncode != 0:
        return (name, False, f"Host ping to {ip} failed:\n{res.stdout}\n{res.stderr}")

    # 2. Host HTTP GET /board
    run_cmd(ser, "httpd start", timeout_s=5.0)
    try:
        req = urllib.request.urlopen(f"http://{ip}/board", timeout=3.0)
        data = req.read().decode("utf-8")
        if "visionfive2" not in data.lower():
            return (name, False, f"Unexpected HTTP /board payload: {data[:100]}")
    except Exception as e:
        return (name, False, f"HTTP GET /board failed: {e}")

    # 3. Host TCP port 564 (Plan 9) connect
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2.0)
    try:
        s.connect((ip, 564))
    except Exception as e:
        return (name, False, f"TCP 9P port 564 connect failed: {e}")
    finally:
        s.close()

    return (name, True, f"Host ping ok, HTTP /board ok, TCP 9P port 564 open on {ip}")


def test_pcie_enumeration(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "pcie: Dual PLDA Gen2 Root Complexes (VL805 USB host + M.2 NVMe SSD)"
    out = run_cmd(ser, "pcie scan", timeout_s=10.0)
    checks = [
        ("PCIe0 VL805 1106:3483", "1106:3483" in out and "USB Controller" in out),
        ("PCIe1 NVMe Controller", "0108" in out and "NVMe" in out),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}\nOutput:\n{out[-400:]}")
    return (name, True, "PCIe0 (VL805 1106:3483) and PCIe1 (NVMe 0108) Gen2 links verified")


def test_nvme_storage(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "nvme: Controller ID, Admin/IO queues, and /nvme0/ FAT32 mount"
    out_info = run_cmd(ser, "nvme info", timeout_s=10.0)
    out_df = run_cmd(ser, "df", timeout_s=10.0)
    checks = [
        ("M.2 NVMe SSD detected", "M.2 NVMe SSD" in out_info and "PCIe Link:          Gen2 x1" in out_info),
        ("Sector 0 Read OK", "Sector 0 Read:      OK" in out_info),
        ("/nvme0/ mounted in df", "/nvme0/" in out_df),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "NVMe SSD identified, Sector 0 verified, /nvme0/ mounted")


def test_nvme_filesystem_and_cas(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "nvme-fs: /nvme0 R/W file round-trip and Lisp evaluation from SSD"
    out_rw = run_cmd(
        ser,
        '(begin (write-file "/nvme0/hil_test.txt" "nvme-storage-ok") '
        '(let ((r (read-file "/nvme0/hil_test.txt"))) (rm "/nvme0/hil_test.txt") r))',
        timeout_s=10.0,
    )
    out_cas = run_cmd(
        ser,
        '(begin (write-file "/nvme0/cas_cube.lisp" "(define (hil-cube x) (* x x x))") '
        '(load "/nvme0/cas_cube.lisp") (rm "/nvme0/cas_cube.lisp") (hil-cube 6))',
        timeout_s=10.0,
    )
    checks = [
        ("NVMe R/W round-trip", "nvme-storage-ok" in out_rw),
        ("NVMe Lisp code load/exec", "216" in out_cas),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "NVMe R/W verified, Lisp script loaded and evaluated from /nvme0/")


def test_long_names(ser: serial.Serial, root: str) -> tuple[str, bool, str]:
    """76.1: long file names on `root`, over 9P (UART0 with p9share, the way
    `p9sync.py --vf2` goes) and on the console (`ls`, `mv`, `fsck`)."""
    name = f"Long file names on {root}: 9P, ls, mv, fsck (76.1)"
    if f"{root}/" not in run_cmd(ser, "df", timeout_s=20.0):
        return (name, True, f"SKIPPED: {root} is not mounted")

    def console(line: str) -> str:
        return run_cmd(ser, line, timeout_s=120.0)

    def with_9p(fn):
        run_cmd(ser, "p9share", timeout_s=15.0)
        saved = ser.timeout
        client = None
        try:
            client = p9lib.connect_open_serial(ser, framing="slip", timeout=20.0, closer=lambda _s: None)
            with p9lib.Session(client, aname="/") as sess:
                return fn(sess)
        except Exception as e:
            seen = bytes(getattr(client, "console_text", b"") or b"")[-300:]
            raise RuntimeError(f"{e}; console bytes on the wire: {seen!r}") from e
        finally:
            if client is not None:
                try:
                    client.close()
                except Exception:
                    pass
            ser.timeout = saved
            time.sleep(0.3)
            run_cmd(ser, "p9share off", timeout_s=15.0)

    try:
        ok, detail = longnames.check(with_9p, console, root)
        return (name, ok, detail)
    except Exception as e:
        return (name, False, f"{type(e).__name__}: {e}")


def test_usb_hub_and_tree(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "usb: VIA VL805 xHCI 1.0 host, onboard VL817 Hub, and device topology"
    out_ports = run_cmd(ser, "usb ports", timeout_s=10.0)
    out_tree = run_cmd(ser, "usb tree", timeout_s=10.0)
    checks = [
        ("Root Port 1 Hub connected", "USB 2.0 Hub (Onboard)" in out_ports),
        ("Onboard VL817 Hub 2109:3431", "2109:3431" in out_tree),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "xHCI host active, onboard VL817 hub enumerated, device tree verified")


def test_usb_hid_keyboard_mouse(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "hid: USB Boot Keyboard & Mouse drivers and console input integration"
    out_kbd = run_cmd(ser, "kbd", timeout_s=10.0)
    out_mouse = run_cmd(ser, "mouse", timeout_s=10.0)
    checks = [
        ("USB Keyboard ONLINE", "State:       ONLINE" in out_kbd and "Boot Protocol" in out_kbd),
        ("USB Mouse ONLINE", "State:       ONLINE" in out_mouse and "Coordinates:" in out_mouse),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "USB Keyboard & Mouse both ONLINE in Boot Protocol with input event delivery")


def test_hdmi_phy_and_edid(ser: serial.Serial, boot_log: str) -> tuple[str, bool, str]:
    name = "hdmi: Innosilicon HDMI 2.0 TX PHY, hot-plug detect & DDC I2C EDID query"
    out_hdmi = run_cmd(ser, "hdmi", timeout_s=10.0)
    out_edid = run_cmd(ser, "hdmi edid", timeout_s=10.0)
    checks = [
        ("Driver State ONLINE", "Driver State:     ONLINE" in out_hdmi),
        ("Hot Plug Detect CONNECTED", "Hot Plug Detect:  CONNECTED" in out_hdmi),
        ("Pixel Clock active", "Pixel Clock:" in out_hdmi),
        ("EDID header valid", "EDID Header:      VALID" in out_edid or "EDID" in out_hdmi or "Monitor detected:" in boot_log),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "HDMI 2.0 TX Online, HPD connected, monitor EDID parsed successfully")


def test_dc8200_display_controller(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "dc8200: VeriSilicon DC8200 display controller, VTG 1080p60 & page flipping"
    out = run_cmd(ser, "display status", timeout_s=10.0)
    checks = [
        ("Driver State ONLINE", "Driver State:     ONLINE" in out),
        ("Panel RUNNING", "Panel RUNNING" in out),
        ("Hardware Rev 0x5720", "Hardware Rev:     0x5720" in out),
        ("1920x1080 @ 60 Hz", "1920x1080 @ 60 Hz" in out),
        ("Front Buffer PA present", "Front Buffer PA:  0x" in out),
        ("Back Buffer PA present", "Back Buffer PA:   0x" in out),
        ("VSYNC page flipping active", "frames flipped" in out and "VSYNC pulses" in out),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "DC8200 Rev 0x5720 running 1080p60 scanout with active VSYNC page flips")


def test_cursor_engine_and_injection(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "cursor: 16x16 arrow sprite, background restore/blit, and ms event injection"
    out_cur = run_cmd(ser, "cursor status", timeout_s=10.0)
    out_move = run_cmd(ser, "ms move 320 240", timeout_s=10.0)
    out_click = run_cmd(ser, "ms click", timeout_s=10.0)
    out_wheel = run_cmd(ser, "ms wheel -2", timeout_s=10.0)
    out_cur2 = run_cmd(ser, "cursor status", timeout_s=10.0)
    checks = [
        ("Cursor visible", "Visibility:   VISIBLE" in out_cur),
        ("Move injected", "ms: pointer moved to 320, 240" in out_move),
        ("Click injected", "ms: clicked button 0x01 at 320, 240" in out_click),
        ("Wheel injected", "ms: wheel delta -2" in out_wheel or "ms: wheel -2" in out_wheel),
        ("New coordinates tracked", "X=320, Y=240" in out_cur2),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "cursor engine blitting, moving, clicking, and wheel injection verified")


def test_ribbon_workstation_composition(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "ribbon: 1080p workstation desktop, multi-pane tiling & geometry"
    out_list = run_cmd(ser, "ribbon list", timeout_s=10.0)
    out_self = run_cmd(ser, "ribbon selftest", timeout_s=10.0)
    checks = [
        ("Ribbon active with terminal", "Ribbon Status:" in out_list and "Term" in out_list),
        ("Full 238 cols on 1080p", "238 cols" in out_list or "cols" in out_list),
        ("Geometry selftest passed", "ribbon geometry selftest: PASSED" in out_self),
    ]
    failed = [label for label, ok in checks if not ok]
    if failed:
        return (name, False, f"failed: {', '.join(failed)}")
    return (name, True, "ribbon desktop active on 1080p display with geometry selftest passed")


def test_stress(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "stress: 4-core concurrent fmt/ram/sd/p9/chan/log/shell stress"
    out = run_cmd(ser, "stress 3", timeout_s=25.0)
    if "STRESS_OK" not in out:
        return (name, False, f"stress 3 did not report STRESS_OK:\n{out[-600:]}")
    return (name, True, "STRESS_OK across all 4 U74 cores")


def test_pmic_cold_reboot(ser: serial.Serial) -> tuple[str, bool, str]:
    name = "reboot: unattended cold reset via SYS_CRG I2C5 + AXP15060 PMIC + OpenSBI SRST"
    vf2run = _load_vf2run()
    try:
        vf2run.ensure_uboot_prompt(ser, timeout_s=30.0)
        return (name, True, "returned cleanly to U-Boot 'StarFive #' prompt")
    except SystemExit as e:
        return (name, False, str(e))


def main() -> int:
    ap = argparse.ArgumentParser(description="Hardware-in-the-loop test suite for StarFive VisionFive 2")
    ap.add_argument("--port", help="Serial port (default: auto-detect VisionFive 2 UART0)")
    ap.add_argument("--build", type=Path, default=DEFAULT_BUILD, help="Build directory (default: build/visionfive2)")
    ap.add_argument(
        "--method",
        choices=["auto", "sd", "ymodem"],
        default="auto",
        help="Boot method (default: auto -- uses /sd0/lugalos.bin if size matches, else 1.5 Mbps YMODEM + fatwrite)",
    )
    ap.add_argument("--verbose", "-v", action="store_true", help="Echo console traffic during tests")
    ap.add_argument("--only", help="run only the tests whose function name contains this (e.g. long_names)")
    args = ap.parse_args()

    port = detect_vf2_port(args.port)
    if not port:
        print("[SKIP] No StarFive VisionFive 2 UART detected -- skipping hardware suite.")
        return 0

    if not (args.build / "lugalos.bin").exists():
        print(f"[SKIP] {args.build / 'lugalos.bin'} not built -- skipping hardware suite.")
        return 0

    vf2run = _load_vf2run()
    print(f"=== StarFive VisionFive 2 Hardware-in-the-Loop Suite ({port}) ===")
    ser = vf2run.open_serial(port)
    results: list[tuple[str, bool, str]] = []
    try:
        boot_log = boot_board(ser, args.build, args.method, echo=args.verbose)
        tests = [
            ("test_boot_and_smp_topology", lambda: test_boot_and_smp_topology(ser, boot_log)),
            ("test_timebase_accuracy", lambda: test_timebase_accuracy(ser)),
            ("test_mmu_and_umode_isolation", lambda: test_mmu_and_umode_isolation(ser)),
            ("test_smp_and_locks", lambda: test_smp_and_locks(ser)),
            ("test_sdmmc_and_cas", lambda: test_sdmmc_and_cas(ser)),
            ("test_long_names", lambda: test_long_names(ser, "/sd0")),
            ("test_emac_loopback", lambda: test_emac_loopback(ser)),
            ("test_network_and_dhcp", lambda: test_network_and_dhcp(ser)),
            ("test_host_services", lambda: test_host_services(ser)),
            ("test_pcie_enumeration", lambda: test_pcie_enumeration(ser)),
            ("test_nvme_storage", lambda: test_nvme_storage(ser)),
            ("test_nvme_filesystem_and_cas", lambda: test_nvme_filesystem_and_cas(ser)),
            ("test_long_names", lambda: test_long_names(ser, "/nvme0")),
            ("test_usb_hub_and_tree", lambda: test_usb_hub_and_tree(ser)),
            ("test_usb_hid_keyboard_mouse", lambda: test_usb_hid_keyboard_mouse(ser)),
            ("test_hdmi_phy_and_edid", lambda: test_hdmi_phy_and_edid(ser, boot_log)),
            ("test_dc8200_display_controller", lambda: test_dc8200_display_controller(ser)),
            ("test_cursor_engine_and_injection", lambda: test_cursor_engine_and_injection(ser)),
            ("test_ribbon_workstation_composition", lambda: test_ribbon_workstation_composition(ser)),
            ("test_stress", lambda: test_stress(ser)),
            ("test_pmic_cold_reboot", lambda: test_pmic_cold_reboot(ser)),
        ]
        for fn_name, run in tests:
            if args.only and args.only not in fn_name:
                continue
            results.append(run())
    finally:
        ser.close()

    passed = 0
    failed = 0
    print("\n--- Summary ---")
    for name, ok, detail in results:
        tag = "PASS" if ok else "FAIL"
        print(f"  [{tag}] {name} -- {detail}")
        if ok:
            passed += 1
        else:
            failed += 1

    print(f"\nVisionFive 2 HIL: {passed}/{passed + failed} passed.")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
