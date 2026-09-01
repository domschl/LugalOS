# fuse-p9

Mounts a LugalOS board's entire 9P namespace (`/sd0`, `/proc`, `/ram0`,
`/flash0`, ... -- whatever `ls /` shows on the board itself) as a real
directory on the host, via FUSE. Built directly on
[`host/p9lib`](../p9lib/README.md) -- no separate protocol implementation,
just a `fusepy` `Operations` class wrapping a single `p9lib.Session`.

Linux is where this is verified end to end -- over TCP, against a Pico 2 W
on WiFi, on 2026-09-01 (see "Verified" below). macOS support (via macFUSE)
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

### Over the network, instead of a cable

A board that is on an IP network -- WiFi (`rp2350-wifi`) or wired Ethernet
(`rp2350-gateway`'s ENC28J60) -- is mounted with `--tcp` instead of
`--serial`. The two are the same command apart from the address: both
personas present the same `netif_t` to the same IP stack, TCP and 9P
server, so nothing above the driver knows which one it is talking to.

```bash
# a network link requires 9P authentication -- see the root README's
# "Serve 9P over the air"; the board needs `p9key <hex>` and `net listen 564`
printf '000102030405060708090a0b0c0d0e0f' > ~/.lugal9p.key && chmod 600 ~/.lugal9p.key

uv run lugal9pfuse --key-file ~/.lugal9p.key --tcp 192.168.178.21 /tmp/lugalos  # WiFi
uv run lugal9pfuse --key-file ~/.lugal9p.key --tcp 192.168.77.2  /tmp/lugalos   # Ethernet
```

`--tcp` takes `HOST` or `HOST:PORT`, port 564 by default. Prefer
`--key-file` over `--key`: a mount lives until it is unmounted, and a
secret passed on the command line is visible in `ps` and in shell history
for that entire time. Omit both on a link with no key installed.

Against QEMU instead of real hardware, use `--unix <chardev socket path>` in
place of `--serial` (see `tests/runner.py`'s `-chardev socket,...`
invocations), and `--framing slip` for a UART link instead of
virtio-console/USB-CDC's own framing (the default, `raw`). `--tcp` works
against QEMU too, through slirp's `hostfwd` -- wire it the way
`tests/runner.py`'s `test_9p_over_tcp` does
(`-netdev user,id=n0,hostfwd=tcp:127.0.0.1:<port>-10.0.2.15:564`), then
mount `127.0.0.1:<port>`. Checked on rv32, so a full mount can be
exercised with no hardware at all.

`lugal9pfuse` runs in the foreground and blocks until unmounted.

## What works, and what doesn't

**Works:** browsing the whole namespace, reading and writing files of any
size, creating and removing files and directories, arbitrary-offset
writes/truncation within a single open/write/close cycle. Ordinary tools
(`cat`, `cp`, `ls`, editors, `find`) work unmodified against the mount.

**Doesn't, deliberately:**
- **Directory rename** (`mv` of a directory) -- fs/9p.c has no `Twstat`
  handler, so there is no server-side rename to call. Faking one by
  recursively copying a whole tree over 9P and then removing the original
  is slow, easy to get subtly wrong, and a directory move is a rare,
  deliberate act, so it is refused (`ENOSYS`) rather than emulated.
  **File** rename *is* emulated (read, write to the new name, remove the
  old) -- not optional, because write-temp-then-rename is the automatic
  save strategy of nearly every real editor, and refusing it silently
  broke every one of them. See `operations.py`'s `rename()`.
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

## Verified

Over TCP, on Linux, against a Pico 2 W on WiFi (`rp2350-wifi`) with an SD
card, 2026-09-01 -- the first end-to-end run of this tool, since the
machine it was written on could not activate macFUSE (see above). The
board was at `192.168.178.21:564` with an authenticated link; every check
below went over the air, not down a cable:

- mount and unmount, clean both ways -- `fusermount -u`, empty log, process
  exits on its own
- `ls` of the whole namespace; `/proc` (12 synthetic files), `/flash0`
  (read-only), `/sd0`, `/dev`; `/srv` shows the documented `?????` row
- `find` across the entire mount, no errors -- 32 files that are always
  there (`/dev` 4, `/flash0` 16, `/proc` 12) plus whatever the card holds
- a 32 KB file copied onto `/sd0` and read back SHA-256-identical
  (~20 KB/s over the radio -- 1.5-1.6 s for the 32 KB, across two runs)
- `mkdir`, cross-volume `cp` (`/flash0` -> `/sd0`) byte-identical, `rm`,
  `rmdir`
- arbitrary-offset write inside one open/write/close cycle
- the editor save pattern (write temp, rename over the target) -- works;
  a *directory* rename is refused with `ENOSYS`, as intended
- a write to read-only `/flash0` fails, as it should
- four concurrent reader threads, twenty round trips, no errors -- the
  lock described below doing its job
- board-side counters afterwards: 960 KB tx, 236 KB rx, 0 resets

**It found one bug, server-side:** FAT32 keeps `.` and `..` as real on-disk
entries in every subdirectory, and `fs/9p.c`'s `p9_read_dir_stream()`
streamed them onto the wire. A 9P2000 directory read carries contents only
-- a client reaches the parent by *walking* the name `..`, never by finding
it in a listing -- so this made every 9P client's tree walk
self-referential: `ls -la` of any subdirectory showed `.` and `..` twice
(once from the server, once from the pair FUSE prepends itself), and a
recursive `p9lib` walk would not have terminated. Fixed in `fs/9p.c`, with
a regression check in `tests/runner.py`'s `test_9p_crud_via_p9lib`;
`readdir()` here also filters the pair defensively, so a board running
older firmware still lists correctly.

The fixed firmware was then flashed to the same board and the whole run
repeated. The server was checked *directly*, through `p9lib` with no FUSE
in the path, so the defensive filter above could not mask a regression:
every subdirectory on both volumes now lists its contents only (an empty
one returns `[]`, where it used to return `['.', '..']`), while `.` and
`..` stay fully walkable -- `stat` on both, and a read through
`DEEPER/../inner.txt`. The mount was then run once more with the
defensive filter temporarily backed out, confirming the server fix alone
is sufficient: one `.` and one `..` per directory, across five nested
directories on both volumes.

## Design: one shared Session, one lock

`P9FS` (`src/fuse_p9/operations.py`) wraps exactly one `p9lib.Session` --
which itself only ever uses two fids, matching the server's small
`P9_MAX_FIDS = 8` -- and guards every use of it with a single lock.

`cli.py` deliberately does *not* pass `nothreads=True`. An earlier version
did, on the reasoning that single-threaded dispatch makes the lock
unnecessary; that turned out to be a stability bug rather than a saved
lock, because libfuse then serialises the whole mount behind whichever
operation is in flight, and one slow but perfectly legitimate real-hardware
request (`/proc/df`'s FAT-table scan took 7-13 s on real cards) froze
everything else using the mount for its full duration. So: threads from
libfuse, one lock here, one Session on the wire. Four concurrent readers
were exercised against real hardware, see "Verified" above.

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
