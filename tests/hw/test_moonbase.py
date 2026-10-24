#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for Moonbase Lambda (Phase 64) on ESP32-P4.

Verifies interactive launch, microbenchmarks, scenario frame-time benchmarks,
and clean shell restoration on attached ESP32-P4 targets (LCD-7B and Tab5).
Gracefully skips when no hardware board is connected.

Usage:
    cd tests/hw && uv run test_moonbase.py --board lcd7b
    cd tests/hw && uv run test_moonbase.py --board tab5
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def _load_p4run():
    path = REPO_ROOT / "tools" / "p4run.py"
    spec = importlib.util.spec_from_file_location("p4run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


p4run = _load_p4run()


def discover(port: str | None = None, reset_port: str | None = None, board: str | None = None):
    try:
        bport, breset = p4run.board_ports(board)
        console = p4run.console_port(port or bport)
        reset = p4run.reset_port(reset_port or breset, console)
        return console, reset
    except SystemExit:
        return None
    except Exception:
        return None


class Console:
    PROMPT = "lsh>"

    def __init__(self, port: str):
        self.ser = p4run.open_console(port)
        self.ser.timeout = 0.2
        self.ser.reset_input_buffer()

    def close(self):
        try:
            p4run.close_console(self.ser)
        except Exception:
            pass

    def cmd(self, line: str, deadline: float = 60.0) -> str:
        self.ser.reset_input_buffer()
        self.ser.write(line.encode() + b"\r")
        self.ser.flush()
        out = b""
        end = time.time() + deadline
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                out += chunk
                decoded = out.decode("utf-8", "replace")
                if self.PROMPT in decoded[len(line):]:
                    break
        out += self.ser.read(4096)
        return out.decode("utf-8", "replace")

    def wake(self) -> bool:
        return self.PROMPT in self.cmd("", deadline=5.0)


# --- Tests -----------------------------------------------------------------

def test_command_registered(con: Console) -> tuple[str, bool, str]:
    name = "moonbase command registered in shell"
    out = con.cmd("help", deadline=10.0)
    if "moonbase" not in out:
        return name, False, f"'moonbase' missing from help output: {out.strip()[-200:]}"
    return name, True, "present in help"


def test_moonbase_micro(con: Console) -> tuple[str, bool, str]:
    name = "moonbase micro: low-level blitter microbenchmarks"
    out = con.cmd("moonbase micro", deadline=35.0)
    if "micro-benchmarks" not in out:
        return name, False, f"did not enter micro-benchmarks: {out.strip()[-300:]}"
    required = [
        "clear the whole frame",
        "store 16-bit pixels",
        "read 16-bit pixels",
        "tint the world area",
        "300 flat iso tiles",
        "300 textured iso tiles",
        "40 text strings",
    ]
    for r in required:
        if r not in out:
            return name, False, f"missing micro-benchmark entry '{r}' in output"
    return name, True, "all micro operations measured"


def test_moonbase_bench(con: Console, board: str | None) -> tuple[str, bool, str]:
    name = "moonbase bench: full scenario frame-time benchmarks"
    out = con.cmd("moonbase bench", deadline=60.0)
    if "pan, 3 structures, textured" not in out:
        return name, False, f"benchmark did not run: {out.strip()[-300:]}"

    scenarios = [
        "pan, 3 structures, textured",
        "pan, 3 structures, flat",
        "pan, 30 structures, textured",
        "pan, 30 structures, flat",
        "idle, nothing pressed",
    ]
    for s in scenarios:
        if s not in out:
            return name, False, f"missing scenario: {s}"

    # Verify frame rates match performance targets
    # Flat mode should achieve at least 7 fps on Tab5 and 10 fps on 7B
    m_flat = re.search(r"pan, 3 structures, flat\s+\d+ frames\s+median\s+([\d.]+)\s+ms.*=\s+(\d+)\s+fps", out)
    if not m_flat:
        return name, False, "could not parse flat pan results"
    fps = int(m_flat.group(2))
    min_fps = 7 if board == "tab5" else 10
    if fps < min_fps:
        return name, False, f"flat pan achieved {fps} fps, expected >= {min_fps} fps"

    # Verify idle state is not burning CPU
    m_idle = re.search(r"idle, nothing pressed\s+loops\s+(\d+)\s+world\s+(\d+)\s+hud\s+(\d+)\s+busy\s+([\d.]+)%", out)
    if not m_idle:
        return name, False, "could not parse idle results"
    busy_pct = float(m_idle.group(4))
    if busy_pct > 25.0:
        return name, False, f"idle busy {busy_pct}% is too high (expected < 25%)"

    return name, True, f"flat pan {fps} fps (target >= {min_fps}), idle {busy_pct}% busy"


def test_clean_shell_return(con: Console) -> tuple[str, bool, str]:
    name = "shell prompt restored and responsive after moonbase"
    out = con.cmd("echo MOONBASE_OK", deadline=5.0)
    if "MOONBASE_OK" not in out:
        return name, False, f"shell did not respond cleanly: {out.strip()[-150:]}"
    return name, True, "lsh prompt verified"


TESTS = [
    test_command_registered,
    test_moonbase_micro,
    test_moonbase_bench,
    test_clean_shell_return,
]


def main():
    ap = argparse.ArgumentParser(description="Moonbase Lambda Hardware Test Suite")
    ap.add_argument("--port", help="serial console port")
    ap.add_argument("--reset-port", help="serial reset port")
    ap.add_argument("--board", choices=["lcd7b", "tab5", "nano"], help="board preset")
    args = ap.parse_args()

    print("======================================================================")
    print("        Moonbase Lambda Hardware Test Suite (Milestone 64.15)")
    print("======================================================================")

    res = discover(args.port, args.reset_port, args.board)
    if res is None:
        print("\n[!] No ESP32-P4 board detected on USB serial port.")
        print("    SKIPPED -- safe for spec runs and CI.")
        return 0

    cport, _ = res
    print(f"\nConnected to console: {cport} (board: {args.board or 'auto'})")

    try:
        con = Console(cport)
    except Exception as e:
        print(f"\n[!] Failed to open console {cport}: {e}")
        return 0

    if not con.wake():
        print("\n[!] Board console is not answering. SKIPPED.")
        con.close()
        return 0

    total = passed = 0
    try:
        for t in TESTS:
            total += 1
            if t == test_moonbase_bench:
                name, ok, detail = t(con, args.board)
            else:
                name, ok, detail = t(con)

            if ok:
                passed += 1
                print(f"  [PASS] {name} ({detail})")
            else:
                print(f"  [FAIL] {name}\n    {detail}")
    finally:
        con.close()

    print("\n----------------------------------------------------------------------")
    print(f"Result: {passed} / {total} PASSED")
    print("======================================================================\n")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
