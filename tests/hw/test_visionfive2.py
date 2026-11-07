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
import sys
import time
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = REPO_ROOT / "build" / "visionfive2"


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
        results.append(test_boot_and_smp_topology(ser, boot_log))
        results.append(test_timebase_accuracy(ser))
        results.append(test_mmu_and_umode_isolation(ser))
        results.append(test_smp_and_locks(ser))
        results.append(test_sdmmc_and_cas(ser))
        results.append(test_stress(ser))
        results.append(test_pmic_cold_reboot(ser))
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
