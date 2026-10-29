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

    def __init__(self, ser, closer=None) -> None:
        self._s = ser
        self._closer = closer       # how the port is let go of, if not plainly

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
        if self._closer:
            self._closer(self._s)
        else:
            self._s.close()


def connect_unix(path: str, timeout: float = 5.0, framing: str = "raw") -> P9Client:
    """QEMU's virtio-console chardev, or any other unix-domain 9P socket --
    see tests/runner.py's `-chardev socket,...` invocations for how a QEMU
    target exposes one."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(path)
    return P9Client(s, framing=framing)


def connect_tcp(host: str, port: int = 564, timeout: float = 30.0, protect: bool = True) -> P9Client:
    """A 9P server reachable over TCP -- the gateway persona's Ethernet link
    (plan/phase18_networking_and_auth.md), on 564, the port IANA assigns to
    the Plan 9 file service.

    Also how the TCP path is tested with no hardware at all: QEMU's chardev
    backend takes `socket,host=...,port=...,server=on` in place of the unix
    socket tests/runner.py normally uses, so the guest keeps seeing an
    ordinary virtio-console byte stream while this end is a real TCP socket.
    Phase 19's R3 replaces that arrangement with a guest that terminates the
    TCP itself; nothing on this side changes when it does.

    9P frames are length-prefixed, so the stream needs no further framing:
    `raw`, the same as the unix-socket and USB-CDC paths."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    return P9Client(s, framing="raw", offer_lp=protect)


class WebSocketAdapter:
    """A WebSocket (RFC 6455, client side) as the socket-like object P9Client expects: each
    sendall() is one masked binary frame holding one whole 9P message, and recv() hands back the
    bytes of the binary messages that arrive, however the peer cut them into frames.

    Control frames are answered here (ping -> pong) and a close frame reads as end of stream."""

    def __init__(self, sock: socket.socket, leftover: bytes = b"") -> None:
        self._s = sock
        self._raw = leftover        # bytes off the wire, not yet parsed into frames
        self._data = b""            # payload bytes ready for the caller
        self._closed = False

    def _need(self, n: int) -> bool:
        while len(self._raw) < n:
            chunk = self._s.recv(65536)
            if not chunk:
                return False
            self._raw += chunk
        return True

    def _frame(self) -> bool:
        """Reads one frame; False at end of stream."""
        if not self._need(2):
            return False
        b0, b1 = self._raw[0], self._raw[1]
        n, off = b1 & 0x7F, 2
        if n == 126:
            if not self._need(4):
                return False
            n, off = int.from_bytes(self._raw[2:4], "big"), 4
        elif n == 127:
            if not self._need(10):
                return False
            n, off = int.from_bytes(self._raw[2:10], "big"), 10
        masked = bool(b1 & 0x80)
        if masked:
            off += 4
        if not self._need(off + n):
            return False
        payload = self._raw[off:off + n]
        if masked:
            mask = self._raw[off - 4:off]
            payload = bytes(c ^ mask[i & 3] for i, c in enumerate(payload))
        self._raw = self._raw[off + n:]
        op = b0 & 0x0F
        if op in (0x0, 0x1, 0x2):
            self._data += payload
        elif op == 0x9:
            self._send(0xA, payload)
        elif op == 0x8:
            self._closed = True
            return False
        return True

    def _send(self, op: int, payload: bytes) -> None:
        import os
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n <= 0xFFFF:
            hdr += bytes([0x80 | 126]) + n.to_bytes(2, "big")
        else:
            hdr += bytes([0x80 | 127]) + n.to_bytes(8, "big")
        body = bytes(c ^ mask[i & 3] for i, c in enumerate(payload))
        self._s.sendall(hdr + mask + body)

    def sendall(self, data: bytes) -> None:
        self._send(0x2, bytes(data))

    def recv(self, n: int) -> bytes:
        while not self._data:
            if self._closed or not self._frame():
                return b""
        out, self._data = self._data[:n], self._data[n:]
        return out

    def settimeout(self, t: float) -> None:
        self._s.settimeout(t)

    def close(self) -> None:
        try:
            if not self._closed:
                self._send(0x8, (1000).to_bytes(2, "big"))
        except OSError:
            pass
        self._s.close()


def connect_ws(host: str, port: int = 80, path: str = "/9p", timeout: float = 30.0, protect: bool = True) -> P9Client:
    """9P over a WebSocket: the admin web server's transport (net/httpd.c). The same server, the
    same Tauth/Tattach, the same P9Client; only the carrier differs."""
    import base64
    import hashlib
    import os

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((host, port))
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: 9p\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise P9Error(f"{host}:{port} closed during the WebSocket handshake")
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    status = head.split(b"\r\n", 1)[0].decode(errors="replace")
    if " 101 " not in status:
        s.close()
        raise P9Error(f"WebSocket upgrade refused: {status}")
    want = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    if f"sec-websocket-accept: {want}".lower() not in head.decode(errors="replace").lower():
        s.close()
        raise P9Error("WebSocket upgrade answered with the wrong Sec-WebSocket-Accept")
    return P9Client(WebSocketAdapter(s, rest), framing="raw", offer_lp=protect)


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


def connect_open_serial(ser, framing: str = "raw", timeout: float = 30.0,
                        warm_up: bool = True, closer=None) -> P9Client:
    """A serial port the caller has already opened, and the caller's way of
    closing it. For boards where *how* a port is opened and closed matters:
    the ESP32-P4 LCD-7B resets on a plain pyserial open (RTS is its EN) and
    on the next open after a plain close, so tools/p4run.py's open_console()
    and close_console() are passed in here (47.8). Nothing about a board is
    assumed in this library."""
    ser.timeout = timeout
    ser.reset_input_buffer()
    adapter = SerialSocketAdapter(ser, closer=closer)
    client = P9Client(adapter, framing=framing)
    if warm_up and not warm_up_9p(client, adapter, settle_timeout=timeout):
        raise P9Error(f"no Rversion from {getattr(ser, 'port', ser)} after warm-up retries")
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
