"""Independent 9P2000 client for LugalOS's 9P server (fs/9p.c), speaking the
wire protocol directly rather than through any of LugalOS's own C client
code. Originally `tests/p9lib.py` (read-only: attach/walk/open/read/clunk,
enough to exercise fs/9p.c from tests/runner.py); promoted here and extended
with the write path (Twrite/Tcreate/Tremove/Tstat) and a path-based `Session`
convenience layer as the actual product this phase (14a,
plan/phase13_lisp_engine_extensions.md's successor) is about: a real,
general-purpose way for a host to read AND write LugalOS's filesystems, not
just a test-only read probe.

Wire format matches fs/9p.c exactly: little-endian, size[4] type[1] tag[2]
followed by a per-type body; strings are u16-length-prefixed (not
NUL-terminated). Two framings are supported over the underlying byte
stream, matching each backend's own choice (see the A3 completion notes in
plan/phase5_distributed_design.md for the rationale):
  - "raw": the message's own size[4] prefix is the only framing
    (virtio-console, and RP2350's USB-CDC ACM1 "net" port -- both reliable,
    already-framed channels needing nothing else).
  - "slip": RFC 1055 SLIP-escaped, END-delimited (a physical UART link,
    matching drivers/uart_net.c's slip_encode()/slip_decode()).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

TVERSION, RVERSION = 100, 101
TATTACH, RATTACH = 104, 105
RERROR = 107
TWALK, RWALK = 110, 111
TOPEN, ROPEN = 112, 113
TCREATE, RCREATE = 114, 115
TREAD, RREAD = 116, 117
TWRITE, RWRITE = 118, 119
TCLUNK, RCLUNK = 120, 121
TREMOVE, RREMOVE = 122, 123
TSTAT, RSTAT = 124, 125

NOFID = 0xFFFFFFFF

# 9P open/create mode bits (fs/include/fs/9p.h) -- the low 2 bits select the
# access mode, the rest are independent flags.
OREAD = 0x00
OWRITE = 0x01
ORDWR = 0x02
OEXEC = 0x03
OTRUNC = 0x10
ORCLOSE = 0x40

# Tcreate's `perm` DMDIR bit marks the new entry as a directory.
DMDIR = 0x80000000

# fs/include/fs/9p.h's own per-connection limits -- not protocol limits, but
# real ones: exceeding P9_MAX_WALK_ELEM/P9_MAX_NAME_LEN server-side fails the
# request, and P9_MAX_FIDS (8) is small enough that a client juggling many
# fids at once will run the connection out, not just its own bookkeeping.
MAX_FIDS = 8
MAX_WALK_ELEM = 16
MAX_NAME_LEN = 32

SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD


class P9Error(Exception):
    pass


def _pack_str(s: str) -> bytes:
    b = s.encode()
    return struct.pack("<H", len(b)) + b


def _unpack_str(buf: bytes, off: int) -> tuple[str, int]:
    (n,) = struct.unpack_from("<H", buf, off)
    off += 2
    return buf[off : off + n].decode(errors="replace"), off + n


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


@dataclass
class Qid:
    type: int
    vers: int
    path: int

    @property
    def is_dir(self) -> bool:
        return (self.type & 0x80) != 0


@dataclass
class Stat:
    """One decoded 9P stat entry -- what Rstat carries, and what a
    directory's Tread stream is a concatenation of (p9_pack_stat() in
    fs/9p.c). atime/mtime are always 0 (these targets have no wall-clock
    epoch time to report -- see fs/9p.c's own comment); uid/gid/muid are
    always empty strings (this server has no user identity concept)."""
    qid: Qid
    mode: int
    atime: int
    mtime: int
    length: int
    name: str
    uid: str
    gid: str
    muid: str

    @property
    def is_dir(self) -> bool:
        return self.qid.is_dir


def _parse_qid(buf: bytes, off: int) -> tuple[Qid, int]:
    qtype = buf[off]
    vers, path = struct.unpack_from("<IQ", buf, off + 1)
    return Qid(qtype, vers, path), off + 13


def _parse_stat(buf: bytes, off: int) -> tuple[Stat, int]:
    """Parses one packed-stat entry starting at `off` (its own leading u16
    size field included), returning the entry and the offset just past it
    -- callers decoding a directory's concatenated stream loop on this."""
    (inner_size,) = struct.unpack_from("<H", buf, off)
    end = off + 2 + inner_size
    p = off + 2
    p += 2  # type (kernel-use only, ignored)
    p += 4  # dev (kernel-use only, ignored)
    qid, p = _parse_qid(buf, p)
    (mode,) = struct.unpack_from("<I", buf, p); p += 4
    (atime,) = struct.unpack_from("<I", buf, p); p += 4
    (mtime,) = struct.unpack_from("<I", buf, p); p += 4
    (length,) = struct.unpack_from("<Q", buf, p); p += 8
    name, p = _unpack_str(buf, p)
    uid, p = _unpack_str(buf, p)
    gid, p = _unpack_str(buf, p)
    muid, p = _unpack_str(buf, p)
    return Stat(qid, mode, atime, mtime, length, name, uid, gid, muid), end


class P9Client:
    """A single-connection, single-outstanding-request 9P client over any
    stream-like object exposing sendall/recv/settimeout/close (a raw
    socket, or transport.SerialSocketAdapter over a real serial port)."""

    def __init__(self, sock, framing: str = "raw") -> None:
        if framing not in ("raw", "slip"):
            raise ValueError(f"unknown framing {framing!r}")
        self._sock = sock
        self._next_tag = 1
        self._framing = framing
        self._pending = bytearray()  # unprocessed bytes, SLIP framing only

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
            raw = bytes(self._pending[: idx + 1])
            del self._pending[: idx + 1]
            decoded = slip_decode(raw)
            if decoded:
                return decoded

    def _recv_one_frame(self) -> tuple[int, int, bytes]:
        """Receives and parses exactly one reply frame, in whichever
        framing this connection uses. Returns (resp_type, resp_tag,
        resp_body) without checking the tag -- _roundtrip() does that,
        since it may need to read (and discard) more than one frame."""
        if self._framing == "slip":
            rest = self._recv_slip_frame()
            if len(rest) < 7:
                raise P9Error(f"malformed reply: {len(rest)} bytes < 7")
            size = struct.unpack_from("<I", rest, 0)[0]
            if size != len(rest):
                raise P9Error(f"declared size {size} != decoded length {len(rest)}")
            resp_type, resp_tag = struct.unpack_from("<BH", rest, 4)
            resp_body = rest[7:]
        else:
            size = struct.unpack("<I", self._recv_exact(4))[0]
            if size < 7:
                raise P9Error(f"malformed reply: declared size {size} < 7")
            rest = self._recv_exact(size - 4)
            resp_type, resp_tag = struct.unpack("<BH", rest[:3])
            resp_body = rest[3:]
        return resp_type, resp_tag, resp_body

    def _roundtrip(self, msg_type: int, body: bytes) -> tuple[int, bytes]:
        tag = self._tag()
        raw_frame = _frame(msg_type, tag, body)

        if self._framing == "slip":
            self._sock.sendall(slip_encode(raw_frame))
        else:
            self._sock.sendall(raw_frame)

        # This client only ever has one request outstanding at a time, so
        # any frame that comes back with the WRONG tag can only be a late
        # reply to a request WE already gave up on -- observed happening
        # for real over a physical CDC-ACM link (never over QEMU's
        # virtio-console, which is effectively instant): warm_up_9p()'s
        # retried Tversion can still be in flight when a retry gives up
        # and resends, so its late Rversion, and possibly more than one,
        # ends up queued ahead of a later, completely unrelated reply.
        # Discarding stale-tagged frames and reading on until the real one
        # (or a small bounded number of frames have been discarded) turns
        # that into a self-healing skip instead of a permanent desync --
        # raw framing has no other resync mechanism, so once a stale frame
        # is left unread ahead of a real one, every reply after it is
        # silently one frame behind, forever.
        stale = 0
        max_stale = 4
        while True:
            resp_type, resp_tag, resp_body = self._recv_one_frame()
            if resp_tag == tag:
                break
            stale += 1
            if stale > max_stale:
                raise P9Error(f"tag mismatch: sent {tag}, got {resp_tag}")

        if resp_type == RERROR:
            (elen,) = struct.unpack_from("<H", resp_body, 0)
            raise P9Error(resp_body[2 : 2 + elen].decode(errors="replace"))
        return resp_type, resp_body

    # --- Wire-level operations, one per 9P message type ---

    def version(self, msize: int = 4096) -> int:
        rtype, body = self._roundtrip(TVERSION, struct.pack("<I", msize) + _pack_str("9P2000"))
        if rtype != RVERSION:
            raise P9Error(f"expected Rversion, got type {rtype}")
        return struct.unpack_from("<I", body, 0)[0]

    def attach(self, fid: int, aname: str = "", uname: str = "lugal") -> Qid:
        body = struct.pack("<II", fid, NOFID) + _pack_str(uname) + _pack_str(aname)
        rtype, body = self._roundtrip(TATTACH, body)
        if rtype != RATTACH:
            raise P9Error(f"expected Rattach, got type {rtype}")
        qid, _ = _parse_qid(body, 0)
        return qid

    def walk(self, fid: int, newfid: int, names: list[str]) -> int:
        if len(names) > MAX_WALK_ELEM:
            raise P9Error(f"walk of {len(names)} elements exceeds server's MAX_WALK_ELEM={MAX_WALK_ELEM}")
        body = struct.pack("<IIH", fid, newfid, len(names))
        for n in names:
            body += _pack_str(n)
        rtype, resp = self._roundtrip(TWALK, body)
        if rtype != RWALK:
            raise P9Error(f"expected Rwalk, got type {rtype}")
        (nwqid,) = struct.unpack_from("<H", resp, 0)
        return nwqid

    def open(self, fid: int, mode: int = OREAD) -> Qid:
        rtype, body = self._roundtrip(TOPEN, struct.pack("<IB", fid, mode))
        if rtype != ROPEN:
            raise P9Error(f"expected Ropen, got type {rtype}")
        qid, _ = _parse_qid(body, 0)
        return qid

    def create(self, fid: int, name: str, perm: int = 0o644, mode: int = OWRITE) -> Qid:
        """Tcreate: `fid` must currently be a walked, unopened directory --
        on success the SAME fid is repurposed by the server to refer to the
        newly created entry (not a separate new fid), matching fs/9p.c's
        p9_handle_tcreate() exactly. Ready to write() immediately afterward
        if `perm` doesn't have DMDIR set."""
        if len(name) > MAX_NAME_LEN - 1:
            raise P9Error(f"name {name!r} exceeds server's MAX_NAME_LEN={MAX_NAME_LEN}")
        body = struct.pack("<I", fid) + _pack_str(name) + struct.pack("<IB", perm, mode)
        rtype, resp = self._roundtrip(TCREATE, body)
        if rtype != RCREATE:
            raise P9Error(f"expected Rcreate, got type {rtype}")
        qid, _ = _parse_qid(resp, 0)
        return qid

    def read(self, fid: int, offset: int, count: int) -> bytes:
        rtype, body = self._roundtrip(TREAD, struct.pack("<IQI", fid, offset, count))
        if rtype != RREAD:
            raise P9Error(f"expected Rread, got type {rtype}")
        (n,) = struct.unpack_from("<I", body, 0)
        return body[4 : 4 + n]

    def read_all(self, fid: int, chunk_size: int = 1024) -> bytes:
        """Reads a file (or, for a directory fid, the raw concatenated-stat
        byte stream -- see read_dir()) to EOF. Directory reads rely on the
        server's own opaque read-cursor convention (fs/9p.c's
        p9_read_dir_stream(): only offset==0 is meaningful, as "start over";
        any nonzero offset just continues its internal cursor) -- passing
        monotonically increasing offsets here (as an ordinary file read
        would) satisfies that convention for free."""
        out = b""
        offset = 0
        while True:
            chunk = self.read(fid, offset, chunk_size)
            if not chunk:
                return out
            out += chunk
            offset += len(chunk)

    def read_dir(self, fid: int, chunk_size: int = 1024) -> list[Stat]:
        """Reads an opened directory fid's full entry list."""
        raw = self.read_all(fid, chunk_size=chunk_size)
        entries: list[Stat] = []
        off = 0
        while off < len(raw):
            st, off = _parse_stat(raw, off)
            entries.append(st)
        return entries

    def write(self, fid: int, offset: int, data: bytes) -> int:
        body = struct.pack("<IQI", fid, offset, len(data)) + data
        rtype, resp = self._roundtrip(TWRITE, body)
        if rtype != RWRITE:
            raise P9Error(f"expected Rwrite, got type {rtype}")
        (n,) = struct.unpack_from("<I", resp, 0)
        return n

    def write_all(self, fid: int, data: bytes, chunk_size: int = 1024) -> int:
        offset = 0
        while offset < len(data):
            chunk = data[offset : offset + chunk_size]
            n = self.write(fid, offset, chunk)
            if n <= 0:
                break
            offset += n
        return offset

    def stat(self, fid: int) -> Stat:
        rtype, body = self._roundtrip(TSTAT, struct.pack("<I", fid))
        if rtype != RSTAT:
            raise P9Error(f"expected Rstat, got type {rtype}")
        # Rstat's body is [outer u16 count][the packed-stat entry], and the
        # entry itself starts with its OWN (redundant, but standard 9P
        # wire format) u16 size field -- skip the outer one, matching
        # fs/9p.c's own P9_RSTAT serialization (wcur_u16(count) then
        # wcur_bytes(the already-self-describing p9_pack_stat() output)).
        st, _ = _parse_stat(body, 2)
        return st

    def remove(self, fid: int) -> None:
        """Tremove: the fid is clunked by the server whether removal
        succeeds or fails (matching fs/9p.c's p9_handle_tremove()) -- do
        not clunk() it again afterward."""
        rtype, _ = self._roundtrip(TREMOVE, struct.pack("<I", fid))
        if rtype != RREMOVE:
            raise P9Error(f"expected Rremove, got type {rtype}")

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
            raise P9Error(
                f"walk to {path!r} only resolved {nwqid}/{len(names)} components "
                f"(stopped at {'/'.join(names[:nwqid]) or '/'})"
            )
        self.open(file_fid, mode=OREAD)
        data = self.read_all(file_fid)
        self.clunk(file_fid)
        return data


class Session:
    """Path-based convenience layer over P9Client -- the actual "file
    utility" surface this phase is about, so callers don't have to juggle
    fids/walks by hand. Deliberately uses only a small, fixed handful of
    fids (the server's own P9_MAX_FIDS is 8, and its own comment notes it
    expects one peer at a time -- a Session that hoarded fids across many
    open paths would starve itself, let alone anyone else)."""

    _ROOT_FID = 1
    _WORK_FID = 2

    def __init__(self, client: P9Client, aname: str = "") -> None:
        self.client = client
        self.client.version()
        self.client.attach(self._ROOT_FID, aname=aname)

    def close(self) -> None:
        self.client.close()

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    @staticmethod
    def _split(path: str) -> list[str]:
        return [p for p in path.split("/") if p]

    def _walk_or_raise(self, fid: int, names: list[str], path: str, what: str) -> None:
        """Shared by _walk_to() and every direct self.client.walk() call
        below: walks `names` onto `fid` and raises if it doesn't fully
        resolve. A partial walk (some names resolved, not all -- only
        possible for failures past the very first component; failing at
        the first is always an outright Rerror, raised inside walk()
        itself, before nwqid is even returned here) still leaves `fid`
        bound to wherever it stopped -- fs/9p.c's p9_handle_twalk() only
        rejects a *fresh* Twalk whose newfid number is already in use, not
        one that's genuinely still walking, so a caller that doesn't
        clunk it back here leaks that fid permanently. Since Session only
        ever has _WORK_FID to give out, one leaked partial walk (e.g.
        stat()'ing a path whose last component doesn't exist yet) used to
        wedge the entire Session for every call after it."""
        nwqid = self.client.walk(self._ROOT_FID, fid, names)
        if nwqid != len(names):
            if nwqid > 0:
                self.client.clunk(fid)
            stopped_at = "/".join(names[:nwqid]) or "/"
            raise P9Error(
                f"{what} {path!r} only resolved {nwqid}/{len(names)} components "
                f"(stopped at {stopped_at})"
            )

    def _walk_to(self, path: str, fid: int = _WORK_FID) -> list[str]:
        names = self._split(path)
        self._walk_or_raise(fid, names, path, "walk to")
        return names

    def read(self, path: str) -> bytes:
        self._walk_to(path)
        self.client.open(self._WORK_FID, mode=OREAD)
        try:
            return self.client.read_all(self._WORK_FID)
        finally:
            self.client.clunk(self._WORK_FID)

    def write(self, path: str, data: bytes, create: bool = True, truncate: bool = True) -> int:
        """Writes `data` to `path`, starting at offset 0. `create=True`
        (the default) creates the file if it doesn't exist yet -- walked
        from its parent directory, since Tcreate needs a directory fid, not
        the file itself. `truncate=True` overwrites existing content past
        what's written (the file's prior length is not otherwise reset by
        Twrite, which only ever extends/overwrites at the given offset)."""
        parts = self._split(path)
        if not parts:
            raise P9Error("cannot write to '/'")
        parent, name = parts[:-1], parts[-1]

        self._walk_or_raise(self._WORK_FID, parent, path, "parent directory of")

        # Walk WORK_FID (now at the parent) onto the final component, IN
        # PLACE (fid == newfid is explicitly legal -- fs/9p.c's
        # p9_handle_twalk() special-cases it). A single-element walk either
        # fully succeeds or errors outright (the server only tolerates a
        # partial walk past the FIRST component; failing at i==0 -- exactly
        # what a 1-element walk risks -- is always an Rerror, never a
        # zero-nwqid Rwalk), so catching P9Error here is an unambiguous
        # "doesn't exist", not a guess. On failure WORK_FID is left
        # unaffected (per spec) -- still positioned at the parent, ready
        # for Tcreate.
        exists = True
        try:
            self.client.walk(self._WORK_FID, self._WORK_FID, [name])
        except P9Error:
            exists = False

        try:
            if exists and truncate:
                # Twrite never shortens a file on its own; recreating gives
                # the same truncating effect as P9_OTRUNC, since this
                # server's Tcreate already truncates (VFS_O_TRUNC).
                self.client.clunk(self._WORK_FID)
                self.client.walk(self._ROOT_FID, self._WORK_FID, parent)
                self.client.create(self._WORK_FID, name, perm=0o644, mode=OWRITE)
            elif exists:
                self.client.open(self._WORK_FID, mode=OWRITE)
            elif create:
                self.client.create(self._WORK_FID, name, perm=0o644, mode=OWRITE)
            else:
                raise P9Error(f"{path!r} does not exist and create=False")
            return self.client.write_all(self._WORK_FID, data)
        finally:
            self.client.clunk(self._WORK_FID)

    def remove(self, path: str) -> None:
        self._walk_to(path)
        self.client.remove(self._WORK_FID)  # clunks itself, per Tremove semantics

    def mkdir(self, path: str) -> None:
        parts = self._split(path)
        if not parts:
            raise P9Error("cannot mkdir '/'")
        parent, name = parts[:-1], parts[-1]
        self._walk_or_raise(self._WORK_FID, parent, path, "parent directory of")
        try:
            self.client.create(self._WORK_FID, name, perm=DMDIR | 0o755, mode=OREAD)
        finally:
            self.client.clunk(self._WORK_FID)

    def stat(self, path: str) -> Stat:
        self._walk_to(path)
        try:
            return self.client.stat(self._WORK_FID)
        finally:
            self.client.clunk(self._WORK_FID)

    def listdir(self, path: str) -> list[Stat]:
        self._walk_to(path)
        self.client.open(self._WORK_FID, mode=OREAD)
        try:
            return self.client.read_dir(self._WORK_FID)
        finally:
            self.client.clunk(self._WORK_FID)
