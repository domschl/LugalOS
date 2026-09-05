#!/usr/bin/env python3
"""A minimal MQTT 3.1.1 broker, for testing a client against a real socket.

Q1/§7(a), plan/phase26_mqtt_and_environment_sensors.md. This is the CI half
of the MQTT testbed: QEMU's slirp puts the host at 10.0.2.2, so the guest
dials an ordinary listener here and every byte on the wire is ours to assert
on. No mosquitto, no external service, nothing to clean up if a test crashes.

**It is a test instrument, not a broker.** One connection at a time, no
persistence, no QoS above 0, no wildcards beyond a trailing '#', and it
records everything it sees so a test can assert on it. What it does have that
a real broker does not is the ability to *misbehave on purpose*:

    broker = MqttBroker(split_headers=True)   # a fixed header across two segments
    broker = MqttBroker(connack_rc=4)         # refuse with "bad username or password"
    broker = MqttBroker(stall_after=3)        # stop reading mid-conversation

which is what makes a client's framing and error paths testable at all. A
peer that can only behave correctly cannot test an implementation.
"""

from __future__ import annotations

import socket
import threading
import time


CONNECT, CONNACK = 0x10, 0x20
PUBLISH, PUBACK = 0x30, 0x40
SUBSCRIBE, SUBACK = 0x80, 0x90
UNSUBSCRIBE, UNSUBACK = 0xA0, 0xB0
PINGREQ, PINGRESP = 0xC0, 0xD0
DISCONNECT = 0xE0


def encode_varint(value: int) -> bytes:
    """Remaining Length: seven bits per byte, high bit as continuation."""
    if value > 268435455:
        raise ValueError("above the protocol maximum")
    out = bytearray()
    while True:
        byte = value % 128
        value //= 128
        if value:
            byte |= 0x80
        out.append(byte)
        if not value:
            return bytes(out)


def decode_varint(buf: bytes, offset: int = 0) -> tuple[int, int]:
    """(value, bytes consumed). Raises on a fifth continuation byte, returns
    (-1, 0) when `buf` holds only part of one."""
    value, mult = 0, 1
    for i in range(4):
        if offset + i >= len(buf):
            return (-1, 0)
        byte = buf[offset + i]
        value += (byte & 0x7F) * mult
        if not byte & 0x80:
            return (value, i + 1)
        mult *= 128
    raise ValueError("malformed Remaining Length: a fifth continuation byte")


def _str_field(buf: bytes, off: int) -> tuple[str, int]:
    n = int.from_bytes(buf[off:off + 2], "big")
    return (buf[off + 2:off + 2 + n].decode("utf-8", "replace"), off + 2 + n)


class Connect:
    """A parsed CONNECT, so a test can assert on what the client actually sent
    rather than on whether the broker liked it."""

    def __init__(self, body: bytes):
        self.protocol, off = _str_field(body, 0)
        self.level = body[off]
        self.flags = body[off + 1]
        self.keepalive = int.from_bytes(body[off + 2:off + 4], "big")
        off += 4
        self.client_id, off = _str_field(body, off)
        self.will_topic = self.will_payload = None
        self.username = self.password = None
        if self.flags & 0x04:
            self.will_topic, off = _str_field(body, off)
            self.will_payload, off = _str_field(body, off)
        if self.flags & 0x80:
            self.username, off = _str_field(body, off)
        if self.flags & 0x40:
            self.password, off = _str_field(body, off)

    @property
    def clean_session(self) -> bool:
        return bool(self.flags & 0x02)

    @property
    def will_retain(self) -> bool:
        return bool(self.flags & 0x20)

    def __repr__(self) -> str:
        return (f"Connect(id={self.client_id!r}, level={self.level}, "
                f"keepalive={self.keepalive}, user={self.username!r}, "
                f"will={self.will_topic!r})")


class Publish:
    def __init__(self, body: bytes, flags: int):
        self.topic, off = _str_field(body, 0)
        self.qos = (flags >> 1) & 0x03
        self.retain = bool(flags & 0x01)
        self.dup = bool(flags & 0x08)
        if self.qos:
            self.packet_id = int.from_bytes(body[off:off + 2], "big")
            off += 2
        else:
            self.packet_id = None
        self.payload = body[off:]

    def __repr__(self) -> str:
        return (f"Publish(topic={self.topic!r}, payload={self.payload!r}, "
                f"qos={self.qos}, retain={self.retain})")


class MqttBroker:
    """Serves one connection on 127.0.0.1, in a background thread.

    Everything received is appended to `self.packets` (and CONNECT/PUBLISH also
    to `self.connect` / `self.publishes`), so assertions run after the fact
    rather than racing the guest."""

    def __init__(self, connack_rc: int = 0, split_headers: bool = False,
                 stall_after: int | None = None, keep_listening: bool = False,
                 port: int = 0):
        self.connack_rc = connack_rc
        self.split_headers = split_headers
        self.stall_after = stall_after
        self.keep_listening = keep_listening

        self.packets: list[tuple[int, bytes]] = []     # (type|flags, body)
        self.publishes: list[Publish] = []
        self.subscriptions: list[str] = []
        self.connect: Connect | None = None
        self.pings = 0
        self.disconnected_cleanly = False
        self.saw_eof = False
        self.error: str | None = None

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # port=0 lets the OS choose; a specific port is what a reconnect test
        # needs, since the client is already dialling the old one. SO_REUSEADDR
        # above is what makes re-binding it immediately possible.
        self._sock.bind(("127.0.0.1", port))
        self._sock.listen(1)
        self._sock.settimeout(60.0)
        self.port: int = self._sock.getsockname()[1]

        self._conn: socket.socket | None = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    # -- what a test drives the broker with -------------------------------

    def publish(self, topic: str, payload: bytes, retain: bool = False) -> None:
        """Sends a PUBLISH to the connected client (Q3's inbound path)."""
        body = len(topic).to_bytes(2, "big") + topic.encode() + payload
        self._send(bytes([PUBLISH | (0x01 if retain else 0)]) + encode_varint(len(body)) + body)

    def wait_for_publish(self, timeout: float = 10.0, count: int = 1) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if len(self.publishes) >= count:
                    return True
            time.sleep(0.02)
        return False

    def wait_for_connect(self, timeout: float = 15.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if self.connect is not None:
                    return True
            time.sleep(0.02)
        return False

    def close(self) -> None:
        self._stop.set()
        for s in (self._conn, self._sock):
            try:
                if s:
                    s.close()
            except OSError:
                pass
        self._thread.join(timeout=5.0)

    # -- the wire ---------------------------------------------------------

    def _send(self, data: bytes) -> None:
        conn = self._conn
        if not conn:
            return
        try:
            if self.split_headers and len(data) > 2:
                # A fixed header split across two segments: entirely legal, and
                # the shape an eager parser gets wrong.
                conn.sendall(data[:1])
                time.sleep(0.05)
                conn.sendall(data[1:])
            else:
                conn.sendall(data)
        except OSError as exc:
            self.error = repr(exc)

    def _serve(self) -> None:
        try:
            conn, _ = self._sock.accept()
        except (OSError, socket.timeout) as exc:
            if not self._stop.is_set():
                self.error = f"accept: {exc!r}"
            return

        self._conn = conn
        conn.settimeout(60.0)
        buf = b""
        served = 0
        try:
            while not self._stop.is_set():
                if self.stall_after is not None and served >= self.stall_after:
                    # Stop reading, but hold the connection open: the client
                    # should notice through its own keepalive, not through a
                    # reset.
                    time.sleep(0.2)
                    continue
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    self.saw_eof = True
                    break
                buf += chunk
                while len(buf) >= 2:
                    try:
                        body_len, vn = decode_varint(buf, 1)
                    except ValueError as exc:
                        self.error = str(exc)
                        return
                    if body_len < 0:
                        break
                    total = 1 + vn + body_len
                    if len(buf) < total:
                        break
                    header, body = buf[0], buf[1 + vn:total]
                    buf = buf[total:]
                    served += 1
                    self._handle(header, body)
        except OSError as exc:
            if not self._stop.is_set():
                self.error = repr(exc)
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def _handle(self, header: int, body: bytes) -> None:
        ptype = header & 0xF0
        with self._lock:
            self.packets.append((header, body))

            if ptype == CONNECT:
                self.connect = Connect(body)
                self._send(bytes([CONNACK, 0x02, 0x00, self.connack_rc]))
            elif ptype == PUBLISH:
                pub = Publish(body, header & 0x0F)
                self.publishes.append(pub)
                if pub.qos == 1 and pub.packet_id is not None:
                    self._send(bytes([PUBACK, 0x02]) + pub.packet_id.to_bytes(2, "big"))
            elif ptype == SUBSCRIBE:
                packet_id = int.from_bytes(body[0:2], "big")
                off, granted = 2, bytearray()
                while off < len(body):
                    topic, off = _str_field(body, off)
                    self.subscriptions.append(topic)
                    granted.append(body[off])       # grant what was requested
                    off += 1
                self._send(bytes([SUBACK]) + encode_varint(2 + len(granted)) +
                           packet_id.to_bytes(2, "big") + bytes(granted))
            elif ptype == UNSUBSCRIBE:
                packet_id = int.from_bytes(body[0:2], "big")
                self._send(bytes([UNSUBACK, 0x02]) + packet_id.to_bytes(2, "big"))
            elif ptype == PINGREQ:
                self.pings += 1
                self._send(bytes([PINGRESP, 0x00]))
            elif ptype == DISCONNECT:
                self.disconnected_cleanly = True


if __name__ == "__main__":
    # Standalone: a broker on a fixed port, printing what arrives. Useful when
    # driving a real board by hand rather than from a test.
    import sys
    b = MqttBroker()
    print(f"listening on 127.0.0.1:{b.port} (ctrl-c to stop)", file=sys.stderr)
    try:
        while True:
            time.sleep(0.5)
            with b._lock:
                while b.publishes:
                    print(b.publishes.pop(0))
    except KeyboardInterrupt:
        b.close()
