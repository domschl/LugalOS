# p9lib

A real, general-purpose 9P2000 client for LugalOS's filesystem server
(`fs/9p.c`) — read **and** write access to a running board's filesystems
from a host machine (macOS/Linux), over whatever transport is already
plugged in: RP2350's USB-CDC "net" port (`/dev/ttyACM1`-ish), a UART/SLIP
link, or (for development without hardware) a QEMU virtio-console unix
socket.

This started as `tests/p9lib.py` (read-only, test-only) and is promoted
here as the actual product: `tests/runner.py` and `tests/hw/` both import
it from here now, so protocol fixes only ever need to happen in one place.

## Install

```bash
cd host/p9lib
uv sync --extra serial   # pulls in pyserial for real-hardware use;
                          # omit --extra serial for QEMU-only / library use
```

## CLI

```bash
uv run lugal9p --serial /dev/ttyACM1 ls /sd0
uv run lugal9p --serial /dev/ttyACM1 cat /sd0/system/etc/init.lisp
uv run lugal9p --serial /dev/ttyACM1 get /sd0/games/latest.pgn ./latest.pgn
uv run lugal9p --serial /dev/ttyACM1 put ./analysis.lisp /sd0/remote/analysis.lisp
uv run lugal9p --serial /dev/ttyACM1 mkdir /sd0/remote
uv run lugal9p --serial /dev/ttyACM1 rm /sd0/remote/old.txt
uv run lugal9p --serial /dev/ttyACM1 stat /sd0/games/latest.pgn
```

Against a QEMU target instead of real hardware, use `--unix <chardev socket
path>` in place of `--serial` (see `tests/runner.py`'s `-chardev
socket,...` invocations for how a QEMU target exposes one), and add
`--framing slip` when talking to a UART-based link instead of
virtio-console/USB-CDC's own framing (the default, `raw`).

## Library

```python
import p9lib

client = p9lib.connect_serial("/dev/ttyACM1")
with p9lib.Session(client, aname="/") as sess:
    for entry in sess.listdir("/sd0"):
        print(entry.name, entry.length, entry.is_dir)
    sess.write("/sd0/hello.txt", b"hello from a host\n")
    print(sess.read("/sd0/hello.txt"))
    sess.remove("/sd0/hello.txt")
```

`Session` is the path-based convenience layer (`read`/`write`/`remove`/
`mkdir`/`stat`/`listdir`) that does the fid/walk bookkeeping for you. The
lower-level `P9Client` (`version`/`attach`/`walk`/`open`/`read`/`write`/
`create`/`remove`/`stat`/`clunk`) is there if you need to manage fids
yourself — the server has a small, fixed fid table (`P9_MAX_FIDS = 8`),
which `Session` respects by only ever holding onto two.

## Remote-triggered Lisp execution

There's no RPC layer here — 9P is a file protocol. But since LugalOS's
`(load path)` primitive evaluates a `.lisp` file already sitting on disk,
writing a file into a directory something on the board watches (e.g. a
tiny `while`-loop in `init.lisp`/`usr_init.lisp` polling an "inbox" for new
files and `(load ...)`ing them) is enough to get remote-triggered
evaluation with zero new protocol work. A more "native" Plan-9-style
mechanism — a writable `ctl` file where a write evaluates and a read
returns the result — is a real possibility for later, but needs new
server-side support (a writable synthetic VFS node, the same shape `/proc/*`
already uses for read-only kernel state) and isn't part of this library.
