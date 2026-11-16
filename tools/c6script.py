#!/usr/bin/env python3
"""Store a Lisp program in a LugalOS node's EEPROM, to run at boot (`script` in the shell manual).

    tools/c6script.py demo.lisp [--board zero | --port PORT] [--run]
    tools/c6script.py --show | --clear [--board zero]

The text is sent as hex over the console (`script begin`, `script put ...`, `script commit`), so
nothing but the serial port is needed; the node verifies a CRC-32 over what the EEPROM holds. The
port is opened the way tools/c6run.py does, which keeps the chip out of reset. At most 3828 bytes of
printable ASCII text.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c6run  # noqa: E402

MAX = 0x0F00 - 12
CHUNK = 48       # bytes per `script put` line


def run(port, cmds, wait):
    return c6run.listen(port, 1.0, cmds, wait)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file", nargs="?", help="the Lisp source to store")
    ap.add_argument("--port")
    ap.add_argument("--board", help="zero, nano, ... -- a port from ~/.config/lugalos/c6-ports.env")
    ap.add_argument("--run", action="store_true", help="run it once after storing it")
    ap.add_argument("--show", action="store_true", help="print the stored script")
    ap.add_argument("--clear", action="store_true", help="remove the stored script")
    a = ap.parse_args()
    port = c6run.find_port(a.port, a.board)

    if a.show:
        print(run(port, ["script", "script show"], 1.0))
        return 0
    if a.clear:
        print(run(port, ["script clear", "script"], 1.0))
        return 0
    if not a.file:
        ap.error("give a Lisp file, or --show / --clear")
    data = open(a.file, "rb").read().replace(b"\r\n", b"\n")
    if not data.strip():
        sys.exit("%s is empty" % a.file)
    if len(data) > MAX:
        sys.exit("%s is %d bytes; the EEPROM holds %d" % (a.file, len(data), MAX))
    bad = [i for i, b in enumerate(data) if not (0x20 <= b < 0x7F or b in (9, 10, 13))]
    if bad:
        sys.exit("%s has a byte that is not plain ASCII text at offset %d" % (a.file, bad[0]))

    cmds = ["script begin"]
    cmds += ["script put " + data[i:i + CHUNK].hex() for i in range(0, len(data), CHUNK)]
    cmds += ["script commit", "script"]
    # Each put is a page write of up to 48 bytes (5 ms a page) plus the console round trip.
    out = run(port, cmds, 0.35)
    tail = [l for l in out.splitlines() if l.startswith("script:")]
    print("\n".join(tail))
    if not any("committed" in l for l in tail):
        print("not committed: see the output above", file=sys.stderr)
        print(out[-1500:], file=sys.stderr)
        return 1
    if a.run:
        print(run(port, ["script run"], 3.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
