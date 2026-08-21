# fuse-p9

Mounts a LugalOS board's entire 9P namespace (`/sd0`, `/proc`, `/ram0`,
`/flash0`, ... -- whatever `ls /` shows on the board itself) as a real
directory on a Linux host, via FUSE. Built directly on
[`host/p9lib`](../p9lib/README.md) -- no separate protocol implementation,
just a `fusepy` `Operations` class wrapping a single `p9lib.Session`.

Linux only for now (needs `libfuse`; `fuse2`/`fuse3` both work). macOS
support (via macFUSE) is a possible later addition, not done here.

## Install

```bash
cd host/fuse-p9
uv sync   # pulls in fusepy and p9lib[serial] from the sibling ../p9lib package
```

## Use

```bash
mkdir -p /tmp/lugalos
uv run lugal9pfuse --serial /dev/ttyACM1 /tmp/lugalos

# in another shell, once mounted:
ls /tmp/lugalos           # flash0  sd0  ram0  proc  dev  srv
cat /tmp/lugalos/sd0/system/etc/init.lisp
echo 'hello' > /tmp/lugalos/sd0/hello.txt
mkdir /tmp/lugalos/sd0/a_directory
cp somefile /tmp/lugalos/sd0/a_directory/

# unmount from anywhere:
fusermount -u /tmp/lugalos   # or Ctrl-C the lugal9pfuse process
```

Against QEMU instead of real hardware, use `--unix <chardev socket path>` in
place of `--serial` (see `tests/runner.py`'s `-chardev socket,...`
invocations), and `--framing slip` for a UART link instead of
virtio-console/USB-CDC's own framing (the default, `raw`).

`lugal9pfuse` runs in the foreground and blocks until unmounted.

## What works, and what doesn't

**Works:** browsing the whole namespace, reading and writing files of any
size, creating and removing files and directories, arbitrary-offset
writes/truncation within a single open/write/close cycle. Ordinary tools
(`cat`, `cp`, `ls`, editors, `find`) work unmodified against the mount.

**Doesn't, deliberately:**
- **Rename** (`mv`) -- fs/9p.c has no `Twstat` handler to call. Emulating a
  rename as copy+delete would silently turn a real directory move into
  something that behaves very differently (and can partially fail), so
  this is refused outright (`ENOSYS`) rather than faked.
- **Permissions and timestamps** -- the server has no permission model and
  no wall clock (`Stat.mtime`/`atime` are always 0 on the wire).
  `chmod`/`chown`/`utimens` are accepted as silent no-ops (so `cp -p` and
  similar don't hard-fail) but nothing is actually stored.
- **Symlinks and special files** -- not a concept this filesystem has.

One quirk worth knowing about, not a bug: some entries an on-board
namespace *lists* (e.g. `/srv` on the stock namespace) may not actually be
walkable if nothing is currently bound there -- that shows up as a `?????`
row in `ls -la` for that one name, exactly reflecting what the 9P server
itself says.

## Design: one shared Session, single-threaded

`P9FS` (`src/fuse_p9/operations.py`) wraps exactly one `p9lib.Session`,
and `cli.py` runs `FUSE(..., nothreads=True)` -- libfuse dispatches one
operation at a time, so the Session (which itself only ever uses two fids,
matching the server's small `P9_MAX_FIDS = 8`) never needs its own
locking. This is a deliberate simplicity choice for a first version, not
an oversight: giving each mount a richer fid pool for real concurrent
access is future work if it's ever needed, not something to build in from
the start.

Reads and writes are whole-file, buffered in memory between `open()`/
`create()` and `release()`: correct for arbitrary offsets, but means a
file has to comfortably fit in host RAM. Fine for what LugalOS's own SD
cards actually hold; would need reworking for anything bigger.

## A bug this found in `p9lib` itself

Building this surfaced a real bug in `host/p9lib`'s `Session`, not
anything FUSE-specific: `stat()`-ing (or reading, or writing to) a path
whose *last* component doesn't exist yet -- exactly what FUSE's `getattr()`
does before every `create()` -- is a partial `Twalk` (the parent resolves,
the leaf doesn't). The server leaves the fid bound to the parent in that
case, but `Session` never clunked it back before raising, so the *first*
such failure permanently wedged the shared work fid ("`walk: newfid
already in use`") for every call after it. Fixed in `p9lib/client.py`'s
new `_walk_or_raise()` helper, used by every walk that can fail partway;
see its docstring for the exact reasoning. Covered by a regression check
in `tests/runner.py`'s `test_9p_crud_via_p9lib` (A6).
