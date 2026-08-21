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

from fuse import FUSE

from p9lib import Session, connect_serial, connect_unix

from .operations import P9FS


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="lugal9pfuse", description=__doc__)
    p.add_argument("--serial", metavar="PORT", help="serial port (e.g. /dev/ttyACM1)")
    p.add_argument("--unix", metavar="PATH", help="unix socket path (QEMU virtio-console chardev)")
    p.add_argument("--baud", type=int, default=115200, help="serial baud rate (default 115200)")
    p.add_argument("--framing", choices=("raw", "slip"), default="raw",
                    help="'raw' for virtio-console/USB-CDC (default), 'slip' for a UART link")
    p.add_argument("--aname", default="/", help="attach root (default '/')")
    p.add_argument("--allow-other", action="store_true",
                    help="allow other host users to access the mount (needs user_allow_other in /etc/fuse.conf)")
    p.add_argument("mountpoint", help="an existing, empty local directory to mount onto")
    args = p.parse_args(argv)

    if args.serial:
        client = connect_serial(args.serial, baudrate=args.baud, framing=args.framing)
    elif args.unix:
        client = connect_unix(args.unix, framing=args.framing)
    else:
        print("error: one of --serial or --unix is required", file=sys.stderr)
        return 2

    session = Session(client, aname=args.aname)
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
