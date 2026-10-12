"""The shared test vectors (doc/llp.md §8): generated here, replayed by C.

    uv run llp-vectors > ../../tests/host/llp_vectors.txt   # regenerate
    uv run llp-vectors --check ../../tests/host/llp_vectors.txt

Three kinds of line:

* ``frame NAME TYPE SEQ PAYLOAD WIRE`` -- the serial binding's exact bytes for
  one frame (PAYLOAD ``-`` when empty). Both sides must encode PAYLOAD to WIRE
  and decode WIRE back.
* ``rx NAME CAP WIRE FRAMES CRC_ERR RUNT BAD_LEN OVERFLOW`` -- a byte stream fed
  to a serial receiver with a CAP-byte buffer, and its counters afterwards.
* ``scenario NAME`` ... ``end`` -- two links, A and B, driven by steps; under
  each step, indented, every line it produced, in order (``emit S HEX``,
  ``event S NAME``, ``recv S TYPE LEN``, ``full S``, ``result S RC``).
  Step verbs: init, time, tick, deliver, repeat, drop, send, consume, state.
"""

from __future__ import annotations

import struct
import sys
from collections.abc import Callable
from dataclasses import dataclass, field

from . import wire
from .link import Config, Link
from .wire import Frame

# --- frames -----------------------------------------------------------------


def pattern(n: int, start: int = 0) -> bytes:
    return bytes(((i + n) & 0xFF) for i in range(start, n))


def data_payload(type_: int, n: int) -> bytes:
    """What a scenario's ``send`` carries: a pattern; for P9, size[4] first."""
    if type_ == wire.T_P9:
        return struct.pack("<I", n) + pattern(n, 4)
    return pattern(n)


def frame_vectors() -> list[str]:
    hello = struct.pack("<BBBBBIIHB6s", wire.OP_HELLO, 1, 0, wire.ROLE_RADIO, wire.HELLO_F_FLOW,
                        0x22222222, 0x11111111, 4096, 8, bytes.fromhex("58e6c5d8d43c"))
    cases: list[tuple[str, int, int, bytes]] = [
        ("hello", wire.T_LINK, 0, hello),
        ("credit", wire.T_LINK, 0, bytes((wire.OP_CREDIT, 7, 8))),
        ("probe", wire.T_LINK, 0, bytes((wire.OP_PROBE, 0xFF))),
        ("state", wire.T_LINK, 0, bytes((wire.OP_STATE, 1, 0xC4, 0, 0))),
        ("eth_min", wire.T_ETH, 3, pattern(14)),
        ("eth_escapes", wire.T_ETH, 0xC0, bytes([0xC0, 0xDB, 0xDC, 0xDD] * 4)),
        ("p9_tversion", wire.T_P9, 9, bytes.fromhex("13000000") + b"\x64\xff\xff" + struct.pack("<I", 4096)
         + struct.pack("<H", 6) + b"9P2000"),
        ("empty_link", wire.T_LINK, 0, b""),
        ("eth_max", wire.T_ETH, 255, pattern(1514)),
    ]
    out = []
    for name, t, seq, p in cases:
        w = wire.serial_encode(Frame(t, seq, p))
        out.append(f"frame {name} {t} {seq} {p.hex() or '-'} {w.hex()}")
    return out


def rx_vectors() -> list[str]:
    good = wire.serial_encode(Frame(wire.T_ETH, 1, pattern(20)))
    good2 = wire.serial_encode(Frame(wire.T_LINK, 0, bytes((wire.OP_PROBE, 4))))
    body = Frame(wire.T_ETH, 1, pattern(20)).to_bytes()
    bad_crc = bytearray(good)
    bad_crc[10] ^= 0x01
    # A header whose length is one short of the payload, with a valid CRC.
    lie = bytearray(body)
    lie[2] -= 1
    bad_len = bytes((wire.SLIP_END,)) + wire.slip_escape(bytes(lie) + struct.pack("<I", wire.crc32(bytes(lie)))) \
        + bytes((wire.SLIP_END,))
    runt = bytes((wire.SLIP_END, 1, 2, 3, wire.SLIP_END))
    big = wire.serial_encode(Frame(wire.T_ETH, 2, pattern(200)))
    cases: list[tuple[str, int, bytes]] = [
        ("good", 4104, good),
        ("good_twice_shared_end", 4104, good[:-1] + good2),
        ("leading_garbage_and_ends", 4104, b"\xc0\xc0" + good),
        ("bad_crc", 4104, bytes(bad_crc)),
        ("bad_len", 4104, bad_len),
        ("runt", 4104, runt),
        # A frame longer than the buffer is dropped whole and the next one
        # survives -- the drivers/slip.c overflow fix (49.1).
        ("overflow_then_good", 64, big + good),
        ("bad_crc_then_good", 4104, bytes(bad_crc) + good),
    ]
    out = []
    for name, cap, stream in cases:
        rx = wire.SerialRx(cap)
        rx.feed(stream)
        out.append(f"rx {name} {cap} {stream.hex()} {rx.frames} {rx.crc_err} {rx.runt} {rx.bad_len} {rx.overflow}")
    return out


# --- scenarios --------------------------------------------------------------


@dataclass
class Side:
    name: str
    link: Link | None = None
    outbox: list[Frame] = field(default_factory=list)
    ring: int = 0
    cap: int = 64


class Scenario:
    """Two links and a log. Each method is one step verb of the DSL."""

    def __init__(self, name: str, lines: list[str]) -> None:
        self.lines = lines
        self.now = 0
        self.sides = {"A": Side("A"), "B": Side("B")}
        lines.append(f"scenario {name}")

    def _log(self, s: str) -> None:
        self.lines.append("  " + s)

    def _step(self, s: str) -> None:
        self.lines.append(f"step {s}")

    def _other(self, s: str) -> Side:
        return self.sides["B" if s == "A" else "A"]

    def link(self, s: str) -> Link:
        lk = self.sides[s].link
        assert lk is not None
        return lk

    def init(self, s: str, role: int, session: int, window: int, maxp: int = 4096, major: int = 0,
             mac: str = "000000000000") -> None:
        self._step(f"init {s} {role} {session:08x} {window} {maxp} {major} {mac}")
        side = self.sides[s]
        side.outbox.clear()
        side.ring = 0
        side.cap = window if window else 64

        def send(f: Frame, side: Side = side) -> bool:
            side.outbox.append(f)
            self._log(f"emit {side.name} {f.to_bytes().hex()}")
            return True

        def rx_free(side: Side = side) -> int:
            return side.cap - side.ring

        def event(ev: str, side: Side = side) -> None:
            self._log(f"event {side.name} {ev}")

        side.link = None
        side.link = Link(Config(role=role, max_payload=maxp, rx_window=window, mac=bytes.fromhex(mac),
                                major=major), session, self.now, send, rx_free, event)

    def time(self, ms: int) -> None:
        self._step(f"time {ms}")
        self.now = ms

    def tick(self, s: str) -> None:
        self._step(f"tick {s}")
        self.link(s).tick(self.now)

    def deliver(self, s: str) -> None:
        self._step(f"deliver {s}")
        src, dst = self.sides[s], self._other(s)
        frames, src.outbox = src.outbox, []
        for f in frames:
            data = self.link(dst.name).input(f, self.now)
            if data is not None:
                if dst.ring < dst.cap:
                    dst.ring += 1
                    self._log(f"recv {dst.name} {data.type} {len(data.payload)}")
                else:
                    self._log(f"full {dst.name}")

    def repeat(self, s: str) -> None:
        """Deliver S's frames but keep them, so a later ``deliver`` sends them
        again: duplicates, which a serial line never makes but a buggy
        binding or a retransmitting one could."""
        self._step(f"repeat {s}")
        src, dst = self.sides[s], self._other(s)
        for f in list(src.outbox):
            data = self.link(dst.name).input(f, self.now)
            if data is not None:
                if dst.ring < dst.cap:
                    dst.ring += 1
                    self._log(f"recv {dst.name} {data.type} {len(data.payload)}")
                else:
                    self._log(f"full {dst.name}")

    def drop(self, s: str) -> None:
        self._step(f"drop {s}")
        self.sides[s].outbox.clear()

    def send(self, s: str, type_: int, n: int) -> None:
        self._step(f"send {s} {type_} {n}")
        rc = self.link(s).send_data(type_, data_payload(type_, n), self.now)
        self._log(f"result {s} {rc}")

    def consume(self, s: str, n: int) -> None:
        self._step(f"consume {s} {n}")
        side = self.sides[s]
        side.ring = max(0, side.ring - n)

    def state(self, s: str, up: int, rssi: int) -> None:
        self._step(f"state {s} {up} {rssi}")
        self.link(s).send_state(up, rssi, self.now)

    def end(self) -> None:
        self.lines.append("end")


H, R = wire.ROLE_HOST, wire.ROLE_RADIO
MAC = "58e6c5d8d43c"


def _handshake(sc: Scenario, wa: int = 4, wb: int = 4) -> None:
    sc.init("A", H, 0x11111111, wa)
    sc.init("B", R, 0x22222222, wb, mac=MAC)
    sc.deliver("A")
    sc.deliver("B")
    sc.deliver("A")
    sc.deliver("B")


def sc_handshake(sc: Scenario) -> None:
    _handshake(sc)


def sc_data_credit(sc: Scenario) -> None:
    _handshake(sc, 4, 2)
    for _ in range(3):
        sc.send("A", wire.T_ETH, 60)
    sc.deliver("A")
    sc.consume("B", 2)
    sc.tick("B")
    sc.deliver("B")
    sc.send("A", wire.T_ETH, 60)
    sc.send("A", wire.T_P9, 23)
    sc.deliver("A")
    sc.send("B", wire.T_ETH, 1514)
    sc.deliver("B")


def sc_hello_lost(sc: Scenario) -> None:
    sc.init("A", H, 0x11111111, 4)
    sc.drop("A")
    sc.init("B", R, 0x22222222, 4, mac=MAC)
    sc.drop("B")
    sc.time(499)
    sc.tick("A")
    sc.time(500)
    sc.tick("A")
    sc.tick("B")
    sc.deliver("A")
    sc.drop("B")              # B's answer lost: B is not up, A is not up
    sc.time(1000)
    sc.tick("A")
    sc.tick("B")
    sc.deliver("A")
    sc.deliver("B")
    sc.deliver("A")


def sc_restart(sc: Scenario) -> None:
    _handshake(sc)
    sc.send("A", wire.T_ETH, 40)
    sc.deliver("A")
    sc.time(100)
    sc.init("B", R, 0x33333333, 4, mac=MAC)   # B reboots
    sc.deliver("B")
    sc.deliver("A")
    sc.deliver("B")
    sc.send("A", wire.T_ETH, 40)
    sc.deliver("A")


def sc_mismatch(sc: Scenario) -> None:
    sc.init("A", H, 0x11111111, 4)
    sc.init("B", R, 0x22222222, 4, major=2, mac=MAC)
    sc.deliver("A")
    sc.deliver("B")
    sc.deliver("A")
    sc.time(500)
    sc.tick("A")
    sc.tick("B")
    sc.deliver("A")
    sc.send("A", wire.T_ETH, 20)
    sc.init("B", R, 0x44444444, 4, mac=MAC)   # B upgraded
    sc.deliver("B")
    sc.deliver("A")
    sc.deliver("B")


def sc_probe(sc: Scenario) -> None:
    _handshake(sc, 4, 2)
    sc.send("A", wire.T_ETH, 30)
    sc.send("A", wire.T_ETH, 30)
    sc.drop("A")              # both lost on the wire
    sc.send("A", wire.T_ETH, 30)
    sc.time(99)
    sc.tick("A")
    sc.time(100)
    sc.tick("A")
    sc.deliver("A")
    sc.deliver("B")
    sc.tick("A")
    sc.send("A", wire.T_ETH, 30)
    sc.deliver("A")


def sc_dead(sc: Scenario) -> None:
    _handshake(sc)
    sc.time(1000)
    sc.tick("A")
    sc.tick("B")
    sc.drop("A")
    sc.drop("B")
    sc.time(2999)
    sc.tick("A")
    sc.time(3000)
    sc.tick("A")
    sc.tick("B")
    sc.deliver("A")
    sc.deliver("B")
    sc.deliver("A")
    sc.send("A", wire.T_ETH, 20)
    sc.deliver("A")


def sc_state_and_bad(sc: Scenario) -> None:
    _handshake(sc)
    sc.state("B", 1, -60)
    sc.deliver("B")
    sc.state("B", 0, 0)
    sc.deliver("B")
    sc.send("A", wire.T_ETH, 13)          # accepted by A, refused by B's shape check
    sc.send("A", wire.T_LINK, 20)         # not a data type: -3
    sc.send("A", wire.T_ETH, 4097)        # bigger than B's max payload: -3
    sc.deliver("A")


def sc_no_flow(sc: Scenario) -> None:
    _handshake(sc, 0, 0)
    for _ in range(5):
        sc.send("A", wire.T_ETH, 20)
    sc.deliver("A")
    sc.time(999)
    sc.tick("A")
    sc.time(1000)
    sc.tick("A")
    sc.deliver("A")


def sc_full_ring(sc: Scenario) -> None:
    # B advertises 4 but its ring has room for 4 only: a sender ignoring the
    # window would be caught as "full", and a correct one never is.
    _handshake(sc, 4, 4)
    for _ in range(6):
        sc.send("A", wire.T_P9, 9)
    sc.deliver("A")
    sc.consume("B", 1)
    sc.tick("B")
    sc.consume("B", 1)
    sc.tick("B")
    sc.deliver("B")
    sc.send("A", wire.T_P9, 9)
    sc.send("A", wire.T_P9, 9)
    sc.send("A", wire.T_P9, 9)
    sc.deliver("A")


def sc_duplicates(sc: Scenario) -> None:
    _handshake(sc, 8, 8)
    sc.send("A", wire.T_ETH, 20)
    sc.send("A", wire.T_ETH, 21)
    sc.repeat("A")
    sc.deliver("A")           # the same two again: stale, dropped
    sc.send("A", wire.T_ETH, 22)
    sc.deliver("A")


SCENARIOS: list[tuple[str, Callable[[Scenario], None]]] = [
    ("handshake", sc_handshake),
    ("data_credit", sc_data_credit),
    ("hello_lost", sc_hello_lost),
    ("restart", sc_restart),
    ("mismatch", sc_mismatch),
    ("probe", sc_probe),
    ("dead", sc_dead),
    ("state_and_bad", sc_state_and_bad),
    ("no_flow", sc_no_flow),
    ("full_ring", sc_full_ring),
    ("duplicates", sc_duplicates),
]


def generate() -> str:
    lines = [
        "# LLP v1 test vectors -- GENERATED by host/llp (`uv run llp-vectors`); do not edit.",
        "# Replayed by tests/host/llp_host.c and host/llp/tests. Format: host/llp/src/llp/vectors.py.",
    ]
    lines += frame_vectors()
    lines += rx_vectors()
    for name, fn in SCENARIOS:
        sc = Scenario(name, lines)
        fn(sc)
        sc.end()
    return "\n".join(lines) + "\n"


def main() -> None:
    args = sys.argv[1:]
    if args[:1] == ["--check"]:
        path = args[1]
        with open(path) as f:
            have = f.read()
        if have != generate():
            print(f"{path} differs from what host/llp generates", file=sys.stderr)
            sys.exit(1)
        print(f"{path}: matches")
        return
    sys.stdout.write(generate())


if __name__ == "__main__":
    main()
