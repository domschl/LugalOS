#!/usr/bin/env python3
"""tests/hw/lcdterm_shots.py -- the panel terminal and the ribbon, in pictures.

47.6, plan/phase47_esp32p4_lcd7b_ribbon.md: the RP2350 terminal is the
regression target when the portable terminal moves out of the RP2350 panel
driver. This plays a fixed scenario on a board's shell, takes a PBM
screenshot after the steps that name one, fetches the shots over 9P, and
keeps the console's answers too.

    uv run lcdterm_shots.py --out /tmp/before
    ... rebuild, reflash ...
    uv run lcdterm_shots.py --out /tmp/after --compare /tmp/before

Scenarios (--scenario):
  terminal  text, Lisp output, rule 90 on the canvas in every split, the
            ribbon's resizes, the canvas alone, back to text (47.6's)
  ribbon    47.8: several terminals and canvases -- a second terminal, rule
            30 on its canvas beside the first's rule 90, focus and moves
            along the ribbon, a canvas closed and reopened

Boards (--board):
  rp2350    the RP2350 terminal: console on if00, 9P on if02 (default)
  lcd7b     the ESP32-P4-WIFI6-Touch-LCD-7B: one CH343P for the console;
            the shots come back over 9P on the same wire (`p9share`)
  tab5      the M5Stack Tab5, the same way over its USB-Serial/JTAG; its
            shots are the portrait frame and are compared turned back to
            the landscape screen the terminal draws

A comparison diffs every shot pixel by pixel, as RGB: a 1-bpp PBM, a GRAY8
PGM and an RGB565 PPM (the panel's three frame formats) compare with each
other, and a format change is reported beside the result. The right end of
the status bar -- the clock and the link icons, which come and go -- is
masked; everything else must be identical. Exit status 0 means identical.
Nothing is flashed here; flash first.
"""
from __future__ import annotations

import argparse
import re
import secrets
import sys
import time
from pathlib import Path

import serial

import rp2350

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host" / "p9lib" / "src"))
import p9lib  # noqa: E402

# (shell line, seconds to let it finish, screenshot name or None)
TERMINAL: list[tuple[str, float, str | None]] = [
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

# 47.8: the ribbon -- phase 44's verify_multicanvas / verify_switch, on
# whichever width the board has. "KEY:" steps are the keyboard's own hotkeys,
# sent as the escape sequences kernel/keyseq.c decodes (Super is mods 8:
# ESC [ 1 ; 9 D is Cmd+Left, ESC [ cp ; 9 u is Cmd+cp), so the ribbon is
# driven exactly as a keyboard would drive it -- from inside chess, too.
KEYS: dict[str, bytes] = {
    "cmd-enter": b"\x1b[13;9u",
    "cmd-left": b"\x1b[1;9D",
    "cmd-right": b"\x1b[1;9C",
    "cmd-ctrl-left": b"\x1b[1;13D",
    "cmd-ctrl-right": b"\x1b[1;13C",
    "cmd-w": b"\x1b[119;9u",
    "cmd-backslash": b"\x1b[92;9u",
    **{f"cmd-{n}": b"\x1b[%d;9u" % (48 + n) for n in range(1, 10)},
}

RIBBON: list[tuple[str, float, str | None]] = [
    ("clear", 1.0, None),
    ('(load "/sd0/demos/ca.lisp")', 3.0, None),
    ("(ca 90 4)", 3.0, "t0-rule90"),
    ("KEY:cmd-enter", 3.0, "t1-new"),             # a terminal, the keyboard's way
    ("clear", 1.0, None),
    ('(load "/sd0/demos/ca.lisp")', 3.0, None),
    ("(ca 30 4)", 3.0, "t1-rule30"),
    ("ribbon", 1.5, None),                        # FOCUS: steps look windows up here
    ("FOCUS:term-0", 2.0, "jump-t0"),
    ("(chess)", 5.0, None),                       # the board on terminal 0's canvas
    ("(+ 100 23)", 1.5, None),                    # Lisp at the chess> prompt
    ("FOCUS:canvas-of-0", 2.0, "chess-board"),
    ("FOCUS:canvas-of-1", 2.0, "nav-rule30"),     # away, across the ribbon ...
    ("FOCUS:canvas-of-0", 2.0, "chess-back"),     # ... and back: the board untouched
    ("FOCUS:term-0", 2.0, None),
    ("quit", 2.0, None),
    ("canvas close", 3.0, "t0-canvas-closed"),
    ("ribbon", 1.5, None),
    ("canvas split", 3.0, "t0-canvas-reopened"),
    ("ribbon", 1.5, None),
    ("KEY:cmd-ctrl-right", 2.0, "t0-moved-right"),
    ("ribbon", 1.5, None),
    ('(load "/sd0/demos/lorenz.lisp")', 3.0, None),
    ("(lorenz 3000)", 8.0, "lorenz"),
    ('(load "/sd0/cas/cas.lisp")', 15.0, None),       # the CAS whole: plot titles use to-infix
    ("(plot '(- (^ x 3) (* 3 x)) '(x -3 3))", 8.0, "plot"),
    ("ribbon", 1.5, None),
]

# FOCUS:<what> is Cmd+N for the window the last `ribbon` listing names: the
# order depends on where each window went in (phase 44: a terminal right of
# the focused window, a canvas right of its terminal), and chess has no
# `ribbon` command to ask with.
FOCUS_RE: dict[str, str] = {
    "term-0": r"\[(\d+)\] Term\s+\(vterm 0,",
    "term-1": r"\[(\d+)\] Term\s+\(vterm 1,",
    "canvas-of-0": r"\[(\d+)\] Canvas \(slot \d+ of vterm 0,",
    "canvas-of-1": r"\[(\d+)\] Canvas \(slot \d+ of vterm 1,",
}

# Shots that must be pixel-identical within one run (clock masked).
RIBBON_SAME: list[tuple[str, str]] = [("chess-board", "chess-back")]

# The status bar's right end -- "HH:MM" and the link icons left of it -- in
# 16-pixel cells; scaled by the cell height (32 on the Tab5).
CLOCK_MASK_PX = 120
STATUS_BAR_ROWS = 16


def run_line(ser: serial.Serial, line: str, wait: float) -> str:
    """Type `line`, then read until the shell's next prompt -- not until a
    quiet spell: (ca ...) computes silently for seconds, and a tool that
    types on into a busy board gets its steps out of order.

    No rp2350.drain()-style kick: a kick is a keystroke, and every one
    shows on the screen as an extra prompt -- a run that kicked once more
    than the last differed from it by exactly those lines."""
    ser.reset_input_buffer()
    if line.startswith("KEY:") or line.startswith("RAW:"):   # no prompt to wait for
        ser.write(KEYS[line[4:]] if line.startswith("KEY:") else line[4:].encode() + b"\r")
        ser.flush()
        time.sleep(wait)
        return ser.read(65536).decode("utf-8", "replace")
    ser.write(line.encode() + b"\r")
    ser.flush()
    out = b""
    end = time.time() + max(60.0, wait)
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            tail = out.split(line.encode()[-12:], 1)[-1]
            if b"lsh>" in tail or b"chess>" in tail:
                break
    time.sleep(wait if wait < 2.0 else 2.0)      # let the screen settle
    return out.decode("utf-8", "replace")


class Rp2350Board:
    """Console on if00, 9P on if02 -- two ports, so fetching needs nothing
    from the console."""
    def __init__(self, console: str | None, net: str | None) -> None:
        self.ports = rp2350.discover_ports(console=console, net=net)

    def present(self) -> bool:
        return self.ports is not None and rp2350.wake_shell(self.ports.console)

    def open(self) -> serial.Serial:
        ser = serial.Serial(self.ports.console, 115200, timeout=0.5)
        ser.dtr = True
        time.sleep(0.3)
        return ser

    def close(self, ser: serial.Serial) -> None:
        ser.close()

    def fetch(self, paths: list[str]) -> list[bytes]:
        client = p9lib.connect_serial(self.ports.net, framing="raw")
        try:
            return [client.cat(p) for p in paths]
        finally:
            client.close()


class P4Board:
    """An ESP32-P4 board (the LCD-7B, the Tab5): one serial line carries the
    console and, for the fetch, 9P in SLIP frames beside it (`p9share`, as the
    P4 suite's 9P test does). Opened only through tools/p4run.py: a plain open
    resets the LCD-7B."""
    def __init__(self, name: str) -> None:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
        import p4run  # noqa: E402
        self.p4run = p4run
        self.port, _ = p4run.board_ports(name)
        self.name = name

    def present(self) -> bool:
        return bool(self.port)

    def open(self) -> serial.Serial:
        ser = self.p4run.open_console(self.port)
        ser.timeout = 0.5
        ser.write(b"\r")
        time.sleep(0.5)
        return ser

    def close(self, ser: serial.Serial) -> None:
        self.p4run.close_console(ser)

    def fetch(self, paths: list[str]) -> list[bytes]:
        if self.name == "tab5":
            return self.fetch_tcp(paths)
        ser = self.open()
        run_line(ser, "p9share", 2.0)
        self.close(ser)
        time.sleep(0.3)
        try:
            client = p9lib.connect_open_serial(self.p4run.open_console(self.port), framing="slip",
                                               timeout=20.0, closer=self.p4run.close_console)
            try:
                sess = p9lib.Session(client)
                return [sess.read(p) for p in paths]
            finally:
                client.close()
        finally:
            time.sleep(0.3)
            ser = self.open()
            run_line(ser, "p9share off", 2.0)
            self.close(ser)


    def fetch_tcp(self, paths: list[str]) -> list[bytes]:
        """The Tab5's console is its USB-Serial/JTAG, which `p9share` does not
        demultiplex: the shots come over 9P on TCP instead, through its Wi-Fi,
        with a key for this fetch alone (cleared afterwards)."""
        key = secrets.token_hex(16)
        ser = self.open()
        try:
            run_line(ser, f"p9key {key}", 2.0)
            net = run_line(ser, "cat /proc/net", 3.0)
        finally:
            self.close(ser)
        addr = None
        for block in net.split("interface: ")[1:]:
            m = re.search(r"address: (\d+\.\d+\.\d+\.\d+)", block)
            if block.startswith("wlan0") and m and m.group(1) != "0.0.0.0":
                addr = m.group(1)
        if not addr:
            raise SystemExit("tab5: no address on wlan0 -- the shots cannot be fetched")
        try:
            sess = p9lib.Session(p9lib.connect_tcp(addr, 564, timeout=20.0), key=bytes.fromhex(key))
            try:
                return [sess.read(p) for p in paths]
            finally:
                sess.close()
        finally:
            ser = self.open()
            run_line(ser, "p9key clear", 2.0)
            self.close(ser)


def ribbon_cleanup(ser: serial.Serial, log: list[str]) -> None:
    """Back to one terminal with no canvas, so the scenario can run again:
    every terminal but 0 exits, then terminal 0 goes back to text."""
    for _ in range(6):
        out = run_line(ser, "ribbon", 1.5)
        log.append(f"--- $ ribbon\n{out}")
        terms = re.findall(r"\[(\d+)\] Term\s+\(vterm (\d+)", out)
        others = [int(i) for i, v in terms if v != "0"]
        if not others:
            zero = [int(i) for i, v in terms if v == "0"]
            if zero:
                run_line(ser, f"ribbon focus {zero[0]}", 1.5)
            break
        run_line(ser, f"ribbon focus {others[0]}", 1.5)
        log.append(f"--- $ RAW:exit\n{run_line(ser, 'RAW:exit', 2.0)}")
    run_line(ser, "(canvas-window 'text)", 2.0)
    run_line(ser, "clear", 1.0)


def ribbon_windows(ser: serial.Serial) -> int:
    m = re.search(r"Ribbon Status: (\d+) windows", run_line(ser, "ribbon", 1.5))
    return int(m.group(1)) if m else -1


def parse_shot(data: bytes) -> tuple[int, int, str, list[tuple[int, int, int]]]:
    """A screenshot as (w, h, format, RGB pixels): P4 (1 = black), P5 or P6,
    as kernel/screenshot.c writes them."""
    kind = data[:2]
    if kind == b"P4":
        parts = data.split(maxsplit=3)
        w, h = int(parts[1]), int(parts[2])
        stride = (w + 7) // 8
        body = parts[3][-stride * h:]
        px = [(0, 0, 0) if (body[y * stride + x // 8] >> (7 - x % 8)) & 1 else (255, 255, 255)
              for y in range(h) for x in range(w)]
        return w, h, "1-bpp", px
    if kind in (b"P5", b"P6"):
        parts = data.split(maxsplit=4)
        w, h = int(parts[1]), int(parts[2])
        n = 3 if kind == b"P6" else 1
        body = parts[4][-w * h * n:]
        if n == 1:
            return w, h, "gray8", [(v, v, v) for v in body]
        return w, h, "rgb", [(body[i], body[i + 1], body[i + 2]) for i in range(0, len(body), 3)]
    raise ValueError("not a PBM, PGM or PPM")


def landscape(w: int, h: int, px: list[tuple[int, int, int]]) -> tuple[int, int, list[tuple[int, int, int]]]:
    """The Tab5's portrait frame (720x1280, the terminal at 270 degrees) as the
    1280x720 screen: logical (x, y) is physical (y, 1279 - x)."""
    if (w, h) != (720, 1280):
        return w, h, px
    return 1280, 720, [px[(1279 - x) * 720 + y] for y in range(720) for x in range(1280)]


def diff_shots(a: bytes, b: bytes) -> tuple[int, int, int, int, str]:
    """(differing pixels outside the mask, total pixels, w, h, format note)."""
    wa, ha, fa, pa = parse_shot(a)
    wb, hb, fb, pb = parse_shot(b)
    wa, ha, pa = landscape(wa, ha, pa)
    wb, hb, pb = landscape(wb, hb, pb)
    note = "" if fa == fb else f" (format {fa} -> {fb})"
    if (wa, ha) != (wb, hb):
        return -1, wa * ha, wa, ha, note
    scale = 2 if ha >= 720 else 1          # the Tab5's 32-pixel cells
    bar, mask = STATUS_BAR_ROWS * scale, CLOCK_MASK_PX * scale
    bad = 0
    for i, (x, y) in enumerate(zip(pa, pb)):
        if x != y:
            row, col = divmod(i, wa)
            if row < bar and col >= wa - mask:
                continue
            bad += 1
    return bad, wa * ha, wa, ha, note


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path, help="directory for shots and the console log")
    ap.add_argument("--compare", type=Path, help="a previous --out directory to diff against")
    ap.add_argument("--board", choices=["rp2350", "lcd7b", "tab5"], default="rp2350")
    ap.add_argument("--scenario", choices=["terminal", "ribbon"], default="terminal")
    ap.add_argument("--console", help="RP2350: override the console port (if00)")
    ap.add_argument("--net", help="RP2350: override the 9P port (if02)")
    args = ap.parse_args()

    board = Rp2350Board(args.console, args.net) if args.board == "rp2350" else P4Board(args.board)
    if not board.present():
        print(f"no {args.board} board answering -- nothing to do")
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    scenario = TERMINAL if args.scenario == "terminal" else RIBBON

    shots: list[str] = []
    log: list[str] = []
    ser = board.open()
    try:
        listing = ""
        for line, wait, shot in scenario:
            if line.startswith("FOCUS:"):
                m = re.search(FOCUS_RE[line[6:]], listing)
                if not m:
                    raise SystemExit(f"{line}: not in the last ribbon listing:\n{listing}")
                line = f"KEY:cmd-{int(m.group(1)) + 1}"
            out = run_line(ser, line, wait)
            if line == "ribbon":
                listing = out
            log.append(f"--- $ {line}\n{out}")
            if shot:
                # 8.3 names: fs/fat32.c keeps short names only, so long ones
                # sharing a prefix land in one file (they did, once).
                path = f"/sd0/LS{len(shots)}.PBM"
                out = run_line(ser, f"screenshot {path}", 3.0)
                log.append(f"--- $ screenshot {path}\n{out}")
                shots.append(shot)
        if args.scenario == "terminal":
            run_line(ser, "(canvas-window 'text)", 2.0)
            run_line(ser, "clear", 1.0)
        else:
            ribbon_cleanup(ser, log)
            left = ribbon_windows(ser)          # the exits took their canvases along
            run_line(ser, "clear", 1.0)
    finally:
        board.close(ser)
    (args.out / "console.log").write_text("".join(log))

    datas = board.fetch([f"/sd0/LS{i}.PBM" for i in range(len(shots))])
    for shot, data in zip(shots, datas):
        (args.out / f"{shot}.pbm").write_bytes(data)
        print(f"  {shot}.pbm  {len(data)} bytes")

    worst = 0
    if args.scenario == "ribbon":
        print(f"  after the cleanup: {left} window(s) {'(ok)' if left == 1 else '-- EXPECTED 1'}")
        if left != 1:
            worst = 1
        for x, y in RIBBON_SAME:
            bad, _, w, h, _ = diff_shots((args.out / f"{x}.pbm").read_bytes(), (args.out / f"{y}.pbm").read_bytes())
            print(f"  {x} == {y}: {'identical' if bad == 0 else f'{bad} pixels differ'}")
            if bad != 0:
                worst = 1
    if not args.compare:
        print("RESULT:", "ok" if worst == 0 else "DIFFERENT")
        return worst
    for shot in shots:
        a, b = args.compare / f"{shot}.pbm", args.out / f"{shot}.pbm"
        if not a.exists():
            print(f"  {shot}: no reference")
            worst = max(worst, 1)
            continue
        bad, total, w, h, note = diff_shots(a.read_bytes(), b.read_bytes())
        verdict = "identical" if bad == 0 else ("SIZE DIFFERS" if bad < 0 else f"{bad} pixels differ")
        print(f"  {shot}: {w}x{h} {verdict}{note}")
        if bad != 0:
            worst = 1
    print("RESULT:", "identical" if worst == 0 else "DIFFERENT")
    return worst


if __name__ == "__main__":
    sys.exit(main())
