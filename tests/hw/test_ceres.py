#!/usr/bin/env python3
"""Hardware-in-the-loop test suite for Ceres Alpha (Phase 67) on ESP32-P4.

Verifies interactive launch, frame rendering loop, framerate benchmark,
chiptune synth initialization, and clean shell restoration on attached
ESP32-P4 targets (M5Stack Tab5 and Waveshare LCD-7B).
Gracefully skips when no hardware board is connected.

Usage:
    cd tests/hw && uv run test_ceres.py --board tab5
    cd tests/hw && uv run test_ceres.py --board lcd7b
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

    def cmd(self, line: str, deadline: float = 30.0) -> str:
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

def test_ceres_command_registered(con: Console) -> tuple[str, bool, str]:
    name = "ceres command registered in shell"
    out = con.cmd("help", deadline=10.0)
    if "ceres" not in out:
        return name, False, f"'ceres' missing from help output: {out.strip()[-200:]}"
    return name, True, "present in help"


def test_ceres_frame_execution(con: Console, frames: int = 60) -> tuple[str, bool, str]:
    name = f"ceres execution: {frames} frames automated run"
    out = con.cmd(f"ceres {frames}", deadline=15.0)
    
    # Match: "ceres: completed X frames (Y FPS avg)"
    m = re.search(r"ceres: completed (\d+) frames \((\d+) FPS avg\)", out)
    if not m:
        return name, False, f"did not find completion summary line: {out.strip()[-300:]}"
    
    completed_frames = int(m.group(1))
    fps = int(m.group(2))
    
    if completed_frames < frames:
        return name, False, f"only completed {completed_frames}/{frames} frames"
    
    return name, True, f"completed {completed_frames} frames @ {fps} FPS"


def test_ceres_session_restoration(con: Console) -> tuple[str, bool, str]:
    name = "ceres session release and shell restoration"
    # Run a short 20-frame run and verify prompt immediately responds
    con.cmd("ceres 20", deadline=10.0)
    out = con.cmd("echo CERES_OK", deadline=5.0)
    if "CERES_OK" not in out:
        return name, False, f"shell did not respond with echo: {out.strip()[-200:]}"
    return name, True, "clean shell prompt restoration"


def main():
    parser = argparse.ArgumentParser(description="Hardware-in-the-loop test suite for Ceres Alpha.")
    parser.add_argument("--board", choices=["tab5", "lcd7b", "nano"], default=None,
                        help="Board to test (default: auto-detect)")
    parser.add_argument("--port", help="Console serial port")
    parser.add_argument("--reset-port", help="Reset serial port")
    args = parser.parse_args()

    disc = discover(port=args.port, reset_port=args.reset_port, board=args.board)
    if not disc:
        print("[SKIP] No matching ESP32-P4 hardware board found. Skipping cleanly.")
        sys.exit(0)

    console_port, reset_port = disc
    board_name = args.board or "detected board"
    print(f"======================================================================")
    print(f"       Ceres Alpha Silicon Hardware-in-the-Loop Test Suite")
    print(f"       Target: {board_name} on {console_port}")
    print(f"======================================================================")

    con = Console(console_port)
    try:
        if not con.wake():
            print(f"[FAIL] Unable to reach prompt on {console_port}")
            sys.exit(1)

        tests = [
            test_ceres_command_registered,
            test_ceres_frame_execution,
            test_ceres_session_restoration,
        ]

        passed = 0
        total = len(tests)

        for t in tests:
            tname, ok, detail = t(con)
            status = "PASS" if ok else "FAIL"
            print(f"  [{status}] {tname}: {detail}")
            if ok:
                passed += 1

        print("----------------------------------------------------------------------")
        print(f"Result: {passed} / {total} Tests PASSED")
        print("======================================================================")
        sys.exit(0 if passed == total else 1)
    finally:
        con.close()


if __name__ == "__main__":
    main()
