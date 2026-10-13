"""An LLP host on a serial port (49.3): the reference playing the P4's part.

One thread owns the port and the Link -- as kernel/llp_node.c's pump does --
so the protocol needs no lock; callers only queue frames and take delivered
ones. The port may be shared with a LugalOS console (the C6-Zero's single
USB port, drivers/usbjtag_esp32c6.c): bytes from one SLIP END to the next are
LLP's, everything outside a frame is console text, kept and optionally echoed.
"""

from __future__ import annotations

import collections
import os
import queue
import sys
import threading
import time
from typing import Protocol

from . import wire
from .link import UP, Config, Link
from .wire import Frame, SerialRx


class Port(Protocol):
    """What a pyserial Serial (or a test double) provides."""

    def read(self, size: int = 1) -> bytes: ...
    def write(self, b: bytes, /) -> int | None: ...
    @property
    def in_waiting(self) -> int: ...


ETH_SLOTS = 16


class SerialNode:
    def __init__(self, port: Port, role: int = wire.ROLE_HOST, echo_console: bool = False,
                 window: int = 8) -> None:
        self.port = port
        self.echo = echo_console
        self.console = bytearray()
        self._rx = SerialRx()
        self._in_frame = False
        self._eth_in: queue.Queue[bytes] = queue.Queue()
        self._p9_in = bytearray()
        self._p9_cv = threading.Condition()
        self._out: collections.deque[tuple[int, bytes]] = collections.deque()   # (type, payload)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self.events: list[str] = []
        self._role = role
        self._window = window
        # No link until start(): a frame sent before the peer has armed LLP on
        # a shared console is typed at its shell (escape bytes and all).
        self.link: Link | None = None
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def start(self) -> Link:
        """Start LLP: the first HELLO goes out now."""
        session = int.from_bytes(os.urandom(4), "little") | 1
        with self._lock:
            self.link = Link(Config(role=self._role, rx_window=self._window), session, self._now(),
                             send=self._send, rx_free=self._rx_free, event=self.events.append)
        return self.link

    # --- the pump -------------------------------------------------------

    @staticmethod
    def _now() -> int:
        return int(time.monotonic() * 1000) & 0xFFFFFFFF

    def _send(self, f: Frame) -> bool:
        self.port.write(wire.serial_encode(f))
        return True

    def _rx_free(self) -> int:
        return max(0, ETH_SLOTS - self._eth_in.qsize())

    def _route(self, data: bytes) -> None:
        for c in data:
            if c == wire.SLIP_END:
                self._in_frame = not self._in_frame
                for f in self._rx.feed(bytes((c,))):
                    self._input(f)
            elif self._in_frame:
                for f in self._rx.feed(bytes((c,))):
                    self._input(f)
            else:
                self.console.append(c)
                if self.echo:
                    sys.stdout.write(chr(c) if c < 128 else "?")
                    sys.stdout.flush()

    def _input(self, f: Frame) -> None:
        if self.link is None:
            return
        d = self.link.input(f, self._now())
        if d is None:
            return
        if d.type == wire.T_ETH:
            self._eth_in.put(d.payload)
        else:
            with self._p9_cv:
                self._p9_in += d.payload
                self._p9_cv.notify_all()

    def _run(self) -> None:
        while not self._stop.is_set():
            data = self.port.read(max(1, min(self.port.in_waiting, 4096)))
            if data:
                self._route(data)
            now = self._now()
            link = self.link
            if link is None:
                continue
            with self._lock:
                # 9P before Ethernet, as the C binding does (doc/llp.md §5).
                pending = sorted(self._out, key=lambda tf: tf[0] != wire.T_P9)
                self._out.clear()
            kept: list[tuple[int, bytes]] = []
            for t, p in pending:
                if kept or link.send_data(t, p, now) == -2:
                    kept.append((t, p))
            if kept:
                with self._lock:
                    self._out.extendleft(reversed(kept))
            link.tick(now)

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2)

    # --- for callers ----------------------------------------------------

    def wait_up(self, timeout: float = 10.0) -> bool:
        t = time.monotonic() + timeout
        while time.monotonic() < t:
            if self.link is not None and self.link.state == UP:
                return True
            time.sleep(0.02)
        return False

    def send_eth(self, frame: bytes) -> None:
        with self._lock:
            self._out.append((wire.T_ETH, frame))

    def recv_eth(self, timeout: float) -> bytes | None:
        try:
            return self._eth_in.get(timeout=timeout)
        except queue.Empty:
            return None

    def console_text(self) -> str:
        return self.console.decode("utf-8", "replace")

    def type_line(self, line: str) -> None:
        """Typed at the peer's console (outside any frame)."""
        self.port.write(line.encode() + b"\r")

    def wait_console(self, needle: str, timeout: float) -> bool:
        t = time.monotonic() + timeout
        while time.monotonic() < t:
            if needle in self.console_text():
                return True
            time.sleep(0.05)
        return False

    def p9_socket(self) -> P9Socket:
        return P9Socket(self)


class P9Socket:
    """The socket-like object p9lib.P9Client drives, over LLP's P9 frames.
    sendall() is handed whole 9P messages by P9Client; each becomes a frame."""

    def __init__(self, node: SerialNode) -> None:
        self._n = node
        self._timeout = 10.0

    def sendall(self, data: bytes) -> None:
        with self._n._lock:
            self._n._out.append((wire.T_P9, data))

    def recv(self, n: int) -> bytes:
        cv = self._n._p9_cv
        with cv:
            if not self._n._p9_in:
                cv.wait(self._timeout)
            out = bytes(self._n._p9_in[:n])
            del self._n._p9_in[:n]
            return out

    def settimeout(self, t: float) -> None:
        self._timeout = t

    def close(self) -> None:
        pass
