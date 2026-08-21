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
Twstat support server-side (no real rename, no chmod/chown persistence,
no mtimes -- fs/9p.c's Stat always reports atime/mtime as 0), and no
symlinks or special files. chmod/chown/utimens are silent no-ops so
ordinary tools like `cp -p` don't hard-fail. rename() *is* emulated for
files (read the source, write it over the destination, remove the
source) despite there being no server-side primitive for it -- not
optional: it's the routine, automatic save strategy nearly every real
editor uses (write a temp file, then rename it over the real target),
and refusing it outright (an earlier version of this file did exactly
that) silently broke every ordinary editor save. Directory renames are
still refused (ENOSYS) -- recursively copying a whole tree over 9P to
fake one is a much bigger, riskier undertaking for something that's a
rare, deliberate action rather than an automatic one.

Every path's reported mtime is stable for the life of the mount unless
*this* session actually wrote, created, or renamed it (see _touch()) --
not time.time() freshly on every stat() call, which an earlier version
of this file did and which broke any tool that compares mtimes to decide
whether a file changed externally (concretely: Emacs's save path always
saw a "different" mtime and always warned the file had changed on disk,
regardless of whether anything actually had).

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
    # fs/vfs_server.c's vfs_pread() services /dev/uart with a bare
    # uart_getc(), which blocks on the real physical UART line until a
    # byte actually arrives -- correct, expected behavior for a real
    # serial device, but catastrophic for anything that opens it
    # incidentally rather than deliberately: this project's 9P server
    # appears to process one client request to completion (including
    # whatever blocking IPC it triggers) before reading its next one, so
    # a stuck read here doesn't just block the requesting client, it
    # stalls the *entire* 9P server -- every other request on the mount,
    # forever, recoverable only by power-cycling the board. Confirmed
    # live: GNOME Files (Nautilus) auto-opens+reads a little of every
    # "regular-looking" file it lists to sniff its MIME type, and that
    # alone was enough to trigger this just from browsing to /dev/.
    # Reporting /dev/* as a character device (S_IFCHR) to stop that
    # sniffing was tried and reverted -- see _attrs()'s comment -- so
    # this is refused directly at open()/truncate() instead: still a
    # visible, listable, stat-able regular file (ls/tree are unaffected,
    # since neither touches vfs_pread()), just never actually readable
    # through this bridge, deliberately or not.
    _NEVER_OPEN = frozenset({"/dev/uart"})

    def __init__(self, session: p9lib.Session) -> None:
        self.sess = session
        self._lock = threading.Lock()
        self._uid = os.getuid()
        self._gid = os.getgid()
        self._handles: dict[int, bytearray] = {}
        self._dirty: set[int] = set()
        self._next_fh = 1
        # See _attrs()'s comment: every path's reported mtime/ctime is this
        # mount's own start time until _touch() below records that *this*
        # session actually modified it -- never time.time() on every
        # stat(), which used to report a file as having just changed on
        # literally every single check.
        self._mount_time = time.time()
        self._mtimes: dict[str, float] = {}

    # --- helpers (always called with self._lock already held) ---

    def _alloc_fh(self, initial: bytes) -> int:
        fh = self._next_fh
        self._next_fh += 1
        self._handles[fh] = bytearray(initial)
        return fh

    def _touch(self, path: str) -> None:
        self._mtimes[path] = time.time()

    def _untouch(self, path: str) -> None:
        self._mtimes.pop(path, None)

    def _attrs(self, path: str, st: p9lib.Stat) -> dict:
        # Reporting /dev/* as S_IFCHR (character special) instead of
        # S_IFREG was tried and reverted: it does stop file managers from
        # auto-probing their content, but FUSE mounts a non-root user
        # creates always carry the kernel's own "nodev" option (there is
        # no way to opt out of it without CAP_SYS_ADMIN), and under nodev
        # the kernel refuses to even open() anything reported as a device
        # node -- "Permission denied" for a deliberate `cat` too, not just
        # for automatic sniffing. See open()'s own guard for the actual
        # fix for the one path (/dev/uart) that needed one.
        mode = (statmod.S_IFDIR | 0o755) if st.is_dir else (statmod.S_IFREG | 0o644)
        # The server has no wall clock at all (Stat.atime/mtime are
        # always 0), so there's no real timestamp to report -- but
        # reporting time.time() fresh on every single stat() call (an
        # earlier version of this method did exactly that) is actively
        # wrong, not just imprecise: every path's mtime then changes on
        # every check, so any tool comparing "has this changed since I
        # last looked" -- Emacs's save path being the concrete case that
        # found this -- always sees a mismatch and always concludes the
        # file was modified externally, even though nothing touched it.
        # A path this session hasn't written gets one fixed value (this
        # mount's own start time) for its entire lifetime; _touch() records
        # a fresh timestamp only for a path *this* session actually wrote,
        # created, or renamed, so those correctly look "just modified"
        # without every untouched path also appearing to change forever.
        mtime = self._mtimes.get(path, self._mount_time)
        return {
            "st_mode": mode,
            "st_nlink": 2 if st.is_dir else 1,
            "st_size": 0 if st.is_dir else st.length,
            "st_uid": self._uid,
            "st_gid": self._gid,
            "st_atime": mtime,
            "st_mtime": mtime,
            "st_ctime": mtime,
        }

    # --- metadata ---

    def getattr(self, path, fh=None):
        with self._lock:
            try:
                st = self.sess.stat(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            return self._attrs(path, st)

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
        if path in self._NEVER_OPEN:
            raise FuseOSError(errno.EIO)
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
            self._touch(path)
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
        if fh is None and path in self._NEVER_OPEN:
            raise FuseOSError(errno.EIO)
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
            self._touch(path)

    def _commit(self, path, fh):
        # Always called with self._lock already held.
        if fh in self._dirty:
            try:
                self.sess.write(path, bytes(self._handles[fh]), create=True, truncate=True)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.EIO) from e
            self._dirty.discard(fh)
            self._touch(path)

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
            self._touch(path)

    def unlink(self, path):
        with self._lock:
            try:
                self.sess.remove(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            self._untouch(path)

    def rmdir(self, path):
        with self._lock:
            try:
                self.sess.remove(path)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            self._untouch(path)

    def rename(self, old, new):
        # fs/9p.c has no Twstat handler -- there is no server-side rename
        # to call, so this can't be a real atomic namespace operation.
        # For a *directory*, emulating one by recursively copying the
        # whole tree over 9P and then removing the original would be
        # slow, is easy to get subtly wrong, and a directory rename is a
        # rare, deliberate action anyway -- still refused outright.
        #
        # A *file* rename is different: it's the routine, automatic save
        # strategy nearly every real editor uses (write the new content
        # to a temp file, then rename it over the real target) --
        # confirmed concretely with Emacs, whose default save silently
        # never landed at all once this unconditionally refused every
        # rename. Emulating it here (read the temp file's already-
        # committed content, write it over `new`, remove `old`) isn't
        # atomic the way a real rename is, but the temp file's own
        # write+close already happened before this call, so the content
        # being moved is never in question -- worth the small window of
        # non-atomicity to make ordinary editor saves actually work.
        with self._lock:
            try:
                st = self.sess.stat(old)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.ENOENT) from e
            if st.is_dir:
                raise FuseOSError(errno.ENOSYS)
            try:
                data = self.sess.read(old)
                self.sess.write(new, data, create=True, truncate=True)
                self.sess.remove(old)
            except p9lib.P9Error as e:
                raise FuseOSError(errno.EIO) from e
            self._untouch(old)
            self._touch(new)

    # --- POSIX metadata the server has no model for: accepted, not stored ---

    def chmod(self, path, mode):
        return 0

    def chown(self, path, uid, gid):
        return 0

    def utimens(self, path, times=None):
        return 0
