#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""One 9P path to a file, measured: write and read throughput of a file, and
the round-trip latency of a small read. 49.9 (plan/phase49_c6_network_nodes.md)
runs it against the same file on a P4 by two routes -- frames through its C6 to
the P4's own server, and the C6's server re-exporting the P4 at /p4 -- and
against a stand-alone C6-Zero for the baseline.

    uv run tests/hw/p9_paths.py --tcp 192.168.178.160 --keyfile K \\
        --file /sd0/bench.bin --size 262144 --small /proc/version -n 5

The key is the node's (`p9key HEX` at its console sets one for that boot).
"""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "host" / "p9lib" / "src"))
import p9lib  # noqa: E402
from p9lib.client import OREAD, OWRITE  # noqa: E402


def open_fid(sess: p9lib.Session, path: str, mode: int) -> int:
    fid = sess._alloc_work_fid()
    sess._walk_or_raise(fid, sess._split(path), path, "walk to")
    sess.client.open(fid, mode=mode)
    return fid


def write_file(sess: p9lib.Session, path: str, data: bytes) -> float:
    """Seconds to write `data` at `path` (created or truncated first, untimed)."""
    sess.write(path, b"", create=True, truncate=True)
    fid = open_fid(sess, path, OWRITE)
    c, chunk = sess.client, sess.client.io_chunk()
    t0 = time.monotonic()
    try:
        off = 0
        while off < len(data):
            off += c.write(fid, off, data[off:off + chunk])
    finally:
        c.clunk(fid)
    return time.monotonic() - t0


def read_file(sess: p9lib.Session, path: str) -> tuple[bytes, float]:
    fid = open_fid(sess, path, OREAD)
    c, chunk = sess.client, sess.client.io_chunk()
    t0 = time.monotonic()
    out = bytearray()
    try:
        while True:
            d = c.read(fid, len(out), chunk)
            if not d:
                break
            out += d
    finally:
        c.clunk(fid)
    return bytes(out), time.monotonic() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tcp", required=True, metavar="HOST[:PORT]")
    ap.add_argument("--keyfile", required=True)
    ap.add_argument("--file", help="the file to write, then read back")
    ap.add_argument("--size", type=int, default=262144)
    ap.add_argument("--small", default="/proc/version", help="a small file for the round trip")
    ap.add_argument("-n", type=int, default=5, help="repetitions")
    ap.add_argument("--label", default="")
    a = ap.parse_args()

    host, _, port = a.tcp.partition(":")
    key = bytes.fromhex(Path(a.keyfile).read_text().split()[0])
    sess = p9lib.Session(p9lib.connect_tcp(host, int(port or 564), timeout=60.0), aname="/", key=key)
    print(f"{a.label or a.tcp}: msize io chunk {sess.client.io_chunk()} B")
    try:
        lat = []
        for _ in range(max(a.n * 4, 20)):
            t0 = time.monotonic()
            sess.read(a.small)
            lat.append((time.monotonic() - t0) * 1000)
        lat.sort()
        print(f"  small read {a.small}: median {statistics.median(lat):.1f} ms, "
              f"p90 {lat[int(len(lat) * 0.9) - 1]:.1f} ms, min {lat[0]:.1f} ms ({len(lat)} reads)")
        if a.file:
            data = os.urandom(a.size)
            w = [a.size / 1024 / write_file(sess, a.file, data) for _ in range(a.n)]
            r = []
            for _ in range(a.n):
                got, dt = read_file(sess, a.file)
                if got != data:
                    print(f"  read back {len(got)} bytes, not what was written")
                    return 1
                r.append(a.size / 1024 / dt)
            print(f"  write {a.size // 1024} KB: median {statistics.median(w):.1f} KB/s "
                  f"(min {min(w):.1f}, max {max(w):.1f}, n={a.n})")
            print(f"  read  {a.size // 1024} KB: median {statistics.median(r):.1f} KB/s "
                  f"(min {min(r):.1f}, max {max(r):.1f}, n={a.n}), identical every time")
    finally:
        sess.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
