"""fusepy Operations implementation exposing a LugalOS 9P namespace as a
real host directory. Wraps a single, shared p9lib.Session -- deliberate,
not a placeholder: the 9P server behind it has a tiny fixed fid table
(P9_MAX_FIDS = 8, see fs/include/fs/9p.h) and Session already serializes
itself down to two fids for exactly that reason.

Every method below is one coarse `with self._lock:` block, and cli.py runs
FUSE in its normal multi-threaded mode (no nothreads=True) -- the lock is
what keeps the single Session safe, not single-threaded dispatch. That's
a deliberate change from this file's first version, which used
nothreads=True instead: a real, previously-legitimate-looking backend
operation can take several seconds on real hardware (e.g. /proc/df, which
does a genuine full FAT-table free-space scan -- see fs/fat32.c's
fat32_statfs()), and with single-threaded dispatch that one slow call
blocks libfuse's *entire* request queue for its duration, including
completely unrelated requests for other files. Observed on real RP2350
hardware (never over QEMU, which has no directory expensive enough to
notice): a `tree` walk would stall for several seconds on /proc, then
fail outright opening /sd0, and every FUSE call after that failed too
until the mount was redone -- consistent with the kernel giving up on (or
otherwise mishandling) a request that sat unanswered too long while
nothreads=True left nothing else able to run. Multi-threaded dispatch
lets libfuse keep servicing other requests immediately; the lock ensures
they still queue safely for the one thing that actually may not run
concurrently, talking to the board.

9P doesn't give this filesystem everything POSIX wants: there's no
Twstat support server-side (no rename, no chmod/chown persistence, no
mtimes -- fs/9p.c's Stat always reports atime/mtime as 0), and no
symlinks or special files. Those are declared absent up front (rename
raises ENOSYS; chmod/chown/utimens are silent no-ops so ordinary tools
like `cp -p` don't hard-fail) rather than half-emulated.

Reads and writes are whole-file, buffered in memory between open() and
release()/flush(): open() (or create()) fetches or starts a bytearray,
write() patches it in place, and the buffer is only ever sent back over
9P as a single Session.write() call. Simple, and correct for arbitrary
offsets -- just not efficient for a file too big to comfortably hold in
RAM, which is a fine trade-off for what LugalOS's own SD cards actually
hold."""

from __future__ import annotations

import errno
import os
import stat as statmod
import threading
import time

from fuse import FuseOSError, Operations

import p9lib


class P9FS(Operations):
    def __init__(self, session: p9lib.Session) -> None:
        self.sess = session
        self._lock = threading.Lock()
        self._uid = os.getuid()
        self._gid = os.getgid()
        self._handles: dict[int, bytearray] = {}
        self._dirty: set[int] = set()
        self._next_fh = 1

    # --- helpers (always called with self._lock already held) ---

    def _alloc_fh(self, initial: bytes) -> int:
        fh = self._next_fh
        self._next_fh += 1
        self._handles[fh] = bytearray(initial)
        return fh

    def _attrs(self, st: p9lib.Stat) -> dict:
        now = time.time()
        mode = (statmod.S_IFDIR | 0o755) if st.is_dir else (statmod.S_IFREG | 0o644)
        return {
            "st_mode": mode,
            "st_nlink": 2 if st.is_dir else 1,
            "st_size": 0 if st.is_dir else st.length,
            "st_uid": self._uid,
            "st_gid": self._gid,
            # The server has no wall clock (Stat.atime/mtime are always 0)
            # -- reporting "now" rather than the epoch avoids tools that
            # treat a 1970 mtime as "obviously stale" misbehaving.
            "st_atime": now,
            "st_mtime": now,
            "st_ctime": now,
        }

    # --- metadata ---

    def getattr(self, path, fh=None):
        with self._lock:
            try:
                st = self.sess.stat(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            return self._attrs(st)

    def readdir(self, path, fh):
        with self._lock:
            try:
                entries = self.sess.listdir(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            return [".", ".."] + [e.name for e in entries]

    def statfs(self, path):
        # No real block/inode accounting to report (FAT32 free-space isn't
        # exposed over 9P) -- fixed plausible-looking numbers so `df`/`stat
        # -f` don't error, not a real measurement. No Session access, so
        # no lock needed.
        return {
            "f_bsize": 4096,
            "f_frsize": 4096,
            "f_blocks": 1 << 20,
            "f_bfree": 1 << 19,
            "f_bavail": 1 << 19,
            "f_files": 1 << 16,
            "f_ffree": 1 << 15,
            "f_namemax": 32,  # P9_MAX_NAME_LEN - 1
        }

    # --- files ---

    def open(self, path, flags):
        with self._lock:
            try:
                data = b"" if (flags & os.O_TRUNC) else self.sess.read(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            return self._alloc_fh(data)

    def create(self, path, mode, fi=None):
        with self._lock:
            try:
                self.sess.write(path, b"", create=True, truncate=True)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.EIO) from e
            return self._alloc_fh(b"")

    def read(self, path, size, offset, fh):
        with self._lock:
            buf = self._handles[fh]
            return bytes(buf[offset : offset + size])

    def write(self, path, data, offset, fh):
        with self._lock:
            buf = self._handles[fh]
            if len(buf) < offset:
                buf.extend(b"\x00" * (offset - len(buf)))
            buf[offset : offset + len(data)] = data
            self._dirty.add(fh)
            return len(data)

    def truncate(self, path, length, fh=None):
        with self._lock:
            if fh is not None and fh in self._handles:
                buf = self._handles[fh]
                if len(buf) < length:
                    buf.extend(b"\x00" * (length - len(buf)))
                else:
                    del buf[length:]
                self._dirty.add(fh)
                return
            try:
                data = self.sess.read(path) if length > 0 else b""
            except p9lib.P9Error:
                data = b""
            data = data[:length] + b"\x00" * max(0, length - len(data))
            try:
                self.sess.write(path, data, create=True, truncate=True)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.EIO) from e

    def _commit(self, path, fh):
        # Always called with self._lock already held.
        if fh in self._dirty:
            try:
                self.sess.write(path, bytes(self._handles[fh]), create=True, truncate=True)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.EIO) from e
            self._dirty.discard(fh)

    def flush(self, path, fh):
        with self._lock:
            self._commit(path, fh)
            return 0

    def release(self, path, fh):
        with self._lock:
            self._commit(path, fh)
            self._handles.pop(fh, None)
            return 0

    # --- namespace changes ---

    def mkdir(self, path, mode):
        with self._lock:
            try:
                self.sess.mkdir(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.EIO) from e

    def unlink(self, path):
        with self._lock:
            try:
                self.sess.remove(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e

    def rmdir(self, path):
        with self._lock:
            try:
                self.sess.remove(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e

    def rename(self, old, new):
        # fs/9p.c has no Twstat handler -- there is no server-side rename
        # to call. Emulating it as read+write+remove would silently
        # discard a real directory move, so this is refused outright
        # rather than half-working. No Session access, no lock needed.
        raise FuseOSError(errno.ENOSYS)

    # --- POSIX metadata the server has no model for: accepted, not stored ---

    def chmod(self, path, mode):
        return 0

    def chown(self, path, uid, gid):
        return 0

    def utimens(self, path, times=None):
        return 0
