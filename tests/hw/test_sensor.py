#!/usr/bin/env python3
"""Hardware-in-the-loop environmental and gas sensor test suite.
Phase 46: Environmental & Gas Sensor Framework.

Verifies attached I2C sensors (BME280/680, CCS811, SGP30, TSL2561/2591, MiCS-6814)
on physical silicon (e.g. RP2350 Pico 2 W, ESP32-P4).

Covers:
  1. Arithmetic selftests across all drivers (`sensor selftest`)
  2. Live multi-sensor sampling and hub aggregation (`sensor`)
  3. Continuous sampling cache exposed via `/proc/sensors`
  4. Lisp sensor inspection and query primitives:
     - `(sensor-list)`
     - `(sensor-read)`
     - `(sensor-read <chan>)`
     - `(sensor-read <dev> <chan>)`
"""

from __future__ import annotations

import glob
import os
import re
import sys
import time
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tests" / "hw"))
from rp2350 import drain, ports_by_usb_descriptor  # noqa: E402


def find_console_port() -> str | None:
    ports = ports_by_usb_descriptor()
    if ports and ports.console:
        return ports.console

    # Fallback to serial-by-id
    for p in glob.glob("/dev/serial/by-id/usb-LugalOS_*_Dual_CDC_ACM_*-if00"):
        return os.path.realpath(p)

    return None


def run_cmd(ser: serial.Serial, cmd: str, timeout: float = 3.0) -> str:
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("utf-8"))
    ser.flush()
    q = min(0.4, timeout / 2.0)
    raw = drain(ser, quiet=q, deadline=max(timeout, 1.5))
    return raw.decode("utf-8", errors="replace")


def main() -> int:
    sys.stdout.reconfigure(line_buffering=True)
    port = find_console_port()
    if not port or not os.path.exists(port):
        print("[!] No LugalOS console port found. Skipping sensor test.")
        return 0

    print(f"Connecting to console at {port}...")
    try:
        ser = serial.Serial(port, 115200, timeout=1.0)
        ser.dtr = True
        time.sleep(0.3)
    except Exception as e:
        print(f"[!] Cannot open {port}: {e}")
        return 0

    try:
        # Clear any pending output
        run_cmd(ser, "", 0.5)

        print("\n--- 1. Sensor Selftest ---")
        out = run_cmd(ser, "sensor selftest", 3.0)
        print(out.strip())
        assert "mics6814 selftest: 0 cases failed" in out, "MiCS-6814 selftest failed"
        assert "mhz19b selftest: 0 cases failed" in out, "MH-Z19B selftest failed"
        assert "hdc1080 selftest: 0 cases failed" in out, "HDC1080 selftest failed"
        assert "sensor_derived selftest: 0 cases failed" in out, "Derived metrics selftest failed"
        assert "bme680 selftest: 0 cases failed" in out or "bme280 selftest: 0 cases failed" in out
        assert "ccs811 selftest: 0 cases failed" in out
        assert "sgp30 selftest: 0 cases failed" in out
        print("  [PASS] All driver selftests passed with 0 errors.")

        print("\n--- 2. Sensor Live Sampling ---")
        out = run_cmd(ser, "sensor", 3.0)
        print(out.strip())
        assert "mics6814 at 0x04:" in out, "mics6814 not active in sensor hub"
        has_hdc1080 = "hdc1080 at 0x40:" in out
        if has_hdc1080:
            print("  [PASS] hdc1080 detected and reporting.")
        has_mhz19b = "mhz19b at" in out
        if has_mhz19b:
            print("  [PASS] mhz19b detected and reporting.")
        else:
            print("  [INFO] mhz19b not detected.")
        print("  [PASS] Sensor hub probed and reported active sensors.")

        print("\n--- 3. /proc/sensors Cache Inspection ---")
        out = run_cmd(ser, "cat /proc/sensors", 1.0)
        print(out.strip())
        assert "valid=yes" in out
        assert "co_c_ppm=" in out and "no2_c_ppm=" in out and "nh3_c_ppm=" in out
        assert "abs_humidity_c100=" in out and "dew_point_c100=" in out, "derived metrics missing from /proc/sensors"
        if has_hdc1080:
            assert "hdc1080_temperature_c100=" in out or "temperature_c100=" in out
        if "part=bme680" in out:
            assert "iaq=" in out, "BME680 IAQ missing from /proc/sensors"
        if has_mhz19b:
            assert "co2_ppm=" in out, "co2_ppm missing from /proc/sensors"
            assert "mox_contaminated=" in out, "mox_contaminated flag missing from /proc/sensors"
        print("  [PASS] /proc/sensors exposes multichannel gas metrics and derived channels.")

        print("\n--- 3b. /proc/sensor/ Virtual Subdirectory Inspection ---")
        out = run_cmd(ser, "ls /proc/sensor", 1.0)
        print(out.strip())
        assert "fused" in out and "inferred" in out, "subdirectories missing from /proc/sensor"
        out = run_cmd(ser, "cat /proc/sensor/fused", 1.0)
        print(out.strip())
        assert "tier=fused" in out, "tier=fused missing from /proc/sensor/fused"
        out = run_cmd(ser, "cat /proc/sensor/inferred", 1.0)
        print(out.strip())
        assert "tier=inferred" in out, "tier=inferred missing from /proc/sensor/inferred"
        print("  [PASS] /proc/sensor/ directory and per-node files verified.")

        print("\n--- 4. Lisp Sensor Primitives ---")
        out = run_cmd(ser, "(sensor-list)", 0.5)
        print(f"(sensor-list) -> {out.strip()}")
        assert "mics6814" in out, "mics6814 missing from (sensor-list)"
        if has_hdc1080:
            assert "hdc1080" in out, "hdc1080 missing from (sensor-list)"
        if has_mhz19b:
            assert "mhz19b" in out, "mhz19b missing from (sensor-list)"

        out = run_cmd(ser, "(sensor-read)", 0.5)
        print(f"(sensor-read) -> {out.strip()}")
        assert "hw" in out and "inferred" in out and "fused" in out, "Tiered hierarchy missing from (sensor-read)"
        assert "co ." in out and "no2 ." in out and "nh3 ." in out
        if has_mhz19b:
            assert "co2 ." in out
        assert "abs-humidity ." in out and "dew-point ." in out

        out = run_cmd(ser, "(sensor-read 'hw)", 0.5)
        print(f"(sensor-read 'hw) -> {out.strip()}")
        assert "mics6814" in out

        out = run_cmd(ser, "(sensor-read 'inferred)", 0.5)
        print(f"(sensor-read 'inferred) -> {out.strip()}")
        assert "dew-point ." in out and "abs-humidity ." in out

        out = run_cmd(ser, "(sensor-read 'fused)", 0.5)
        print(f"(sensor-read 'fused) -> {out.strip()}")
        assert "co ." in out or "temp ." in out

        out = run_cmd(ser, "(sensor-origin 'dew-point)", 0.5)
        print(f"(sensor-origin 'dew-point) -> {out.strip()}")
        assert "tier . inferred" in out and "magnus-tetens" in out

        out = run_cmd(ser, "(sensor-origin 'co2)", 0.5)
        print(f"(sensor-origin 'co2) -> {out.strip()}")
        assert "tier . fused" in out

        out = run_cmd(ser, "(sensor-read 'mics6814 'co)", 0.5)
        print(f"(sensor-read 'mics6814 'co) -> {out.strip()}")
        m = re.search(r"=>\s+(\d+)", out)
        assert m and int(m.group(1)) > 0, "Failed to read CO from mics6814"

        if has_hdc1080:
            out = run_cmd(ser, "(sensor-read 'hdc1080 'temp)", 0.5)
            print(f"(sensor-read 'hdc1080 'temp) -> {out.strip()}")
            m = re.search(r"=>\s+(\d+)", out)
            assert m and int(m.group(1)) > 0, "Failed to read temp from hdc1080"

            out = run_cmd(ser, "(sensor-read 'hdc1080 'humidity)", 0.5)
            print(f"(sensor-read 'hdc1080 'humidity) -> {out.strip()}")
            m = re.search(r"=>\s+(\d+)", out)
            assert m and int(m.group(1)) > 0, "Failed to read humidity from hdc1080"

        # A CCS811 that was just (re)started has no result at the hub's first
        # sample -- the driver keeps no value rather than a 0 -- so its first
        # eCO2 can take one sampler period (60 s) to appear.
        deadline = time.time() + 75.0
        while True:
            out = run_cmd(ser, "(sensor-read 'ccs811 'eco2)", 0.5)
            m = re.search(r"=>\s+(\d+)", out)
            if (m and int(m.group(1)) >= 400) or time.time() > deadline:
                break
            time.sleep(5.0)
        print(f"(sensor-read 'ccs811 'eco2) -> {out.strip()}")
        assert m and int(m.group(1)) >= 400, "Failed to read eCO2 from ccs811"

        out = run_cmd(ser, "(sensor-read 'sgp30 'eco2)", 0.5)
        print(f"(sensor-read 'sgp30 'eco2) -> {out.strip()}")
        m = re.search(r"=>\s+(\d+)", out)
        assert m and int(m.group(1)) >= 400, "Failed to read eCO2 from sgp30"

        out = run_cmd(ser, "(sensor-read 'abs-humidity)", 0.5)
        print(f"(sensor-read 'abs-humidity) -> {out.strip()}")
        m = re.search(r"=>\s+(\d+)", out)
        assert m and int(m.group(1)) > 0, "Failed to read abs-humidity"

        out = run_cmd(ser, "(sensor-read 'dew-point)", 0.5)
        print(f"(sensor-read 'dew-point) -> {out.strip()}")
        m = re.search(r"=>\s+(-?\d+)", out)
        assert m, "Failed to read dew-point"

        if has_mhz19b:
            out = run_cmd(ser, "(sensor-read 'mhz19b 'co2)", 0.5)
            print(f"(sensor-read 'mhz19b 'co2) -> {out.strip()}")
            m = re.search(r"=>\s+(\d+)", out)
            assert m and int(m.group(1)) >= 300, "Failed to read CO2 from mhz19b"

            out = run_cmd(ser, "(sensor-read 'co2)", 0.5)
            print(f"(sensor-read 'co2) -> {out.strip()}")
            m = re.search(r"=>\s+(\d+)", out)
            assert m and int(m.group(1)) >= 300, "Failed to read default 'co2"

        print("  [PASS] Lisp sensor query primitives operational.")

        print("\n--- 5. Sensor Calibration & Persistence ---")
        out = run_cmd(ser, "sensor cal", 1.0)
        print(out.strip())
        assert "Active Sensor Baselines:" in out
        assert "SGP30:" in out and "CCS811:" in out and "MiCS-6814 R0:" in out

        out = run_cmd(ser, "sensor cal save", 2.0)
        print(out.strip())
        assert "saved to persistent identity store" in out

        out = run_cmd(ser, "sensor cal restore", 1.0)
        print(out.strip())
        assert "restored from persistent identity store" in out

        out = run_cmd(ser, "(sensor-cal)", 0.5)
        print(f"(sensor-cal) -> {out.strip()}")
        assert "saved? . #t" in out
        assert "sgp30-eco2 ." in out and "ccs811 ." in out

        out = run_cmd(ser, "(sensor-cal 'save)", 1.0)
        print(f"(sensor-cal 'save) -> {out.strip()}")
        assert "=> #t" in out

        out = run_cmd(ser, "(sensor-cal 'restore)", 1.0)
        print(f"(sensor-cal 'restore) -> {out.strip()}")
        assert "=> #t" in out

        # EEPROM testing if detected
        out = run_cmd(ser, "sensor cal", 1.0)
        if "AT24C32 EEPROM" in out:
            print("\n--- 6. AT24C32 EEPROM Fast Calibration Storage ---")
            out = run_cmd(ser, "sensor cal eeprom save", 1.5)
            print(out.strip())
            assert "saved to AT24C32 EEPROM" in out

            out = run_cmd(ser, "sensor cal eeprom restore", 1.0)
            print(out.strip())
            assert "restored from AT24C32 EEPROM" in out

            out = run_cmd(ser, "(sensor-cal 'save-eeprom)", 1.0)
            print(f"(sensor-cal 'save-eeprom) -> {out.strip()}")
            assert "=> #t" in out

            out = run_cmd(ser, "(sensor-cal 'restore-eeprom)", 1.0)
            print(f"(sensor-cal 'restore-eeprom) -> {out.strip()}")
            assert "=> #t" in out

            out = run_cmd(ser, "(sensor-cal)", 0.5)
            print(f"(sensor-cal) -> {out.strip()}")
            assert "eeprom-saved? . #t" in out
            print("  [PASS] AT24C32 EEPROM calibration operations verified.")

        print("\n--- 7. Per-Sensor Calibration Reset ---")
        out = run_cmd(ser, "sensor cal reset ccs811", 1.5)
        print(out.strip())
        assert "baseline reset/cleared for 'ccs811'" in out
        out = run_cmd(ser, "sensor cal", 1.0)
        print(out.strip())
        assert "CCS811: 0x0000" in out
        print("  [PASS] Single-sensor baseline reset verified.")

        print("  [PASS] Multi-sensor calibration model and flash persistence verified.")

        print("\n=======================================================")
        print(" All sensor hardware integration tests PASSED!")
        print("=======================================================")
        return 0

    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
