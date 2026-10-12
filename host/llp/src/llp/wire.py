"""LLP framing and the serial binding (doc/llp.md §3, §7.1).

The same bytes as kernel/llp.c's llp_hdr_put(), llp_frame_parse(),
llp_serial_encode() and llp_serial_feed(), and drivers/slip.c underneath.
CRC-32 is zlib's, which is kernel/crc32.c's construction exactly.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

VERSION_MAJOR = 1
VERSION_MINOR = 0

HDR_LEN = 4
CRC_LEN = 4
MAX_PAYLOAD = 4096
MIN_PAYLOAD = 1514
MAX_WINDOW = 64

T_LINK = 0x01
T_ETH = 0x02
T_P9 = 0x03

OP_HELLO = 0x01
OP_CREDIT = 0x02
OP_PROBE = 0x03
OP_STATE = 0x04

HELLO_LEN = 22
CREDIT_LEN = 3
PROBE_LEN = 2
STATE_LEN = 5

HELLO_F_UP = 0x01
HELLO_F_FLOW = 0x02

ROLE_HOST = 1
ROLE_RADIO = 2

SLIP_END = 0xC0
SLIP_ESC = 0xDB
SLIP_ESC_END = 0xDC
SLIP_ESC_ESC = 0xDD


@dataclass(frozen=True)
class Frame:
    type: int
    seq: int
    payload: bytes

    def to_bytes(self) -> bytes:
        """Header + payload: what a binding transmits (before its own framing)."""
        return header(self.type, self.seq, len(self.payload)) + self.payload


def header(type_: int, seq: int, length: int) -> bytes:
    return struct.pack("<BBH", type_, seq & 0xFF, length)


class FrameError(ValueError):
    pass


def parse(buf: bytes) -> Frame:
    """llp_frame_parse(): exactly one header + payload, no CRC."""
    if len(buf) < HDR_LEN:
        raise FrameError("short")
    type_, seq, length = struct.unpack_from("<BBH", buf)
    if length != len(buf) - HDR_LEN:
        raise FrameError("len")
    return Frame(type_, seq, buf[HDR_LEN:])


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def slip_escape(data: bytes) -> bytes:
    out = bytearray()
    for c in data:
        if c == SLIP_END:
            out += bytes((SLIP_ESC, SLIP_ESC_END))
        elif c == SLIP_ESC:
            out += bytes((SLIP_ESC, SLIP_ESC_ESC))
        else:
            out.append(c)
    return bytes(out)


def serial_encode(frame: Frame) -> bytes:
    """llp_serial_encode(): SLIP(header | payload | CRC-32 LE)."""
    body = frame.to_bytes()
    return bytes((SLIP_END,)) + slip_escape(body + struct.pack("<I", crc32(body))) + bytes((SLIP_END,))


class SerialRx:
    """llp_serial_rx_t + llp_serial_feed(): bytes in, good frames out.

    `cap` is the buffer the C side would have (header + max payload + CRC);
    a longer frame is discarded whole, as drivers/slip.c's slip_feed() does.
    """

    def __init__(self, cap: int = HDR_LEN + MAX_PAYLOAD + CRC_LEN) -> None:
        self.cap = cap
        self.buf = bytearray()
        self.esc = False
        self.discarding = False
        self.frames = 0
        self.crc_err = 0
        self.runt = 0
        self.bad_len = 0
        self.overflow = 0

    def feed(self, data: bytes) -> list[Frame]:
        out: list[Frame] = []
        for c in data:
            f = self._byte(c)
            if f is not None:
                out.append(f)
        return out

    def _byte(self, c: int) -> Frame | None:
        if c == SLIP_END:
            self.esc = False
            if self.discarding:
                self.discarding = False
                self.buf.clear()
                return None
            if not self.buf:
                return None
            raw = bytes(self.buf)
            self.buf.clear()
            return self._frame(raw)
        if self.discarding:
            return None
        if self.esc:
            self.esc = False
            if c == SLIP_ESC_END:
                c = SLIP_END
            elif c == SLIP_ESC_ESC:
                c = SLIP_ESC
        elif c == SLIP_ESC:
            self.esc = True
            return None
        if len(self.buf) >= self.cap:
            self.discarding = True
            self.overflow += 1
            return None
        self.buf.append(c)
        return None

    def _frame(self, raw: bytes) -> Frame | None:
        if len(raw) < HDR_LEN + CRC_LEN:
            self.runt += 1
            return None
        body, (crc,) = raw[:-CRC_LEN], struct.unpack("<I", raw[-CRC_LEN:])
        if crc32(body) != crc:
            self.crc_err += 1
            return None
        try:
            f = parse(body)
        except FrameError:
            self.bad_len += 1
            return None
        self.frames += 1
        return f
