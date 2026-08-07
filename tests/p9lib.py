"""Minimal, independent 9P2000 client used by tests/runner.py to exercise
LugalOS's 9P server (fs/9p.c) over real transports (A3: drivers/virtio_console.c
and drivers/uart_net.c's SLIP link) from an external host process -- not
through any of LugalOS's own C client code. Deliberately small: just enough
of the wire format (Tversion, Tattach, Twalk, Topen, Tread, Tclunk) to
attach, walk to a file, and read it.

Wire format matches fs/9p.c exactly: little-endian, size[4] type[1] tag[2]
followed by a per-type body; strings are u16-length-prefixed (not
NUL-terminated). Two framings are supported over the underlying byte
stream, matching each backend's own choice (see the A3 completion notes in
plan/phase5_distributed_design.md for the rationale):
  - "raw": the message's own size[4] prefix is the only framing
    (virtio-console -- a reliable virtqueue-backed channel needs nothing
    else).
  - "slip": RFC 1055 SLIP-escaped, END-delimited (the physical UART link,
    matching drivers/uart_net.c's slip_encode()/slip_decode()).
"""

from __future__ import annotations

import socket
import struct

TVERSION, RVERSION = 100, 101
TATTACH, RATTACH = 104, 105
RERROR = 107
TWALK, RWALK = 110, 111
TOPEN, ROPEN = 112, 113
TREAD, RREAD = 116, 117
TCLUNK, RCLUNK = 120, 121

NOFID = 0xFFFFFFFF

SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD


class P9Error(Exception):
    pass


def _pack_str(s: str) -> bytes:
    b = s.encode()
    return struct.pack("<H", len(b)) + b


def _frame(msg_type: int, tag: int, body: bytes) -> bytes:
    payload = struct.pack("<BH", msg_type, tag) + body
    return struct.pack("<I", 4 + len(payload)) + payload


def slip_encode(data: bytes) -> bytes:
    out = bytearray([SLIP_END])
    for b in data:
        if b == SLIP_END:
            out += bytes([SLIP_ESC, SLIP_ESC_END])
        elif b == SLIP_ESC:
            out += bytes([SLIP_ESC, SLIP_ESC_ESC])
        else:
            out.append(b)
    out.append(SLIP_END)
    return bytes(out)


def slip_decode(data: bytes) -> bytes:
    """Mirrors drivers/uart_net.c's slip_decode(): stops at the first END
    once something has been decoded; a leading END (or run of them) is
    skipped."""
    out = bytearray()
    escaping = False
    for b in data:
        if b == SLIP_END:
            if out:
                break
            continue
        if escaping:
            out.append(SLIP_END if b == SLIP_ESC_END else SLIP_ESC)
            escaping = False
        elif b == SLIP_ESC:
            escaping = True
        else:
            out.append(b)
    return bytes(out)


class P9Client:
    """A single-connection, single-outstanding-request 9P client over a raw
    stream socket (e.g. a QEMU chardev unix socket)."""

    def __init__(self, sock: socket.socket, framing: str = "raw") -> None:
        if framing not in ("raw", "slip"):
            raise ValueError(f"unknown framing {framing!r}")
        self._sock = sock
        self._next_tag = 1
        self._framing = framing
        self._pending = bytearray()  # unprocessed bytes, SLIP framing only

    @classmethod
    def connect_unix(cls, path: str, timeout: float = 5.0, framing: str = "raw") -> "P9Client":
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(path)
        return cls(s, framing=framing)

    def close(self) -> None:
        self._sock.close()

    def _tag(self) -> int:
        t = self._next_tag
        self._next_tag += 1
        return t

    def _recv_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise P9Error("connection closed while reading a frame")
            buf += chunk
        return buf

    def _recv_slip_frame(self) -> bytes:
        while True:
            idx = self._pending.find(bytes([SLIP_END]))
            if idx == -1:
                chunk = self._sock.recv(4096)
                if not chunk:
                    raise P9Error("connection closed while reading a SLIP frame")
                self._pending.extend(chunk)
                continue
            if idx == 0:
                del self._pending[0]
                continue
            raw = bytes(self._pending[: idx + 1])  # content + its terminating END
            del self._pending[: idx + 1]
            decoded = slip_decode(raw)
            if decoded:
                return decoded
            # decoded to nothing (shouldn't normally happen) -- keep scanning

    def _roundtrip(self, msg_type: int, body: bytes) -> tuple[int, bytes]:
        tag = self._tag()
        raw_frame = _frame(msg_type, tag, body)

        if self._framing == "slip":
            self._sock.sendall(slip_encode(raw_frame))
            rest = self._recv_slip_frame()
            if len(rest) < 7:
                raise P9Error(f"malformed reply: {len(rest)} bytes < 7")
            size = struct.unpack_from("<I", rest, 0)[0]
            if size != len(rest):
                raise P9Error(f"declared size {size} != decoded length {len(rest)}")
            resp_type, resp_tag = struct.unpack_from("<BH", rest, 4)
            resp_body = rest[7:]
        else:
            self._sock.sendall(raw_frame)
            size = struct.unpack("<I", self._recv_exact(4))[0]
            if size < 7:
                raise P9Error(f"malformed reply: declared size {size} < 7")
            rest = self._recv_exact(size - 4)
            resp_type, resp_tag = struct.unpack("<BH", rest[:3])
            resp_body = rest[3:]

        if resp_tag != tag:
            raise P9Error(f"tag mismatch: sent {tag}, got {resp_tag}")
        if resp_type == RERROR:
            (elen,) = struct.unpack_from("<H", resp_body, 0)
            raise P9Error(resp_body[2 : 2 + elen].decode(errors="replace"))
        return resp_type, resp_body

    def version(self, msize: int = 4096) -> int:
        rtype, body = self._roundtrip(TVERSION, struct.pack("<I", msize) + _pack_str("9P2000"))
        if rtype != RVERSION:
            raise P9Error(f"expected Rversion, got type {rtype}")
        return struct.unpack_from("<I", body, 0)[0]

    def attach(self, fid: int, aname: str = "", uname: str = "lugal") -> bytes:
        body = struct.pack("<II", fid, NOFID) + _pack_str(uname) + _pack_str(aname)
        rtype, body = self._roundtrip(TATTACH, body)
        if rtype != RATTACH:
            raise P9Error(f"expected Rattach, got type {rtype}")
        return body  # 13-byte qid

    def walk(self, fid: int, newfid: int, names: list[str]) -> int:
        body = struct.pack("<IIH", fid, newfid, len(names))
        for n in names:
            body += _pack_str(n)
        rtype, resp = self._roundtrip(TWALK, body)
        if rtype != RWALK:
            raise P9Error(f"expected Rwalk, got type {rtype}")
        (nwqid,) = struct.unpack_from("<H", resp, 0)
        return nwqid

    def open(self, fid: int, mode: int = 0) -> bytes:
        rtype, body = self._roundtrip(TOPEN, struct.pack("<IB", fid, mode))
        if rtype != ROPEN:
            raise P9Error(f"expected Ropen, got type {rtype}")
        return body  # 13-byte qid + 4-byte iounit

    def read(self, fid: int, offset: int, count: int) -> bytes:
        rtype, body = self._roundtrip(TREAD, struct.pack("<IQI", fid, offset, count))
        if rtype != RREAD:
            raise P9Error(f"expected Rread, got type {rtype}")
        (n,) = struct.unpack_from("<I", body, 0)
        return body[4 : 4 + n]

    def read_all(self, fid: int, chunk_size: int = 1024) -> bytes:
        out = b""
        offset = 0
        while True:
            chunk = self.read(fid, offset, chunk_size)
            if not chunk:
                return out
            out += chunk
            offset += len(chunk)

    def clunk(self, fid: int) -> None:
        rtype, _ = self._roundtrip(TCLUNK, struct.pack("<I", fid))
        if rtype != RCLUNK:
            raise P9Error(f"expected Rclunk, got type {rtype}")

    def cat(self, path: str, root_fid: int = 1, file_fid: int = 2) -> bytes:
        """Convenience: attach at '/', walk to `path` (split on '/'), open
        for read, read the whole file, clunk. Matches the sequence
        drivers/loopback_net.c's loopback_9p_cat() drives in-kernel."""
        self.version()
        self.attach(root_fid, aname="/")
        names = [p for p in path.split("/") if p]
        nwqid = self.walk(root_fid, file_fid, names)
        if nwqid != len(names):
            # A partial walk (nwqid < len(names)) leaves file_fid pointing at
            # whatever directory it got stuck on -- open()+read_all() would
            # silently return that directory's stat stream instead of
            # raising, which reads exactly like a successful cat() of the
            # wrong thing. Fail loudly instead.
            raise P9Error(
                f"walk to {path!r} only resolved {nwqid}/{len(names)} components "
                f"(stopped at {'/'.join(names[:nwqid]) or '/'})"
            )
        self.open(file_fid, mode=0)
        data = self.read_all(file_fid)
        self.clunk(file_fid)
        return data
