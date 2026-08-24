"""`lugal9pfuse` -- mounts a LugalOS board's entire 9P namespace (/sd0,
/proc, /ram0, ... -- whatever `(help)`/`ls /` shows on the board itself) as
a real directory on the host, via FUSE. Built on host/p9lib's existing
Session/P9Client rather than a new protocol implementation; see
operations.py's docstring for what it can and can't do.

Runs in the foreground (blocks until unmounted); stop it with Ctrl-C or
`fusermount -u <mountpoint>` from another shell.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from fuse import FUSE

from p9lib import Session, connect_serial, connect_tcp, connect_unix

from .operations import P9FS


def _auth_key(args) -> bytes | None:
    """The key, from --key or --key-file, or None for an unauthenticated
    attach. --key-file exists because a secret on a command line is visible in
    shell history and in every process listing for as long as the mount
    runs -- which for a FUSE mount is "until it is unmounted"."""
    if getattr(args, "key_file", None):
        text = Path(args.key_file).read_text().strip()
    elif getattr(args, "key", None):
        text = args.key.strip()
    else:
        return None
    try:
        return bytes.fromhex(text)
    except ValueError:
        print(f"error: key is not valid hex: {text[:16]!r}...", file=sys.stderr)
        sys.exit(2)


def _split_hostport(spec: str, default_port: int = 564) -> tuple[str, int]:
    if ":" in spec:
        host, _, port = spec.rpartition(":")
        return host, int(port)
    return spec, default_port


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="lugal9pfuse", description=__doc__)
    p.add_argument("--serial", metavar="PORT", help="serial port (e.g. /dev/ttyACM1)")
    p.add_argument("--unix", metavar="PATH", help="unix socket path (QEMU virtio-console chardev)")
    p.add_argument("--baud", type=int, default=115200, help="serial baud rate (default 115200)")
    p.add_argument("--framing", choices=("raw", "slip"), default="raw",
                    help="'raw' for virtio-console/USB-CDC (default), 'slip' for a UART link")
    p.add_argument("--aname", default="/", help="attach root (default '/')")
    p.add_argument("--tcp", metavar="HOST[:PORT]",
                   help="9P over TCP, e.g. 192.168.1.50 or 192.168.1.50:564 "
                        "(the gateway persona's W5500 link)")
    p.add_argument("--key", metavar="HEX",
                   help="pre-shared key, hex, for a link that requires authentication")
    p.add_argument("--key-file", metavar="PATH",
                   help="read the hex key from a file instead of the command line, "
                        "so it does not land in shell history or `ps` output")
    p.add_argument("--uname", default="lugal",
                   help="identity to authenticate as (default 'lugal')")

    p.add_argument("--timeout", type=float, default=30.0,
                    help="serial read timeout in seconds (default 30.0 -- /proc/df's real "
                         "FAT-table scan alone took 7-13s across two different SD cards in "
                         "testing, so this needs real margin over any one measurement, not "
                         "just enough for an ordinary request; raise it further for a bigger "
                         "card)")
    p.add_argument("--allow-other", action="store_true",
                    help="allow other host users to access the mount (needs user_allow_other in /etc/fuse.conf)")
    p.add_argument("mountpoint", help="an existing, empty local directory to mount onto")
    args = p.parse_args(argv)

    if args.serial:
        client = connect_serial(args.serial, baudrate=args.baud, framing=args.framing,
                                 timeout=args.timeout)
    elif args.unix:
        client = connect_unix(args.unix, framing=args.framing)
    elif args.tcp:
        host, port = _split_hostport(args.tcp)
        client = connect_tcp(host, port, timeout=args.timeout)
    else:
        print("error: one of --serial, --unix or --tcp is required", file=sys.stderr)
        return 2

    session = Session(client, aname=args.aname, uname=args.uname, key=_auth_key(args))
    # Multi-threaded (fusepy's default -- no nothreads=True): P9FS's own
    # lock is what keeps the single Session safe now, not single-threaded
    # dispatch. See operations.py's docstring for why nothreads=True was
    # actually a stability bug, not just a missed optimization: a slow but
    # legitimate real-hardware operation (e.g. /proc/df) blocked libfuse's
    # entire request queue for its whole duration, degrading everything
    # else using the mount at the same time.
    fuse_kwargs = {"foreground": True}
    if args.allow_other:
        fuse_kwargs["allow_other"] = True
    FUSE(P9FS(session), args.mountpoint, **fuse_kwargs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
