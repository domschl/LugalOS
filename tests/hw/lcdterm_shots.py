#!/usr/bin/env python3
"""tests/hw/lcdterm_shots.py -- the panel terminal, before and after a refactor.

47.6, plan/phase47_esp32p4_lcd7b_ribbon.md: the RP2350 terminal is the
regression target when the portable terminal moves out of the RP2350 panel
driver. This plays one fixed scenario on the board's shell (text, Lisp
output, the three split layouts with the chess board on the canvas, the
rule 90 on the canvas in every split, the ribbon's resizes, the canvas
alone, back to text), takes a PBM screenshot after
each step, fetches the shots over 9P, and keeps the console's answers too.

    uv run lcdterm_shots.py --out /tmp/before
    ... rebuild, reflash ...
    uv run lcdterm_shots.py --out /tmp/after --compare /tmp/before

A comparison diffs every shot pixel by pixel. The clock at the right end of
the status bar is masked (it is the one thing allowed to change between two
runs); everything else must be identical. Exit status 0 means identical.

Boards: the RP2350 terminal (two CDC-ACM ports, 9P on if02). Nothing is
flashed here; flash first.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import serial

import rp2350

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host" / "p9lib" / "src"))
import p9lib  # noqa: E402

# (shell line, seconds to let it finish, screenshot name or None)
SCENARIO: list[tuple[str, float, str | None]] = [
    ("clear", 1.0, None),
    ("(+ 1 2)", 1.0, None),
    ("ls /flash0", 2.0, None),
    ('(display "47.6: the portable terminal")', 1.0, "text"),
    ("ribbonselftest", 3.0, None),
    ("vtselftest", 6.0, None),
    ("clear", 1.0, None),
    ('(load "/sd0/demos/ca.lisp")', 3.0, None),
    ("(ca 90 4)", 3.0, "split-wide"),
    ("(canvas-window 'split-half)", 3.0, "split-half"),
    ("(canvas-window 'split-narrow)", 3.0, "split-narrow"),
    ("ribbon", 1.5, None),
    ("ribbon full", 3.0, "ribbon-full"),
    ("ribbon narrow", 3.0, "ribbon-narrow"),
    ("(canvas-window 'canvas)", 3.0, "canvas"),
    ("(canvas-on-redraw '())", 1.0, None),
    ("(canvas-window 'text)", 3.0, "back-to-text"),
]

CLOCK_MASK_PX = 64       # the status bar's right end: "HH:MM" and its gap
STATUS_BAR_ROWS = 16


def run_line(ser: serial.Serial, line: str, wait: float) -> str:
    """Type `line`, then read until the shell's next prompt -- not until a
    quiet spell: (ca ...) computes silently for seconds, and a tool that
    types on into a busy board gets its steps out of order."""
    ser.reset_input_buffer()
    ser.write(line.encode() + b"\r")
    ser.flush()
    out = b""
    # No rp2350.drain()-style kick: a kick is a keystroke, and every one
    # shows on the screen as an extra prompt -- a run that kicked once more
    # than the last differed from it by exactly those lines.
    end = time.time() + max(60.0, wait)
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            if b"lsh>" in out.split(line.encode()[-12:], 1)[-1]:
                break
    time.sleep(wait if wait < 2.0 else 2.0)      # let the screen settle
    return out.decode("utf-8", "replace")


def parse_pbm(data: bytes) -> tuple[int, int, bytes]:
    """P4 (binary) only, as kernel/screenshot.c writes it."""
    parts = data.split(maxsplit=3)
    if parts[0] != b"P4":
        raise ValueError("not a binary PBM")
    w, h = int(parts[1]), int(parts[2])
    stride = (w + 7) // 8
    body = parts[3][-stride * h:]
    return w, h, body


def diff_pbm(a: bytes, b: bytes) -> tuple[int, int, int, int]:
    """(differing pixels outside the mask, total pixels, w, h)."""
    wa, ha, pa = parse_pbm(a)
    wb, hb, pb = parse_pbm(b)
    if (wa, ha) != (wb, hb):
        return -1, wa * ha, wa, ha
    stride = (wa + 7) // 8
    bad = 0
    for y in range(ha):
        for xb in range(stride):
            d = pa[y * stride + xb] ^ pb[y * stride + xb]
            if not d:
                continue
            for bit in range(8):
                if d & (0x80 >> bit):
                    x = xb * 8 + bit
                    if y < STATUS_BAR_ROWS and x >= wa - CLOCK_MASK_PX:
                        continue
                    bad += 1
    return bad, wa * ha, wa, ha


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path, help="directory for shots and the console log")
    ap.add_argument("--compare", type=Path, help="a previous --out directory to diff against")
    ap.add_argument("--console", help="override the console port (if00)")
    ap.add_argument("--net", help="override the 9P port (if02)")
    args = ap.parse_args()

    ports = rp2350.discover_ports(console=args.console, net=args.net)
    if ports is None:
        print("no RP2350 board found -- nothing to do")
        return 0
    if not rp2350.wake_shell(ports.console):
        print(f"no shell prompt on {ports.console}")
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    shots: list[str] = []
    log: list[str] = []
    with serial.Serial(ports.console, 115200, timeout=0.5) as ser:
        ser.dtr = True
        time.sleep(0.3)
        for line, wait, shot in SCENARIO:
            out = run_line(ser, line, wait)
            log.append(f"--- $ {line}\n{out}")
            if shot:
                # 8.3 names: fs/fat32.c keeps short names only, so long ones
                # sharing a prefix land in one file (they did, once).
                path = f"/sd0/LS{len(shots)}.PBM"
                out = run_line(ser, f"screenshot {path}", 3.0)
                log.append(f"--- $ screenshot {path}\n{out}")
                shots.append(shot)
        run_line(ser, "(canvas-window 'text)", 2.0)
        run_line(ser, "clear", 1.0)
    (args.out / "console.log").write_text("".join(log))

    client = p9lib.connect_serial(ports.net, framing="raw")
    try:
        for i, shot in enumerate(shots):
            data = client.cat(f"/sd0/LS{i}.PBM")
            (args.out / f"{shot}.pbm").write_bytes(data)
            print(f"  {shot}.pbm  {len(data)} bytes")
    finally:
        client.close()

    if not args.compare:
        return 0
    worst = 0
    for shot in shots:
        a, b = args.compare / f"{shot}.pbm", args.out / f"{shot}.pbm"
        if not a.exists():
            print(f"  {shot}: no reference")
            worst = max(worst, 1)
            continue
        bad, total, w, h = diff_pbm(a.read_bytes(), b.read_bytes())
        verdict = "identical" if bad == 0 else ("SIZE DIFFERS" if bad < 0 else f"{bad} pixels differ")
        print(f"  {shot}: {w}x{h} {verdict}")
        if bad != 0:
            worst = 1
    print("RESULT:", "identical" if worst == 0 else "DIFFERENT")
    return worst


if __name__ == "__main__":
    sys.exit(main())
