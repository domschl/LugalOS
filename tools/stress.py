#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial"]
# ///
"""tools/stress.py -- the hardware stress run (plan/phase63 §63.7) on an ESP32-P4 board.

The board runs `stress bg` (kernel/stress.c: worker tasks on the shared paths,
every result checked) while this host adds the rest of a busy day:

  * the environmental sensors sampled every --period seconds (`sensor period`)
  * three terminals on the panel (two opened with Cmd+Enter), focus moved
    between them, commands typed into whichever has it
  * 9P over TCP when the board has an address (`/proc/net`): a one-time
    `p9key`, files read in a loop from a thread
  * the touch poller and the automatic power log, which run anyway

Pass: STRESS_OK, and nothing in `dmesg` about a trap, a lock or scheduler
BUG, an I2C or SDIO error, a DMA error or an underrun, and no new touch
error. Everything the board printed goes to --log.

    uv run tools/stress.py --board tab5 --minutes 60
"""
from __future__ import annotations

import argparse
import re
import secrets
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
sys.path.insert(0, str(REPO / "host" / "p9lib" / "src"))
import p4run  # noqa: E402

CMD_ENTER = b"\x1b[13;9u"
CMD_W = b"\x1b[119;9u"
CMD_N = {n: b"\x1b[%d;9u" % (48 + n) for n in range(1, 10)}

TYPED = ["ps", "cat /proc/meminfo", "(+ 40 2)", "ls /sd0", "sensor", "power", "lockcheck",
         "cat /proc/version", "i2cstats", "(length (list 1 2 3 4 5))", "power log"]
BAD = re.compile(r"\[Trap|Lock BUG|Sched BUG|I2C error|SDIO.*(error|fail)|DMA error|underrun [1-9]|"
                 r"stack FULL|STRESS_FAIL|Guru|panic", re.I)


class Console:
    """The board's console, opened the way p4run.py does (a plain open resets the LCD-7B)."""
    def __init__(self, board: str, log: Path) -> None:
        self.port, _ = p4run.board_ports(board)
        self.s = p4run.open_console(self.port)
        self.s.timeout = 0.1
        self.log = log.open("ab")
        self.tail = b""
        self.lock = threading.Lock()

    def pump(self, secs: float) -> bytes:
        got = b""
        end = time.monotonic() + secs
        while time.monotonic() < end:
            b = self.s.read(65536)
            if b:
                got += b
                self.log.write(b)
        self.tail = (self.tail + got)[-200_000:]
        return got

    def send(self, data: bytes) -> None:
        with self.lock:
            self.s.write(data)

    def cmd(self, line: str, wait: float = 1.5) -> str:
        self.send(line.encode() + b"\r")
        return self.pump(wait).decode(errors="replace")

    def close(self) -> None:
        p4run.close_console(self.s)
        self.log.close()


def board_address(c: Console) -> str | None:
    out = c.cmd("cat /proc/net", 2.0)
    for block in out.split("interface: ")[1:]:
        m = re.search(r"address: (\d+\.\d+\.\d+\.\d+)", block)
        if m and m.group(1) != "0.0.0.0":
            return m.group(1)
    return None


def p9_traffic(addr: str, key: str, until: float, stats: dict[str, int]) -> None:
    import p9lib  # noqa: E402
    while time.monotonic() < until:
        try:
            sess = p9lib.Session(p9lib.connect_tcp(addr, 564, timeout=10.0), key=bytes.fromhex(key))
        except OSError:
            stats["connect_fail"] += 1
            time.sleep(2.0)
            continue
        try:
            while time.monotonic() < until:
                for path in ("/proc/version", "/proc/meminfo", "/proc/sensors"):
                    data = sess.read(path)
                    stats["reads"] += 1
                    if path == "/proc/version" and b"LugalOS" not in data:
                        stats["bad"] += 1
        except Exception:
            stats["errors"] += 1
        finally:
            try:
                sess.close()
            except Exception:
                pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", required=True, help="nano, lcd7b or tab5 (as p4run.py --board)")
    ap.add_argument("--minutes", type=float, default=60.0)
    ap.add_argument("--period", type=int, default=2, help="environmental sensor period during the run, s")
    ap.add_argument("--norot", action="store_true", help="Tab5: do not turn the panel during the run")
    ap.add_argument("--only", default="", help="the board's paths to run, e.g. sd,p9 (default: all)")
    ap.add_argument("--bare", action="store_true", help="no extra terminals and no typing: the board's load and 9P over TCP only")
    ap.add_argument("--log", type=Path, default=None, help="everything the board printed (default build/stress-BOARD.log)")
    a = ap.parse_args()
    secs = int(a.minutes * 60)
    log = a.log or REPO / "build" / f"stress-{a.board}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_bytes(b"")

    c = Console(a.board, log)
    try:
        c.cmd("", 0.5)
        if "LugalOS" not in c.cmd("cat /proc/version", 2.0):
            # A board left in download mode by a flash says nothing at all.
            print(f"{a.board}: the console does not answer (reset it: p4run.py --board {a.board} --run)")
            return 2
        panel = "up" in c.cmd("lcd", 2.0) and not a.bare
        touch0 = re.search(r"(\d+) I2C errors", c.cmd("ts", 1.5))
        c.cmd("dmesg -c", 2.0)                       # from here on, dmesg is this run's
        c.cmd(f"sensor period {a.period}", 1.0)
        if panel:
            for _ in range(2):
                c.send(CMD_ENTER)
                c.pump(2.0)
        addr = board_address(c)
        key = secrets.token_hex(16)
        stats = {"reads": 0, "errors": 0, "bad": 0, "connect_fail": 0}
        until = time.monotonic() + secs
        t = None
        if addr:
            c.cmd(f"p9key {key}", 1.0)
            t = threading.Thread(target=p9_traffic, args=(addr, key, until, stats), daemon=True)
            t.start()
        if panel:
            c.send(CMD_N[1])        # the run's report goes to the terminal that started it:
            c.pump(1.0)             # the first, the one the serial console shows
        c.cmd(f"stress bg {secs}{' norot' if a.norot else ''}{' only=' + a.only if a.only else ''}", 1.0)
        print(f"{a.board}: stress for {a.minutes:g} min; panel {'yes' if panel else 'no'}; "
              f"9P over TCP {'to ' + addr if addr else 'none (no address)'}", flush=True)

        i, focus, last_focus = 0, 1, time.monotonic()
        done_re = re.compile(rb"STRESS_(OK|FAIL)")
        hard = until + 150
        while time.monotonic() < hard and not done_re.search(c.tail):
            # The serial console shows the focused terminal: from 30 s before the
            # end, the first one keeps focus, so the report is seen.
            ending = time.monotonic() > until - 30.0
            if panel and ending and focus != 1:
                focus = 1
                c.send(CMD_N[1])
            elif panel and not ending and time.monotonic() - last_focus > 20.0:
                focus = focus % 3 + 1
                c.send(CMD_N[focus])
                last_focus = time.monotonic()
            if a.bare:
                c.pump(1.0)
            else:
                c.cmd(TYPED[i % len(TYPED)], 1.0)
                i += 1
            if i % 60 == 0:
                left = max(0, int(until - time.monotonic()))
                print(f"  {i} commands typed, {stats['reads']} 9P reads, {left} s left", flush=True)
        c.pump(3.0)
        verdict = done_re.search(c.tail)
        report = c.tail[c.tail.rfind(b"stress: ") if b"stress: " in c.tail else 0:].decode(errors="replace")

        if t:
            t.join(timeout=30)
            c.cmd("p9key clear", 1.0)
        c.cmd("sensor period 60", 1.0)
        if panel:
            for _ in range(2):
                c.send(CMD_W)
                c.pump(1.5)
        after = c.cmd("lockcheck", 2.0) + c.cmd("i2cstats", 2.0)
        touch1 = re.search(r"(\d+) I2C errors", c.cmd("ts", 1.5))
        dm = c.cmd("dmesg", 6.0)
    finally:
        c.close()

    bad = sorted({ln.strip() for ln in dm.splitlines() if BAD.search(ln) and "dmesg" not in ln})
    touch_new = (int(touch1.group(1)) - int(touch0.group(1))) if (touch0 and touch1) else 0
    print("\n".join(ln for ln in report.splitlines() if ln.startswith(("stress:", "  ", "STRESS"))))
    print(after.strip())
    print(f"9P over TCP: {stats['reads']} reads, {stats['errors']} session errors, {stats['bad']} bad, "
          f"{stats['connect_fail']} failed connects")
    print(f"touch: {touch_new} new I2C errors")
    for ln in bad[:20]:
        print("  dmesg:", ln)
    ok = bool(verdict and verdict.group(1) == b"OK") and not bad and touch_new == 0 and \
        stats["errors"] == 0 and stats["bad"] == 0
    print(f"RESULT: {'PASS' if ok else 'FAIL'} (log: {log})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
