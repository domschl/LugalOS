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

    p0 <persona> <seq> <t_s> <ntp_off_ms> <rtt_ms> <stratum> <dcf_err_ms|-> <q> <frames>

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
    # Lines may carry the sender's address as a leading field. Older logs do
    # not, and still parse -- see the note in the receive loop about why the
    # address is recorded at all.
    src = ""
    if f and f[0] != "p0" and len(f) > 1 and f[1] == "p0":
        src = f[0]
        f = f[1:]
    # 10 fields is the pre-P4 line and still parses: the PPS offset was
    # appended rather than inserted, so a log spanning a firmware change reads
    # correctly throughout instead of being silently dropped at the boundary.
    if len(f) not in (10, 11) or f[0] != "p0":
        return None
    try:
        return {
            "persona": f[1], "seq": int(f[2]), "t_s": int(f[3]),
            "off_ms": int(f[4]), "rtt_ms": int(f[5]), "stratum": int(f[6]),
            "dcf_ms": None if f[7] == "-" else int(f[7]),
            "src": src,
            "q": int(f[8]), "frames": int(f[9]),
            "pps_us": None if len(f) < 11 or f[10] == "-" else int(f[10]),
        }
    except ValueError:
        return None


def split_by_delay(rows: list[dict]) -> tuple[list[dict], list[dict]]:
    """Separate the samples whose round trip makes their offset untrustworthy.

    NTP cancels symmetric delay and cannot see asymmetry, so a reply that took
    `rtt` milliseconds carries up to +/-rtt/2 of unseen error. On a quiet
    segment that is a millisecond or two; on a WiFi link that has just
    retransmitted it can exceed the whole quantity being measured. One such
    sample in the first ten of a real run -- rtt 228 ms against a median of
    19 -- moved the mean by 17 ms and quadrupled the standard deviation.

    The threshold is three times the median rather than an absolute number, so
    it adapts to whatever the link actually is instead of encoding this
    bench's. Both populations are reported: an outlier that is silently
    dropped is indistinguishable from one that never happened."""
    if not rows:
        return [], []
    # Stratum first, and it is not a nicety. A reference that drops off its
    # own GPS keeps answering, keeps looking healthy, and carries a systematic
    # offset of its own -- which lands in every DCF figure computed through it
    # and is indistinguishable from the thing being measured. That happened
    # here between the P0 and P1 runs: 192.168.178.23 spent a stretch at
    # stratum 3, chained off the network, while still replying to everything.
    # Comparing across it would have conflated a fix with a reference change.
    graded = [r for r in rows if r["stratum"] == 1]
    if graded:
        rows = graded
    rtts = sorted(r["rtt_ms"] for r in rows)
    median = rtts[len(rtts) // 2]
    limit = max(3 * median, 10)
    good = [r for r in rows if r["rtt_ms"] <= limit]
    bad = [r for r in rows if r["rtt_ms"] > limit]
    return good, bad


def split_runs(rows: list[dict]) -> list[list[dict]]:
    """Split a log into boots, at every point the sequence number restarts.

    The board numbers its samples from zero at boot and measures t_s from its
    own start, so a log spanning a reboot holds two independent series sharing
    one range of sequence numbers and one range of timestamps. Concatenating
    them is not merely untidy: the crystal-drift fit regresses offset against
    t_s, and t_s jumping backwards makes that fit describe nothing at all.

    This replaces a dedupe-by-sequence-number that assumed repeated numbers
    meant duplicated datagrams. They did not -- the board had been restarted
    after a WLAN drop (user, 2026-09-03) -- and deduping would have silently
    discarded a whole second run rather than reporting it. Checked against the
    log that prompted it: three runs, and not one repeated sequence number
    inside any of them, so there were no duplicates to remove in the first
    place."""
    runs: list[list[dict]] = []
    cur: list[dict] = []
    prev: int | None = None
    for r in rows:
        if prev is not None and r["seq"] <= prev:
            runs.append(cur)
            cur = []
        cur.append(r)
        prev = r["seq"]
    if cur:
        runs.append(cur)
    return runs


def analyse(rows: list[dict]) -> str:
    if len(rows) < 3:
        return f"{len(rows)} samples -- not enough for either result yet."

    raw_n = len(rows)
    off_stratum = sum(1 for r in rows if r["stratum"] != 1)
    # One boot only. Which one: the longest, because that is the run with the
    # most to say, and a short tail after a restart should not displace hours
    # of data.
    # One sender first. A segment can carry more than one board with the same
    # persona, and merging them is not a degraded analysis but a meaningless
    # one -- t_s and seq belong to whichever board emitted them.
    srcs = {r["src"] for r in rows if r["src"]}
    src_note = ""
    if len(srcs) > 1:
        best = max(srcs, key=lambda a: sum(
            1 for r in rows if r["src"] == a and r.get("pps_us") is not None))
        dropped = sum(1 for r in rows if r["src"] != best)
        rows = [r for r in rows if r["src"] == best]
        src_note = (f"  ({len(srcs)} boards broadcasting on this port; analysing "
                    f"{best}, ignoring {dropped} lines from the others)")
    runs = split_runs(rows)
    note = ""
    if len(runs) > 1:
        rows = max(runs, key=len)
        note = (f"  ({len(runs)} runs in this log -- the board restarted. "
                f"Analysing the longest, {len(rows)} samples;\n"
                f"   figures derived from t_s cannot span a reboot.)")
    # Everything measured against the local GPS is computed from *all* of this
    # run's samples. The filters below concern the remote NTP reference -- its
    # stratum, its round trip -- and a sample taken while that server was
    # degraded is still a perfectly good PPS measurement, because the PPS path
    # never consults it. Filtering first threw those away for no reason (found
    # 2026-09-03: the reference spent 14 minutes off its own GPS, and 14 local
    # measurements went with it).
    local_rows = rows
    rows, slow = split_by_delay(rows)
    out = [f"{len(rows)} of {raw_n} samples over {(rows[-1]['t_s'] - rows[0]['t_s']) / 3600:.2f} h"]
    if off_stratum:
        out.append(f"  ({off_stratum} taken while the reference was not stratum 1 -- "
                   f"excluded; a degraded reference biases every figure below)")
    if slow:
        rtts = sorted(r["rtt_ms"] for r in rows)
        out.append(f"  ({len(slow)} set aside: round trip over "
                   f"{max(3 * rtts[len(rtts) // 2], 10)} ms, so the offset is "
                   f"worth less than the thing being measured)")

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
    if src_note:
        out.append(src_note)
    if note:
        out.append(note)
    out.append(f"  ntp rtt      : min {rtts[0]} / median {rtts[len(rtts) // 2]} / max {rtts[-1]} ms")

    # --- the DCF path.
    d = [r["dcf_ms"] for r in rows if r["dcf_ms"] is not None]
    if not d:
        out.append("  dcf          : no frames accepted yet")
        out.append("                 (frames column stays 0 until reception is good enough --\n                  a frame needs 59 consecutive clean seconds)")
    else:
        mean = sum(d) / len(d)
        sd = (sum((x - mean) ** 2 for x in d) / len(d)) ** 0.5
        ordered = sorted(d)
        out.append(f"  dcf offset   : mean {mean:+.1f} ms, sd {sd:.1f} ms, n={len(d)}")

    # P4: the same delay measured against the GPS pulse instead of the network.
    # Reported beside the NTP figure on purpose -- two independent routes to
    # one quantity, where a disagreement bigger than the two uncertainties
    # means one of them is wrong.
    pv = [r["pps_us"] for r in local_rows if r.get("pps_us") is not None]
    if not pv:
        out.append("  dcf vs pps   : no GPS-referenced samples")
    else:
        pm = sum(pv) / len(pv)
        psd = (sum((x - pm) ** 2 for x in pv) / len(pv)) ** 0.5
        sem = psd / (len(pv) ** 0.5) if len(pv) > 1 else 0.0
        out.append(f"  dcf vs pps   : mean {pm / 1000:+.3f} ms, sd {psd / 1000:.3f} ms, "
                   f"n={len(pv)} (local reference; no stratum or rtt filter)")
        # Deliberately not named `ordered`: that name belongs to the DCF
        # block above and is still read after this one, so reusing it here
        # printed microseconds under a millisecond label.
        pps_sorted = sorted(pv)
        med = pps_sorted[len(pps_sorted) // 2]
        out.append(f"  -> delay     : mean {pm:+.0f} +/- {sem:.0f} us, "
                   f"median {med:+d} us")
        out.append(f"  -> quartiles : {pps_sorted[len(pps_sorted) // 4]} / {med} / "
                   f"{pps_sorted[3 * len(pps_sorted) // 4]} us")
        # Mean, once there is enough data to see the shape. On a few dozen
        # samples this looked late-tailed and the median was the safer choice;
        # across 760 it is symmetric -- 5681 us below the median against 5752
        # above, 2537/2864 at the deciles -- so the apparent tail was a
        # small-sample artefact and the mean is simply the lower-variance
        # estimator. Reported with its standard error, because a constant
        # without one is an opinion.
        skew = abs((pm - med) / psd) if psd else 0.0
        pick = round(pm) if skew < 0.15 else med
        out.append(f"  -> CONFIG_DCF77_DELAY_US = {pick} "
                   f"({'mean' if pick != med else 'median'}, "
                   f"+/- {sem:.0f} us)")
        if d:
            # The NTP route measures the radio's error against true time; the
            # PPS route measures the same lateness directly. They should agree
            # in magnitude and oppose in sign.
            diff = pm / 1000 + (sum(d) / len(d))
            out.append(f"  -> agreement : {diff:+.3f} ms between the two methods")
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
                # With the sender's address, because two boards broadcasting
                # to the same port produced 1098 lines that looked like one
                # series with 283 reboots in it (2026-09-04). They were an
                # 18-hour run interleaved with a second board restarting every
                # few minutes, and nothing in the wire format could tell them
                # apart -- both carry the same persona string.
                log.write(f"{addr[0]} {line}\n")
                rows.append(r)
                dcf = f"{r['dcf_ms']:+d} ms" if r["dcf_ms"] is not None else "-"
                # P4 beside it on every line, in milliseconds so the two are
                # directly comparable at a glance. They measure the same
                # lateness by independent routes and should agree in magnitude
                # while opposing in sign -- seeing that hold sample by sample is
                # worth more than discovering it in the summary at the end.
                pps = f"{r['pps_us'] / 1000:+.1f} ms" if r.get("pps_us") is not None else "-"
                gap = ""
                if len(rows) > 1 and r["seq"] != rows[-2]["seq"] + 1:
                    # Broadcast is fire-and-forget by design, so a gap is
                    # expected rather than alarming -- but it should be
                    # visible, because the board's own accumulators counted
                    # the sample this listener did not hear.
                    gap = f"  (seq {rows[-2]['seq'] + 1}..{r['seq'] - 1} not heard)"
                # Stratum on every line, not only in the summary. The
                # reference losing its GPS is a failure that keeps answering
                # and looks healthy -- it went 1 -> 3 -> 5 -> 7 -> 9 -> 11
                # through one overnight run here and nothing said so until the
                # log was graded afterwards. A watcher should see it happen.
                st = f"st={r['stratum']}" if r["stratum"] == 1 else f"st={r['stratum']} <-- NOT stratum 1"
                print(f"[{time.strftime('%H:%M:%S')}] {addr[0]:>15}  seq={r['seq']:<5} "
                      f"t={r['t_s']:<6} off={r['off_ms']:+5d} rtt={r['rtt_ms']:<4} {st:<6} "
                      f"dcf={dcf:<10} pps={pps:<10} q={r['q'] / 10:.1f} "
                      f"frames={r['frames']}{gap}")
                if len(rows) % args.every == 0:
                    print("\n" + analyse(rows) + "\n")
    except KeyboardInterrupt:
        print("\n\n" + analyse(rows))
        return 0
    finally:
        log.close()


if __name__ == "__main__":
    sys.exit(main())
