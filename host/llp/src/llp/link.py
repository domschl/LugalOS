"""The LLP link: handshake, credits, liveness (doc/llp.md §4-§6).

A line-for-line counterpart of kernel/llp.c's llp_link_*(), with the same
callbacks: `send(frame) -> bool`, `rx_free() -> int`, `event(name)`.
tests/host/llp_vectors.txt (generated from this file by `llp-vectors`) is
replayed by tests/host/llp_host.c against the C one; a difference in any
emitted byte, event or delivery fails there.
"""

from __future__ import annotations

import struct
from collections.abc import Callable
from dataclasses import dataclass, field

from . import wire
from .wire import Frame

HELLO_RETRY_MS = 500
DEAD_MS = 3000
KEEPALIVE_MS = 1000
PROBE_MS = 100

DOWN = 0
UP = 1
MISMATCH = 2

EV_UP = "up"
EV_DOWN = "down"
EV_MISMATCH = "mismatch"
EV_PEER_STATE = "peer_state"


def _u8(x: int) -> int:
    return x & 0xFF


def _elapsed(now: int, t: int, ms: int) -> bool:
    return ((now - t) & 0xFFFFFFFF) >= ms


@dataclass
class Config:
    role: int
    max_payload: int = wire.MAX_PAYLOAD
    rx_window: int = 0
    mac: bytes = bytes(6)
    major: int = 0  # 0, 0 = this build's version
    minor: int = 0


@dataclass
class Stats:
    tx_frames: int = 0
    rx_frames: int = 0
    tx_link: int = 0
    rx_link: int = 0
    tx_refused: int = 0
    tx_no_credit: int = 0
    rx_lost: int = 0
    rx_stale: int = 0
    rx_not_up: int = 0
    rx_bad: int = 0
    rx_too_big: int = 0
    hellos: int = 0
    probes: int = 0
    credits: int = 0
    ups: int = 0
    downs: int = 0


@dataclass
class Link:
    cfg: Config
    session: int
    now: int
    send: Callable[[Frame], bool]
    rx_free: Callable[[], int] = lambda: 0
    event: Callable[[str], None] = lambda _ev: None

    state: int = DOWN
    peer_session: int = 0
    peer_major: int = 0
    peer_minor: int = 0
    peer_role: int = 0
    peer_flags: int = 0
    peer_max_payload: int = 0
    peer_mac: bytes = bytes(6)
    peer_link: int = 0
    peer_rssi: int = 0
    tx_seq: int = 0
    tx_ack: int = 0
    tx_window: int = 0
    blocked: bool = False
    last_probe: int = 0
    rx_next: int = 0
    adv_limit: int = 0
    last_rx: int = 0
    last_tx: int = 0
    last_hello: int = 0
    st: Stats = field(default_factory=Stats)

    def __post_init__(self) -> None:
        """llp_link_init()."""
        self.cfg.rx_window = min(self.cfg.rx_window, wire.MAX_WINDOW)
        self.cfg.max_payload = min(self.cfg.max_payload, wire.MAX_PAYLOAD)
        self.last_rx = self.now
        self.last_tx = self.now
        self._send_hello(self.now)

    # --- helpers --------------------------------------------------------

    def _major(self) -> int:
        return self.cfg.major or wire.VERSION_MAJOR

    def _minor(self) -> int:
        return self.cfg.minor if self.cfg.major else wire.VERSION_MINOR

    def _emit(self, type_: int, seq: int, payload: bytes, now: int) -> bool:
        if not self.send(Frame(type_, seq, payload)):
            self.st.tx_refused += 1
            return False
        self.last_tx = now
        if type_ == wire.T_LINK:
            self.st.tx_link += 1
        return True

    def _rx_window_now(self) -> int:
        if not self.cfg.rx_window:
            return 0
        return min(self.rx_free(), self.cfg.rx_window)

    def _send_hello(self, now: int) -> None:
        window = self._rx_window_now()
        flags = (wire.HELLO_F_UP if self.state == UP else 0) | (wire.HELLO_F_FLOW if self.cfg.rx_window else 0)
        p = struct.pack(
            "<BBBBBIIHB6s",
            wire.OP_HELLO, self._major(), self._minor(), self.cfg.role, flags,
            self.session, self.peer_session, self.cfg.max_payload, window, self.cfg.mac,
        )
        self.adv_limit = _u8(self.rx_next + window)
        self.last_hello = now
        self.st.hellos += 1
        self._emit(wire.T_LINK, 0, p, now)

    def _send_credit(self, window: int, now: int) -> None:
        self.adv_limit = _u8(self.rx_next + window)
        self.st.credits += 1
        self._emit(wire.T_LINK, 0, bytes((wire.OP_CREDIT, self.rx_next, window)), now)

    def _maybe_credit(self, force: bool, now: int) -> None:
        if not self.cfg.rx_window:
            if force:
                self._send_credit(0, now)
            return
        window = self._rx_window_now()
        moved = _u8(_u8(self.rx_next + window) - self.adv_limit)
        thresh = self.cfg.rx_window // 4 or 1
        if force or (thresh <= moved < 128):
            self._send_credit(window, now)

    def can_send(self) -> bool:
        if self.state != UP:
            return False
        if not self.peer_flags & wire.HELLO_F_FLOW:
            return True
        return _u8(self.tx_seq - self.tx_ack) < self.tx_window

    # --- receiving ------------------------------------------------------

    def _on_hello(self, p: bytes, now: int) -> None:
        if len(p) < wire.HELLO_LEN:
            self.st.rx_bad += 1
            return
        _op, major, minor, role, flags, session, seen, max_payload, window, mac = struct.unpack_from(
            "<BBBBBIIHB6s", p
        )
        if session == 0:
            self.st.rx_bad += 1
            return
        if session != self.peer_session:
            was_up = self.state == UP
            self.peer_session = session
            self.peer_major, self.peer_minor, self.peer_role, self.peer_flags = major, minor, role, flags
            self.peer_max_payload = max_payload
            self.peer_mac = bytes(mac)
            self.peer_link = 0
            self.peer_rssi = 0
            self.tx_seq = 0
            self.tx_ack = 0
            self.tx_window = min(window, wire.MAX_WINDOW)
            self.blocked = False
            self.rx_next = 0
            if was_up:
                self.state = DOWN
                self.st.downs += 1
                self.event(EV_DOWN)
            if self.peer_major != self._major() or self.peer_max_payload < wire.MIN_PAYLOAD:
                self.state = MISMATCH
                self.event(EV_MISMATCH)
                self._send_hello(now)
                return
            if self.state == MISMATCH:
                self.state = DOWN
        else:
            if self.state == MISMATCH:
                return
            self.peer_flags = flags

        if seen == self.session:
            if self.state != UP:
                self.state = UP
                self.last_rx = now
                self.st.ups += 1
                self.event(EV_UP)
            if not flags & wire.HELLO_F_UP:
                self._send_hello(now)
        else:
            self._send_hello(now)

    def _on_credit(self, p: bytes) -> None:
        if len(p) < wire.CREDIT_LEN:
            self.st.rx_bad += 1
            return
        if self.state != UP:
            return
        ack = p[1]
        if _u8(ack - self.tx_ack) > _u8(self.tx_seq - self.tx_ack):
            self.st.rx_bad += 1
            return
        self.tx_ack = ack
        self.tx_window = min(p[2], wire.MAX_WINDOW)
        if self.can_send():
            self.blocked = False

    def _on_probe(self, p: bytes, now: int) -> None:
        if len(p) < wire.PROBE_LEN:
            self.st.rx_bad += 1
            return
        if self.state != UP:
            return
        gap = _u8(p[1] - self.rx_next)
        if gap < 128:
            self.st.rx_lost += gap
            self.rx_next = p[1]
        self._maybe_credit(True, now)

    def _on_state(self, p: bytes) -> None:
        if len(p) < wire.STATE_LEN:
            self.st.rx_bad += 1
            return
        if self.state != UP:
            return
        self.peer_link = p[1]
        self.peer_rssi = p[2] - 256 if p[2] >= 128 else p[2]
        self.event(EV_PEER_STATE)

    def input(self, f: Frame, now: int) -> Frame | None:
        """llp_link_input(): data for the caller, or None."""
        self.last_rx = now
        if f.type == wire.T_LINK:
            self.st.rx_link += 1
            if not f.payload:
                self.st.rx_bad += 1
                return None
            op = f.payload[0]
            if op == wire.OP_HELLO:
                self._on_hello(f.payload, now)
            elif op == wire.OP_CREDIT:
                self._on_credit(f.payload)
            elif op == wire.OP_PROBE:
                self._on_probe(f.payload, now)
            elif op == wire.OP_STATE:
                self._on_state(f.payload)
            else:
                self.st.rx_bad += 1
            return None
        if f.type not in (wire.T_ETH, wire.T_P9):
            self.st.rx_bad += 1
            return None
        if self.state != UP:
            self.st.rx_not_up += 1
            return None
        if len(f.payload) > self.cfg.max_payload:
            self.st.rx_too_big += 1
            return None
        gap = _u8(f.seq - self.rx_next)
        if gap >= 128:
            self.st.rx_stale += 1
            return None
        self.st.rx_lost += gap
        self.rx_next = _u8(f.seq + 1)
        n = len(f.payload)
        if f.type == wire.T_ETH and n < 14:
            self.st.rx_bad += 1
            return None
        if f.type == wire.T_P9 and (n < 7 or struct.unpack_from("<I", f.payload)[0] != n):
            self.st.rx_bad += 1
            return None
        self.st.rx_frames += 1
        return f

    # --- sending and time -----------------------------------------------

    def send_data(self, type_: int, payload: bytes, now: int) -> int:
        """llp_link_send(): 0, -1 not up, -2 no credit, -3 bad, -4 refused."""
        if self.state != UP:
            return -1
        if type_ not in (wire.T_ETH, wire.T_P9) or len(payload) > self.peer_max_payload:
            return -3
        if not self.can_send():
            self.st.tx_no_credit += 1
            if not self.blocked:
                self.blocked = True
                self.last_probe = now
            return -2
        if not self._emit(type_, self.tx_seq, payload, now):
            return -4
        self.tx_seq = _u8(self.tx_seq + 1)
        self.st.tx_frames += 1
        return 0

    def send_state(self, link_up: int, rssi: int, now: int) -> None:
        if self.state != UP:
            return
        self._emit(wire.T_LINK, 0, bytes((wire.OP_STATE, link_up, rssi & 0xFF, 0, 0)), now)

    def tick(self, now: int) -> None:
        if self.state == DOWN:
            if _elapsed(now, self.last_hello, HELLO_RETRY_MS):
                self._send_hello(now)
            return
        if self.state != UP:
            return
        if _elapsed(now, self.last_rx, DEAD_MS):
            self.state = DOWN
            self.st.downs += 1
            self.event(EV_DOWN)
            self._send_hello(now)
            return
        self._maybe_credit(False, now)
        if self.blocked:
            if self.can_send():
                self.blocked = False
            elif _elapsed(now, self.last_probe, PROBE_MS):
                self.last_probe = now
                self.st.probes += 1
                self._emit(wire.T_LINK, 0, bytes((wire.OP_PROBE, self.tx_seq)), now)
        if _elapsed(now, self.last_tx, KEEPALIVE_MS):
            self._maybe_credit(True, now)
