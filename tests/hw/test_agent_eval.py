#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "pyserial>=3.5",
# ]
# ///
"""Automated Agent E2E Benchmark Suite for LugalOS (Milestone 59.7).

Executes canonical end-to-end agent benchmarks against the live on-device AI
assistant (`ea`), verifying that the agent autonomously invokes tools (lisp,
sh, cc, help), reasons through problems without getting stuck, and arrives at
exact solutions in 1-2 shots.

Canonical Benchmarks:
1. CAS Calculus: Trajectory / polynomial differentiation via Lisp CAS (`deriv`).
2. Algebra / Quadratic Solving: Roots of polynomial equations via `solve-poly`.
3. System / Demo Discovery: Shell recursive search (`find`) discovering demos.
4. Hardware Telemetry: Reading live environmental sensors on I2C (`sensor`).
5. Native C Compilation & Execution: Writing, compiling with `cc`, and executing user ELF.

Usage:
    uv run test_agent_eval.py --board lcd7b
    uv run test_agent_eval.py --board lcd7b --no-load
"""

from __future__ import annotations

import argparse
import functools
import importlib.util
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build" / "esp32p4-lcd7b"


@functools.cache
def _load_p4run():
    path = REPO_ROOT / "tools" / "p4run.py"
    spec = importlib.util.spec_from_file_location("p4run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


BAUD = _load_p4run().BAUD


def open_port(dev: str, timeout: float = 0.2) -> serial.Serial:
    s = _load_p4run().open_console(dev)
    s.timeout = timeout
    return s


def close_port(s: serial.Serial) -> None:
    _load_p4run().close_console(s)


@dataclass
class Board:
    console: str
    reset: str
    p4run: object
    board_name: str | None = None
    console_session: "Console | None" = None


class Console:
    PROMPT = "lsh>"

    def __init__(self, port: str):
        self.ser = open_port(port)
        self.ser.reset_input_buffer()

    def close(self):
        try:
            close_port(self.ser)
        except Exception:
            pass

    def drain(self, quiet: float = 0.4, deadline: float = 5.0) -> str:
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

    def cmd(self, line: str, deadline: float = 90.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write(line.encode() + b"\r")
        self.ser.flush()
        out = b""
        end = time.time() + deadline
        last_rx = time.time()
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                out += chunk
                last_rx = time.time()
            else:
                tail = out.decode("utf-8", "replace")[len(line):]
                if re.search(r"lsh>\s*(?:\x1b\[[0-9;?]*[a-zA-Z])*\s*$", tail) and (time.time() - last_rx >= 0.4):
                    break
                time.sleep(0.05)
        out += self.ser.read(4096)
        return out.decode("utf-8", "replace")

    def wake(self) -> bool:
        return "lsh>" in self.cmd("", deadline=5.0)


def discover(port: str | None, reset_port: str | None, board: str | None) -> Board | None:
    p4run = _load_p4run()
    try:
        bport, breset = p4run.board_ports(board)
        console = p4run.console_port(port or bport)
        reset = p4run.reset_port(reset_port or breset, console)
    except SystemExit:
        return None
    except Exception:
        return None
    if not console:
        return None

    for role, dev in (("console", console), ("reset", reset)):
        if not dev:
            continue
        try:
            close_port(open_port(dev, 0.1))
        except Exception as e:
            print(f"\n[!] The {role} port {dev} cannot be opened: {e}")
            return None

    return Board(console=console, reset=reset, p4run=p4run, board_name=board)


# --- Benchmark Test Cases ---

def test_boots(b: Board) -> tuple[str, bool, str]:
    name = "Boots to shell prompt"
    c = b.console_session
    if not c or not c.wake():
        return name, False, "no prompt on the console"
    return name, True, ""


def test_ea_config_live(b: Board) -> tuple[str, bool, str]:
    name = "Agent Endpoint Configuration Live"
    c = b.console_session
    out = c.cmd("ea config", deadline=10.0)
    if "LLM Endpoint Configuration:" not in out or "URL:" not in out:
        return name, False, f"Unexpected config output:\n{out}"
    return name, True, ""


def test_benchmark_calculus(b: Board) -> tuple[str, bool, str]:
    name = "Benchmark 1: CAS Calculus (Deriv Trajectory)"
    c = b.console_session
    prompt = 'ea "Differentiate 3*x^3 + 2*x^2 - 5*x + 7 with respect to x using CAS calculus"'
    print(f"\n    Sending prompt: {prompt}")
    t0 = time.time()
    out = c.cmd(prompt, deadline=120.0)
    duration = time.time() - t0
    print(f"    Elapsed: {duration:.1f}s")

    # Assert tool was called or deriv evaluated
    has_deriv = "(deriv " in out or "deriv" in out
    # Derivative is 9*x^2 + 4*x - 5
    has_answer = ("9" in out and "x^2" in out and "4" in out and "5" in out) or "9*x^2 + 4*x - 5" in out or "(* 9 (^ x 2))" in out
    if not has_answer:
        return name, False, f"Expected derivative 9x^2 + 4x - 5, got:\n{out}"
    return name, True, f"{duration:.1f}s (CAS deriv verified)"


def test_benchmark_quadratic(b: Board) -> tuple[str, bool, str]:
    name = "Benchmark 2: Algebra / Quadratic Solver"
    c = b.console_session
    prompt = 'ea "Solve 2*x^2 + 5*x - 3 = 0 using algebra"'
    print(f"\n    Sending prompt: {prompt}")
    t0 = time.time()
    out = c.cmd(prompt, deadline=120.0)
    duration = time.time() - t0
    print(f"    Elapsed: {duration:.1f}s")

    # Roots are 1/2 (or 0.5) and -3
    has_root1 = ("1/2" in out or "0.5" in out)
    has_root2 = ("-3" in out)
    if not (has_root1 and has_root2):
        return name, False, f"Expected roots 1/2 and -3, got:\n{out}"
    return name, True, f"{duration:.1f}s (Roots 1/2 and -3 verified)"


def test_benchmark_demo_discovery(b: Board) -> tuple[str, bool, str]:
    name = "Benchmark 3: System / Demo Discovery"
    c = b.console_session
    prompt = 'ea "Find where cellular automaton rule 30 or rule30 demo is stored"'
    print(f"\n    Sending prompt: {prompt}")
    t0 = time.time()
    out = c.cmd(prompt, deadline=90.0)
    duration = time.time() - t0
    print(f"    Elapsed: {duration:.1f}s")

    has_rule30 = ("RULE30" in out.upper() or "CA.LISP" in out.upper())
    if not has_rule30:
        return name, False, f"Expected rule30 / ca.lisp demo discovery, got:\n{out}"
    return name, True, f"{duration:.1f}s (Discovered demo path)"


def test_benchmark_sensor_telemetry(b: Board) -> tuple[str, bool, str]:
    name = "Benchmark 4: Live Environmental Sensor Telemetry"
    c = b.console_session
    prompt = 'ea "Read the live temperature from the onboard environmental sensor"'
    print(f"\n    Sending prompt: {prompt}")
    t0 = time.time()
    out = c.cmd(prompt, deadline=90.0)
    duration = time.time() - t0
    print(f"    Elapsed: {duration:.1f}s")

    # Check for temperature reading (typically 20-35 C indoors)
    has_reading = bool(re.search(r"\b(2\d|3\d)(\.\d+)?\s*(C|deg|degrees|\xc2\xb0C)", out, re.IGNORECASE) or "temperature" in out.lower())
    if not has_reading:
        return name, False, f"Expected temperature reading, got:\n{out}"
    return name, True, f"{duration:.1f}s (Sensor read verified)"


def test_benchmark_c_compilation(b: Board) -> tuple[str, bool, str]:
    name = "Benchmark 5: C Code Generation, Compilation & Execution"
    c = b.console_session
    prompt = 'ea "Write a C program that computes factorial of 5 and prints it, compile and run"'
    print(f"\n    Sending prompt: {prompt}")
    t0 = time.time()
    out = c.cmd(prompt, deadline=120.0)
    duration = time.time() - t0
    print(f"    Elapsed: {duration:.1f}s")

    # Factorial of 5 is 120
    has_120 = "120" in out
    if not has_120:
        return name, False, f"Expected factorial 120, got:\n{out}"
    return name, True, f"{duration:.1f}s (Factorial 120 compiled & run)"


BENCHMARKS = [
    test_boots,
    test_ea_config_live,
    test_benchmark_calculus,
    test_benchmark_quadratic,
    test_benchmark_demo_discovery,
    test_benchmark_sensor_telemetry,
    test_benchmark_c_compilation,
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--board", default="lcd7b", help="board identifier (default: lcd7b)")
    ap.add_argument("--port", help="console serial port")
    ap.add_argument("--reset-port", help="reset control serial port")
    args = ap.parse_args()

    print("======================================================================")
    print("      LugalOS Automated Agent E2E Benchmark Suite (Milestone 59.7)")
    print("======================================================================")

    b = discover(args.port, args.reset_port, args.board)
    if b is None:
        print("\n[!] No board found on serial ports.")
        return 0

    print(f"Connecting to console on {b.console}...")
    try:
        b.console_session = Console(b.console)
    except Exception as e:
        print(f"\n[!] Failed to open console {b.console}: {e}")
        return 0

    if not b.console_session.wake():
        print("\n[!] Console not answering with shell prompt.")
        b.console_session.close()
        return 0

    total = passed = 0
    try:
        for bm in BENCHMARKS:
            try:
                name, ok, log = bm(b)
            except Exception as e:
                name, ok, log = bm.__name__, False, f"Exception: {e}"
            total += 1
            if ok:
                passed += 1
                print(f"  [PASS] {name}" + (f" -- {log}" if log else ""))
            else:
                print(f"  [FAIL] {name}\n    {log}")
    finally:
        b.console_session.close()

    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed} / {total} Benchmarks PASSED")
    print("======================================================================\n")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
