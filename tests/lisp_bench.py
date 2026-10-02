#!/usr/bin/env python3
"""Lisp Performance Benchmark Suite for LugalOS.

Runs standardized micro-benchmarks in QEMU or over hardware serial
to track performance improvements across Phase 42 milestones.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from runner import QemuSession  # type: ignore[import-untyped]

BENCHMARKS: list[tuple[str, str, str]] = [
    (
        "loop_10k",
        "Named-let loop 10,000 iterations",
        "(let ((t0 (time))) (let loop ((i 0)) (if (= i 10000) (- (time) t0) (loop (+ i 1)))))",
    ),
    (
        "special_forms_10k",
        "10k iterations of let* + begin + math",
        "(let ((t0 (time))) (let loop ((i 0) (sum 0)) (if (= i 10000) (- (time) t0) (let* ((a 1) (b 2)) (begin (loop (+ i 1) (+ sum a b)))))))",
    ),
    (
        "comparison_10k",
        "10k chained comparisons (< 1 2 3 4 5)",
        "(let ((t0 (time))) (let loop ((i 0)) (if (= i 10000) (- (time) t0) (if (< 1 2 3 4 5) (loop (+ i 1)) 0))))",
    ),
    (
        "list_cons_1k",
        "1,000 cons allocations and traversal",
        "(let ((t0 (time))) (let loop ((i 0) (acc '())) (if (= i 1000) (- (time) t0) (loop (+ i 1) (cons i acc)))))",
    ),
    (
        "fib_16",
        "Recursive fibonacci (fib 16) - 1,973 calls",
        "(begin (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) (let ((t0 (time))) (let ((res (fib 16))) (- (time) t0))))",
    ),
]


class HardwareSession:
    """Minimal hardware interactive session over serial port."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        import serial

        self.ser = serial.Serial(port, baud, timeout=1.0)
        self.ser.dtr = True
        time.sleep(0.2)

    def start(self) -> None:
        self.ser.reset_input_buffer()
        self.ser.write(b"\r\n")
        self.ser.flush()
        time.sleep(0.2)

    def send_and_expect(
        self, command: str, expected_pattern: str, timeout: float = 15.0
    ) -> tuple[bool, str]:
        self.ser.reset_input_buffer()
        self.ser.write((command + "\r\n").encode("utf-8"))
        self.ser.flush()

        out = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1).decode(
                "utf-8", errors="replace"
            )
            if chunk:
                out += chunk
                if re.search(expected_pattern, out):
                    return True, out
            else:
                time.sleep(0.01)
        return False, out

    def close(self) -> None:
        self.ser.close()


def run_benchmarks(
    arch: str = "rv32",
    port: str | None = None,
    baseline: dict[str, Any] | None = None,
) -> dict[str, Any]:
    session: Any
    if port:
        print(f"==> Connecting to hardware serial port {port} for Lisp benchmarking...")
        session = HardwareSession(port=port)
    else:
        print(f"==> Launching LugalOS ({arch}) in QEMU for Lisp benchmarking...")
        img_path = REPO_ROOT / "build" / arch / "lugalos_sd.img"
        elf_path = REPO_ROOT / "build" / arch / "lugalos.elf"

        if not elf_path.exists():
            print(f"[!] Binary not found at {elf_path}. Please build it first.")
            sys.exit(1)

        session = QemuSession(elf_path=elf_path, img_path=img_path, arch=arch)

    session.start()

    # Wait for prompt / banner
    if not port:
        ok, _ = session.send_and_expect("", "LugalOS", timeout=5.0)
        if not ok:
            print("[!] Guest failed to boot to prompt.")
            session.close()
            sys.exit(1)

    # Warm-up evaluation
    session.send_and_expect("(+ 1 1)", "=> 2", timeout=3.0)

    results: dict[str, Any] = {
        "timestamp": time.time(),
        "arch": arch if not port else f"hw_{Path(port).name}",
        "scores": {},
    }

    print("=" * 82)
    header = f"{'Benchmark':<20} | {'Description':<35} | {'Time (ms)':<10} | {'Delta vs Baseline':<12}"
    print(header)
    print("-" * 82)

    for bench_id, desc, expr in BENCHMARKS:
        times: list[int] = []
        for _ in range(3):
            ok, res_text = session.send_and_expect(expr, "=>", timeout=20.0)
            if ok:
                match = re.search(r"=>\s+(-?\d+)", res_text)
                if match:
                    times.append(int(match.group(1)))
            time.sleep(0.05)

        best_time = min(times) if times else -1
        results["scores"][bench_id] = {
            "name": desc,
            "min_ms": best_time,
            "runs_ms": times,
        }

        delta_str = "-"
        if baseline and bench_id in baseline.get("scores", {}):
            base_ms = baseline["scores"][bench_id].get("min_ms", -1)
            if base_ms > 0 and best_time > 0:
                diff = best_time - base_ms
                pct = (diff / base_ms) * 100.0
                sign = "+" if diff > 0 else ""
                delta_str = f"{sign}{pct:.1f}% ({sign}{diff}ms)"

        print(f"{bench_id:<20} | {desc:<35} | {best_time:>6} ms   | {delta_str:<12}")

    print("=" * 82)
    session.close()
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description="LugalOS Lisp Benchmark Suite")
    parser.add_argument(
        "--arch",
        default="rv32",
        help="Target architecture preset (default: rv32)",
    )
    parser.add_argument(
        "--port",
        default=None,
        help="Serial port for hardware benchmarking (e.g. /dev/ttyACM0)",
    )
    parser.add_argument(
        "--record-baseline",
        action="store_true",
        help="Save these results as the baseline reference",
    )
    args = parser.parse_args()

    target_name = args.arch if not args.port else f"hw_{Path(args.port).name}"
    baseline_file = REPO_ROOT / "tests" / f"lisp_bench_baseline_{target_name}.json"
    results_file = REPO_ROOT / "tests" / f"lisp_bench_{target_name}.json"
    history_file = REPO_ROOT / "tests" / f"lisp_bench_history_{target_name}.json"

    baseline_data: dict[str, Any] | None = None
    if baseline_file.exists() and not args.record_baseline:
        try:
            with open(baseline_file, "r", encoding="utf-8") as f:
                baseline_data = json.load(f)
        except Exception:
            baseline_data = None

    results = run_benchmarks(
        arch=args.arch, port=args.port, baseline=baseline_data
    )

    if args.record_baseline or not baseline_file.exists():
        with open(baseline_file, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=2)
        print(f"[+] Saved baseline reference to {baseline_file}")

    history: list[dict[str, Any]] = []
    if history_file.exists():
        try:
            with open(history_file, "r", encoding="utf-8") as f:
                history = json.load(f)
        except Exception:
            history = []

    history.append(results)
    with open(history_file, "w", encoding="utf-8") as f:
        json.dump(history, f, indent=2)

    with open(results_file, "w", encoding="utf-8") as f:
        json.dump(results, f, indent=2)

    print(f"[+] Saved latest benchmark results to {results_file}")


if __name__ == "__main__":
    main()
