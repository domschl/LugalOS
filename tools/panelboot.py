#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial"]
# ///
"""tools/panelboot.py -- reset a P4 board N times and tell from a light sensor
whether its panel came up.

A TSL2591 (or TSL2561) on the board's I2C, facing the panel, is read after
each boot with the backlight on and then off (`sensor lux`, `lcd bl off`). A
panel showing its picture is far brighter than the backlight's leak through a
black one: on the Tab5, ~180 lux lit, ~2.6 lux black, ~0.9 lux backlight off,
in a dark room. The boot counts as lit when on - off >= --threshold lux.

Every boot's console output goes to --log; for a black boot the script also
asks for `lcd` and `dmesg`. --stop-on-black leaves the board in the black
state for a look by hand.

    uv run tools/panelboot.py --board tab5 --boots 30
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LUX = re.compile(r"(-?\d+)\.(\d+) Lux")


def p4run(board: str, cmds: list[str], run: bool, listen: float, wait: float) -> str:
    argv = [sys.executable, str(REPO / "tools" / "p4run.py"), "--board", board,
            "--cmd-wait", str(wait)]
    if run:
        argv += ["--run", "--listen-secs", str(listen)]
    for c in cmds:
        argv += ["--cmd", c]
    p = subprocess.run(argv, capture_output=True, text=True, errors="replace", timeout=180)
    return p.stdout + p.stderr


def luxes(out: str) -> list[float]:
    return [int(a) + int(b) / 100 for a, b in LUX.findall(out)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", required=True, help="as p4run.py --board (tab5, lcd7b, nano)")
    ap.add_argument("--boots", type=int, default=30)
    ap.add_argument("--settle", type=float, default=10.0, help="seconds to listen after each reset")
    ap.add_argument("--threshold", type=float, default=30.0, help="lux, backlight on minus off")
    ap.add_argument("--stop-on-black", action="store_true")
    ap.add_argument("--log", type=Path, default=Path("panelboot.log"))
    a = ap.parse_args()

    lit = black = unknown = 0
    with a.log.open("a") as log:
        log.write(f"\n=== panelboot {a.board} {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        for n in range(1, a.boots + 1):
            out = p4run(a.board, ["sensor lux", "lcd bl off", "sensor lux", "lcd bl on"],
                        run=True, listen=a.settle, wait=1.5)
            log.write(f"--- boot {n}\n{out}\n")
            v = luxes(out)
            if len(v) < 2:
                unknown += 1
                verdict = "no reading"
            else:
                diff = v[0] - v[1]
                if diff >= a.threshold:
                    lit += 1
                    verdict = "lit"
                else:
                    black += 1
                    verdict = "BLACK"
                verdict += f"  (on {v[0]:.2f}, off {v[1]:.2f} lux)"
            print(f"boot {n:3}/{a.boots}: {verdict}", flush=True)
            if verdict.startswith("BLACK"):
                diag = p4run(a.board, ["lcd", "dmesg"], run=False, listen=0, wait=3)
                log.write(f"--- boot {n} black: lcd, dmesg\n{diag}\n")
                if a.stop_on_black:
                    break
            log.flush()
    print(f"lit {lit}, black {black}, no reading {unknown}  (log: {a.log})")
    return 1 if black or unknown else 0


if __name__ == "__main__":
    sys.exit(main())
