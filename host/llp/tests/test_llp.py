"""Tests of the reference itself. The cross-check with C is tests/host/llp_host.c."""

from __future__ import annotations

import random
import struct
from pathlib import Path

from llp import wire
from llp.link import UP, Config, Link
from llp.vectors import generate
from llp.wire import Frame, SerialRx

VECTORS = Path(__file__).resolve().parents[3] / "tests" / "host" / "llp_vectors.txt"


def test_committed_vectors_are_current() -> None:
    """tests/host/llp_vectors.txt is what this package generates: regenerate
    with `uv run llp-vectors > ../../tests/host/llp_vectors.txt` after a change."""
    assert VECTORS.read_text() == generate()


def test_serial_round_trip() -> None:
    rnd = random.Random(1)
    rx = SerialRx()
    for _ in range(300):
        n = rnd.choice([0, 1, 14, rnd.randrange(4096)])
        f = Frame(rnd.choice([1, 2, 3]), rnd.randrange(256), bytes(rnd.randrange(256) for _ in range(n)))
        assert rx.feed(wire.serial_encode(f)) == [f]
    assert rx.crc_err == rx.runt == rx.bad_len == rx.overflow == 0


def test_crc_is_kernel_crc32() -> None:
    # kernel/crc32.c's check value for "123456789" (CRC-32/ISO-HDLC).
    assert wire.crc32(b"123456789") == 0xCBF43926


def test_corruption_is_never_delivered() -> None:
    rnd = random.Random(2)
    good = wire.serial_encode(Frame(wire.T_ETH, 5, bytes(range(60))))
    for _ in range(2000):
        bad = bytearray(good)
        i = rnd.randrange(1, len(bad) - 1)
        bad[i] ^= 1 << rnd.randrange(8)
        for f in SerialRx().feed(bytes(bad)):
            assert f == Frame(wire.T_ETH, 5, bytes(range(60))), "a damaged frame got through"


class _Peer:
    def __init__(self, role: int, session: int, now: int, cap: int) -> None:
        self.line = bytearray()
        self.rx = SerialRx()
        self.ring = 0
        self.cap = cap
        self.ids: list[int] = []
        self.link = Link(Config(role=role, rx_window=cap, max_payload=1514), session, now,
                         send=self._send, rx_free=lambda: self.cap - self.ring)

    def _send(self, f: Frame) -> bool:
        self.line += wire.serial_encode(f)
        return True


def test_soak_lossy_line() -> None:
    """The C soak in miniature: noise on the line, in-order intact delivery,
    no buffer overflow, and recovery from a total outage."""
    rnd = random.Random(3)
    a, b = _Peer(wire.ROLE_HOST, 0xA, 0, 8), _Peer(wire.ROLE_RADIO, 0xB, 0, 8)
    next_id = 0
    full = 0
    for now in range(9000):
        outage = 2500 <= now < 6500   # longer than DEAD_MS
        for src, dst in ((a, b), (b, a)):
            chunk, src.line = bytes(src.line[:300]), src.line[300:]
            if outage:
                continue
            noisy = bytearray(chunk)
            for i in range(len(noisy)):
                if rnd.random() < 2e-5:
                    noisy[i] ^= 1 << rnd.randrange(8)
            for f in dst.rx.feed(bytes(noisy)):
                d = dst.link.input(f, now)
                if d is None:
                    continue
                if dst.ring >= dst.cap:
                    full += 1
                    continue
                dst.ring += 1
                (ident,) = struct.unpack_from("<I", d.payload)
                assert d.payload[4:] == bytes((ident + i) & 0xFF for i in range(4, len(d.payload)))
                dst.ids.append(ident)
        for p in (a, b):
            p.ring = max(0, p.ring - rnd.randrange(3))
        while a.link.can_send():
            payload = struct.pack("<I", next_id) + bytes((next_id + i) & 0xFF for i in range(4, 200))
            assert a.link.send_data(wire.T_ETH, payload, now) == 0
            next_id += 1
        if not a.link.can_send() and a.link.state == UP:
            a.link.send_data(wire.T_ETH, struct.pack("<I", next_id) + bytes(16), now)
        a.link.tick(now)
        b.link.tick(now)
    assert full == 0, "credits let the receive buffer overflow"
    assert b.ids == sorted(set(b.ids)), "duplicate or reordered delivery"
    assert len(b.ids) > 500
    assert a.link.state == UP and b.link.state == UP
    assert a.link.st.downs >= 1 and a.link.st.ups >= 2, "the outage was not noticed and recovered from"
