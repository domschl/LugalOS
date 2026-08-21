"""`lugal9p` -- a small command-line file utility for LugalOS's 9P server,
the actual "product" surface of p9lib (phase14a,
plan/phase14_networking_and_host_tooling.md): ls/cat/get/put/rm/mkdir/stat
against any LugalOS board reachable over a serial port or (for QEMU-based
development) a unix socket, without writing any Python.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import Session, connect_serial, connect_unix
from .client import P9Error


def _connect(args: argparse.Namespace) -> Session:
    if args.serial:
        client = connect_serial(args.serial, baudrate=args.baud, framing=args.framing,
                                 timeout=args.timeout)
    elif args.unix:
        client = connect_unix(args.unix, framing=args.framing)
    else:
        print("error: one of --serial or --unix is required", file=sys.stderr)
        sys.exit(2)
    return Session(client, aname=args.aname)


def _fmt_mode(st) -> str:
    return "d" if st.is_dir else "-"


def cmd_ls(args: argparse.Namespace) -> int:
    with _connect(args) as sess:
        entries = sess.listdir(args.path)
        for e in sorted(entries, key=lambda e: e.name):
            size = "" if e.is_dir else str(e.length)
            print(f"{_fmt_mode(e)} {size:>10}  {e.name}")
    return 0


def cmd_stat(args: argparse.Namespace) -> int:
    with _connect(args) as sess:
        st = sess.stat(args.path)
        print(f"name:   {st.name}")
        print(f"type:   {'directory' if st.is_dir else 'file'}")
        print(f"length: {st.length}")
        print(f"mode:   {oct(st.mode)}")
        print(f"qid:    type={st.qid.type} vers={st.qid.vers} path={st.qid.path}")
    return 0


def cmd_cat(args: argparse.Namespace) -> int:
    with _connect(args) as sess:
        data = sess.read(args.path)
        sys.stdout.buffer.write(data)
    return 0


def cmd_get(args: argparse.Namespace) -> int:
    with _connect(args) as sess:
        data = sess.read(args.remote_path)
    dest = Path(args.local_path)
    dest.write_bytes(data)
    print(f"{args.remote_path} -> {dest} ({len(data)} bytes)", file=sys.stderr)
    return 0


def cmd_put(args: argparse.Namespace) -> int:
    data = Path(args.local_path).read_bytes()
    with _connect(args) as sess:
        n = sess.write(args.remote_path, data)
    print(f"{args.local_path} -> {args.remote_path} ({n} bytes)", file=sys.stderr)
    return 0


def cmd_rm(args: argparse.Namespace) -> int:
    with _connect(args) as sess:
        sess.remove(args.path)
    return 0


def cmd_mkdir(args: argparse.Namespace) -> int:
    with _connect(args) as sess:
        sess.mkdir(args.path)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="lugal9p", description=__doc__)
    p.add_argument("--serial", metavar="PORT", help="serial port (e.g. /dev/ttyACM1)")
    p.add_argument("--unix", metavar="PATH", help="unix socket path (QEMU virtio-console chardev)")
    p.add_argument("--baud", type=int, default=115200, help="serial baud rate (default 115200)")
    p.add_argument("--framing", choices=("raw", "slip"), default="raw",
                    help="'raw' for virtio-console/USB-CDC (default), 'slip' for a UART link")
    p.add_argument("--aname", default="/", help="attach root (default '/')")
    p.add_argument("--timeout", type=float, default=30.0,
                    help="serial read timeout in seconds (default 30.0 -- /proc/df's real "
                         "FAT-table scan alone took 7-13s across two different SD cards in "
                         "testing, so this needs real margin over any one measurement, not "
                         "just enough for an ordinary request; raise it further for a bigger "
                         "card)")

    sub = p.add_subparsers(dest="command", required=True)

    sp = sub.add_parser("ls", help="list a directory")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_ls)

    sp = sub.add_parser("stat", help="show a file or directory's metadata")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_stat)

    sp = sub.add_parser("cat", help="print a file's content to stdout")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_cat)

    sp = sub.add_parser("get", help="download a file to the local machine")
    sp.add_argument("remote_path")
    sp.add_argument("local_path")
    sp.set_defaults(func=cmd_get)

    sp = sub.add_parser("put", help="upload a local file, creating or overwriting it")
    sp.add_argument("local_path")
    sp.add_argument("remote_path")
    sp.set_defaults(func=cmd_put)

    sp = sub.add_parser("rm", help="remove a file or (empty) directory")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_rm)

    sp = sub.add_parser("mkdir", help="create a directory")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_mkdir)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except P9Error as e:
        print(f"lugal9p: {e}", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"lugal9p: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
