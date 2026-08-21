"""Connection helpers for reaching a real LugalOS 9P endpoint: a serial
port (RP2350's USB-CDC ACM1 "net" port, or a UART/SLIP link) or a unix
socket (QEMU's virtio-console chardev, used for development/testing without
real hardware). Split out from client.py since only real-hardware use needs
pyserial at all -- client.py itself has no dependency beyond the standard
library.
"""

from __future__ import annotations

import socket
import time

from .client import P9Client, P9Error


class SerialSocketAdapter:
    """Adapts a pyserial `Serial` to the minimal socket-like interface
    P9Client expects (sendall/recv/settimeout/close).

    recv(n) honors the real socket.recv() contract of "at most n bytes,
    possibly fewer" -- P9Client's _recv_exact() buffers by looping until it
    has exactly what it asked for, which silently breaks (a struct.unpack
    on a too-long buffer) if a naive adapter over-delivers past `n`."""

    def __init__(self, ser) -> None:
        self._s = ser

    def sendall(self, data: bytes) -> None:
        self._s.write(data)
        self._s.flush()

    def recv(self, n: int) -> bytes:
        data = self._s.read(1)
        if not data:
            return b""
        extra = min(self._s.in_waiting, n - 1)
        if extra > 0:
            data += self._s.read(extra)
        return data

    def settimeout(self, t: float) -> None:
        self._s.timeout = t

    def close(self) -> None:
        self._s.close()


def connect_unix(path: str, timeout: float = 5.0, framing: str = "raw") -> P9Client:
    """QEMU's virtio-console chardev, or any other unix-domain 9P socket --
    see tests/runner.py's `-chardev socket,...` invocations for how a QEMU
    target exposes one."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    return P9Client(s, framing=framing)


def connect_serial(port: str, baudrate: int = 115200, timeout: float = 30.0,
                    framing: str = "raw", warm_up: bool = True) -> P9Client:
    """A real serial port -- RP2350's USB-CDC ACM1 "net" port (framing=
    "raw") or a UART/SLIP link (framing="slip"). Requires pyserial, only
    imported here so client.py itself stays dependency-free.

    `warm_up=True` (the default) retries an initial Tversion a few times
    before returning -- see warm_up_9p()'s own docstring for why a freshly
    (re)opened CDC-ACM port can silently drop its first frame. `timeout`
    is used both for that initial connection and, via warm_up_9p's
    settle_timeout, for every request afterward -- passing settle_timeout=
    5.0 unconditionally here (warm_up_9p's own default) used to silently
    discard whatever timeout the caller asked for the moment warm-up
    finished.

    The default was 5.0 until real hardware showed it was too short for a
    genuinely slow, legitimate operation: /proc/df's full FAT-table free-
    space scan (fs/fat32.c's fat32_statfs(), across every mounted FAT32
    volume) made stat()/read() on it fail outright (visibly, as a
    `?????` row in `ls -la /proc`) rather than just being slow --
    indistinguishable, from the client's own perspective, from a real
    hang like the /dev/uart or /dev/eeprom ones fuse-p9 refuses outright.
    How slow depends directly on card size, not just a fixed cost: ~7s on
    one test board's /sd0, ~13s on the same code against a larger card a
    day later -- so this needs real headroom above any single
    measurement, not just enough for the one card this was tested
    against. 30s leaves that headroom without making an actual hang
    (there is no way to tell the two apart in advance) take dramatically
    longer to report as a clean error; pass a larger `timeout` explicitly
    if an even bigger card needs more."""
    import serial  # local import: only real-hardware callers need this

    ser = serial.Serial(port, baudrate, timeout=timeout)
    ser.dtr = True
    time.sleep(0.3)
    ser.reset_input_buffer()
    adapter = SerialSocketAdapter(ser)
    client = P9Client(adapter, framing=framing)
    if warm_up and not warm_up_9p(client, adapter, settle_timeout=timeout):
        raise P9Error(f"no Rversion from {port} after warm-up retries")
    return client


def warm_up_9p(client: P9Client, adapter: SerialSocketAdapter,
               attempts: int = 5, attempt_timeout: float = 1.0,
               settle_timeout: float = 5.0) -> bool:
    """The first 9P frame sent to a freshly (re)opened CDC-ACM port
    occasionally gets silently dropped by the device -- observed testing
    against real RP2350 hardware, most likely a DATA0/DATA1 toggle
    assumption mismatch between the host driver's fresh open() and the
    device's own toggle state, which has persisted since USB enumeration
    and has no protocol-level reason to resync just because a *host-side*
    file descriptor closed and reopened (no bus reset occurs). Tversion is
    cheap and idempotent (it resets the server's fid table either way, by
    design -- see fs/9p.c), so retrying it a few times with a short timeout
    is a safe, low-risk way past a dropped first frame.

    Returns True once a real Rversion comes back; leaves `adapter`'s
    timeout at `settle_timeout` for the caller's actual work either way."""
    for _ in range(attempts):
        adapter.settimeout(attempt_timeout)
        try:
            client.version()
            adapter.settimeout(settle_timeout)
            return True
        except P9Error:
            continue
    adapter.settimeout(settle_timeout)
    return False
