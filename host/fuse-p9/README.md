# fuse-p9

Mounts a LugalOS board's entire 9P namespace (`/sd0`, `/proc`, `/ram0`,
`/flash0`, ... -- whatever `ls /` shows on the board itself) as a real
directory on the host, via FUSE. Built directly on
[`host/p9lib`](../p9lib/README.md) -- no separate protocol implementation,
just a `fusepy` `Operations` class wrapping a single `p9lib.Session`.

Linux is where this is verified end to end. macOS support (via macFUSE)
is written -- `fusepy` is the same package on both, the code has no
Linux-specific assumptions left in it -- but is **untested past the mount
call itself**: on Apple Silicon under macOS's default "Full Security" boot
policy, activating any third-party system extension (macFUSE included)
needs a trip through Recovery Mode to lower that policy first, which is a
real, deliberate security tradeoff on the user's own machine and not
something to talk anyone into. Treat macOS as "should work, not verified
working" until someone confirms it on a machine where that trade has
already been made for other reasons.

## Install

```bash
cd host/fuse-p9
uv sync   # pulls in fusepy and p9lib[serial] from the sibling ../p9lib package
```

**Linux** additionally needs `libfuse` (`fuse2` or `fuse3`, either works),
from the system package manager -- `apt install libfuse2` / `libfuse3-3`
or equivalent. Nothing else to configure; a fresh install works immediately.

**macOS** additionally needs [macFUSE](https://macfuse.github.io/)
(`brew install macfuse`, or the `.pkg` from that site), and getting it
actually active is more involved than the install:

1. **Installing macFUSE is not enough -- it needs a one-time system
   extension approval**, separate from the install and *not* granted by a
   reboot on its own. Trigger the prompt by attempting a mount once
   (`uv run lugal9pfuse ...` below), or go to **System Settings ->
   General -> Login Items & Extensions -> Driver Extensions** directly.
   Confirm it actually took with:
   ```bash
   systemextensionsctl list | grep -i fuse
   ```
   A mount attempt before approval fails with exactly `RuntimeError: 1` /
   `mount_macfuse: the file system is not available`, indistinguishable
   from a genuine fault without this context.
2. **On Apple Silicon under the default security policy, step 1's prompt
   never succeeds at all** -- `systemextensionsctl` shows no entry no
   matter how many times "Allow" is clicked, and a reboot changes
   nothing. The actual fix is **Recovery Mode -> Startup Security
   Utility -> Security Policy -> Reduced Security -> allow user
   management of kernel extensions from identified developers**, then
   reboot again and redo step 1. This is the real reason macOS is listed
   as untested above: it is a legitimate cost, not a checkbox, and
   whether to pay it is the user's call.
3. **Unmounting uses plain `umount`, not `fusermount`** -- macOS has no
   `fusermount` binary at all (that's Linux-specific, from
   `libfuse`/`fuse-utils`); `umount /tmp/lugalos` is the macOS equivalent.

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
fusermount -u /tmp/lugalos   # Linux; macOS: umount /tmp/lugalos
# or Ctrl-C the lugal9pfuse process
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
