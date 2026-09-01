#!/usr/bin/env python3
"""Collect phase 24's P0 measurements from a clock board, and compute the two
numbers P0 exists to produce.

The board broadcasts one line a minute on UDP 5959 (drivers/dcf77_p0log.c).
It broadcasts rather than being polled because it has to be carried to
wherever the DCF-77 reception is: 9P over TCP would be the nicer channel, but
it requires authentication and this board has no persistent device key, and
its console belongs to the clock application. A board across the house has no
inbound channel at all, so the only way to hear from it is to listen.

    uv run collect_p0.py                      # listen, print, and append to a log
    uv run collect_p0.py --out p0.log         # ...somewhere specific
    uv run collect_p0.py --analyse p0.log     # just re-run the arithmetic

Each line is:

    p0 <persona> <seq> <t_s> <ntp_off_ms> <rtt_ms> <stratum> <dcf_err_ms|-> <q> <clean>

where `t_s` is seconds since the board's run began on its own free-running
clock, `ntp_off_ms` is what the reference says that clock is out by, and
`dcf_err_ms` -- present only in the minutes a frame was accepted -- is the
radio's claim minus the reference's truth at the same instant.

**The board never sets its clock during a run** (one deliberate step at
startup, then frozen), which is what makes the two series comparable: they are
two independent statements about the time, measured against one clock that is
allowed to drift. The drift is not noise here, it is the *other* result.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path

PORT = 5959


def parse(line: str) -> dict | None:
    f = line.split()
    if len(f) != 10 or f[0] != "p0":
        return None
    try:
        return {
            "persona": f[1], "seq": int(f[2]), "t_s": int(f[3]),
            "off_ms": int(f[4]), "rtt_ms": int(f[5]), "stratum": int(f[6]),
            "dcf_ms": None if f[7] == "-" else int(f[7]),
            "q": int(f[8]), "clean": int(f[9]),
        }
    except ValueError:
        return None


def analyse(rows: list[dict]) -> str:
    if len(rows) < 3:
        return f"{len(rows)} samples -- not enough for either result yet."

    out = [f"{len(rows)} samples over {(rows[-1]['t_s'] - rows[0]['t_s']) / 3600:.2f} h"]

    # --- the crystal, from the drift of the local clock against the reference.
    # A least-squares slope of offset (ms) against elapsed local seconds; times
    # 1000 because ms/s is already parts per thousand.
    n = len(rows)
    sx = sum(r["t_s"] for r in rows)
    sy = sum(r["off_ms"] for r in rows)
    sxx = sum(r["t_s"] ** 2 for r in rows)
    sxy = sum(r["t_s"] * r["off_ms"] for r in rows)
    den = n * sxx - sx * sx
    if den:
        slope = (n * sxy - sx * sy) / den          # ms per second
        ppm = slope * 1000.0
        out.append(f"  crystal      : {ppm:+.2f} ppm  ({ppm * 86.4:+.1f} ms/day drift)")
        resid = [r["off_ms"] - (sy / n + slope * (r["t_s"] - sx / n)) for r in rows]
        sd = (sum(e * e for e in resid) / len(resid)) ** 0.5
        out.append(f"  fit residual : {sd:.1f} ms sd  (how well a straight line describes it)")

    rtts = sorted(r["rtt_ms"] for r in rows)
    out.append(f"  ntp rtt      : min {rtts[0]} / median {rtts[len(rtts) // 2]} / max {rtts[-1]} ms")

    # --- the DCF path.
    d = [r["dcf_ms"] for r in rows if r["dcf_ms"] is not None]
    if not d:
        out.append("  dcf          : no frames accepted yet")
        best = max((r["clean"] for r in rows), default=0)
        out.append(f"                 longest clean run seen: {best} s of the 59 a frame needs")
    else:
        mean = sum(d) / len(d)
        sd = (sum((x - mean) ** 2 for x in d) / len(d)) ** 0.5
        ordered = sorted(d)
        out.append(f"  dcf offset   : mean {mean:+.1f} ms, sd {sd:.1f} ms, n={len(d)}")
        out.append(f"                 min {ordered[0]:+d} / median {ordered[len(ordered) // 2]:+d} "
                   f"/ max {ordered[-1]:+d} ms")
        out.append("")
        out.append(f"  -> P4's calibration constant is about {-mean:+.0f} ms;")
        out.append(f"     {sd:.0f} ms sd is the honest accuracy this path can claim.")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="p0.log", help="append received lines here")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--analyse", metavar="FILE",
                    help="skip listening; just analyse a log already collected")
    ap.add_argument("--every", type=int, default=10,
                    help="re-print the analysis every N samples")
    args = ap.parse_args()

    if args.analyse:
        rows = [r for r in (parse(l) for l in Path(args.analyse).read_text().splitlines()) if r]
        print(analyse(rows))
        return 0

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", args.port))
    print(f"listening on udp/{args.port}, appending to {args.out} -- Ctrl-C to stop\n")

    rows: list[dict] = []
    log = open(args.out, "a", buffering=1)
    try:
        while True:
            data, addr = s.recvfrom(512)
            for line in data.decode("utf-8", "replace").splitlines():
                r = parse(line)
                if not r:
                    continue
                log.write(line + "\n")
                rows.append(r)
                dcf = f"{r['dcf_ms']:+d} ms" if r["dcf_ms"] is not None else "-"
                print(f"[{time.strftime('%H:%M:%S')}] {addr[0]:>15}  seq={r['seq']:<5} "
                      f"t={r['t_s']:<6} off={r['off_ms']:+5d} rtt={r['rtt_ms']:<4} "
                      f"dcf={dcf:<10} clean={r['clean']}")
                if len(rows) % args.every == 0:
                    print("\n" + analyse(rows) + "\n")
    except KeyboardInterrupt:
        print("\n\n" + analyse(rows))
        return 0
    finally:
        log.close()


if __name__ == "__main__":
    sys.exit(main())
