"""p9lib -- a real, general-purpose 9P2000 client for LugalOS's filesystem
server (fs/9p.c), from a host machine. See client.py for the wire protocol
and the path-based `Session` convenience layer, and transport.py for how to
actually reach a board (serial port or unix socket).

Quick start, real hardware (RP2350's USB-CDC ACM1 "net" port)::

    import p9lib
    client = p9lib.connect_serial("/dev/ttyACM1")
    with p9lib.Session(client, aname="/") as sess:
        print(sess.listdir("/sd0"))
        sess.write("/sd0/hello.txt", b"hello from a host\\n")
        print(sess.read("/sd0/hello.txt"))
"""

from .client import (
    DMDIR,
    MAX_FIDS,
    MAX_NAME_LEN,
    MAX_WALK_ELEM,
    OEXEC,
    OREAD,
    ORCLOSE,
    ORDWR,
    OTRUNC,
    OWRITE,
    P9Client,
    P9Error,
    Qid,
    Session,
    Stat,
    slip_decode,
    slip_encode,
)
from .transport import SerialSocketAdapter, connect_serial, connect_tcp, connect_unix, warm_up_9p

__all__ = [
    "P9Client",
    "P9Error",
    "Session",
    "Qid",
    "Stat",
    "OREAD",
    "OWRITE",
    "ORDWR",
    "OEXEC",
    "OTRUNC",
    "ORCLOSE",
    "DMDIR",
    "MAX_FIDS",
    "MAX_WALK_ELEM",
    "MAX_NAME_LEN",
    "slip_encode",
    "slip_decode",
    "connect_unix",
    "connect_serial",
    "connect_tcp",
    "warm_up_9p",
    "SerialSocketAdapter",
]

__version__ = "0.1.0"
