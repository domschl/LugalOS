# Phase 14 — 9P distribution, real networking & host tooling

**Status: 14a CONCLUDED, 2026-08-21. 14a-2 (`host/fuse-p9`) CONCLUDED,
2026-08-21. 14b CONCLUDED, 2026-08-22** (deferred at the time in favour of
14a-2, built in phase 16). 14c-14e not started.

## Background: five topics, sequenced

Phase 13 concluded the Lisp engine work. Before starting phase 14, five
loosely-related topics were raised together: LugalOS's 9P distribution
features are barely used by any actual application scenario; real
networking (RP2350W wireless vs. W5500 hardware TCP/IP) hasn't been
touched; new hardware platforms (K210 Maix Bit, ESP32-P4-Nano) are
available; networking raises a security/authentication question that
doesn't exist yet; and a host-side (macOS/Linux) library for remote access
to LugalOS over 9P was raised, possibly FUSE-shaped eventually.

These were untangled into a sequence, confirmed with the hardware actually
on hand (5× W5500 modules, 2× ESP32-P4-Nano, 2× K210 Maix Bit):

- **14a — `product-p9lib`** (this phase): a real, general-purpose 9P2000
  host-side file-access library and CLI, exercised over transports LugalOS
  *already* speaks (virtio-console/USB-CDC, UART/SLIP) — no new networking
  hardware or protocol work required. Chosen to go first specifically
  because it's decoupled from every open hardware question below.
- **14b — chess PGN save-games**: use 14a's write path to let a host fetch
  (and later, push) complete-game PGN records from the chess persona's SD
  card. Small, deferred until after 14a lands.
- **14c — security/authentication**: a lightweight pre-shared-secret/HMAC
  gate on `Tattach`, needed before any link is reachable over a real
  network rather than a physically-attached cable. Sequenced before 14d,
  not after, since retrofitting auth onto a live network story is worse
  than building it in.
- **14d — real networking**: 9P-over-IP, prototyped first against QEMU's
  virtio-net (protocol-layer testing without hardware), then W5500 on a
  **dedicated network-gateway RP2350 persona** — chosen (over sharing the
  chess persona's SPI bus) specifically to sidestep RP2350 having only two
  SPI controllers, both already claimed by the chess persona's TFT and SD
  card. The gateway persona has no display/keypad; it bridges to
  chess/clock boards over the *existing* UART/USB 9P links this project's
  T3 milestone already proved (`plan/phase5_distributed_design.md`), so no
  new SPI arbitration work is needed.
- **14e — new hardware platforms** (K210, ESP32-P4): orthogonal to the
  above, sequenced last only because nothing else depends on it.

Additional question from the same conversation: **can Lisp programs be
started via the remote 9P connection?** Answered in 14a below (write-to-
watched-directory, no protocol changes) rather than deferred.

---

## 14a — `product-p9lib`: a real host-side 9P file utility *(done, 2026-08-21)*

**Goal.** Promote the ad hoc, read-only `tests/p9lib.py` (test-only, used
solely by `tests/runner.py`) into an actual product: `host/p9lib/`, a
`uv`-packaged Python library + CLI (`lugal9p`) giving a host machine full
read/write/mkdir/remove access to any LugalOS filesystem reachable over a
serial port (RP2350's USB-CDC "net" port, a UART/SLIP link) or a unix
socket (QEMU virtio-console, for hardware-free development).

**What was built.**
- `host/p9lib/src/p9lib/client.py` — the wire protocol: `P9Client`
  (`version`/`attach`/`walk`/`open`/`create`/`read`/`write`/`stat`/
  `remove`/`clunk`, raw and SLIP framing) and `Session`, a path-based
  convenience layer that does fid bookkeeping for the caller using only 2
  fids, respecting the server's small fixed fid table (`P9_MAX_FIDS = 8`
  in `fs/include/fs/9p.h`).
- `host/p9lib/src/p9lib/transport.py` — `connect_unix`, `connect_serial`
  (local `import serial`, so the core library has zero dependencies unless
  this specific function is called), and the CDC-ACM first-frame-drop
  `warm_up_9p()` workaround ported from `tests/hw/rp2350.py`.
- `host/p9lib/src/p9lib/cli.py` — `lugal9p`: `ls`/`stat`/`cat`/`get`/
  `put`/`rm`/`mkdir`, against `--serial` or `--unix`.
- Packaged with `hatchling`, `pyproject.toml` + `uv.lock`, matching
  `tests/hw/`'s existing `uv` convention. `serial` is an optional extra so
  QEMU-only/library use pulls in nothing beyond the standard library.
- `tests/runner.py` and `tests/hw/rp2350.py` now both import `p9lib` from
  `host/p9lib/src` (via `sys.path.insert`) instead of the old
  `tests/p9lib.py`, which is deleted — protocol fixes only ever need to
  happen in one place now, as intended.

**Bug found and fixed along the way (not part of 14a's original scope):**
CRUD verification against live QEMU caught a real, pre-existing bug in
`fs/fat32.c`'s `filename_to_83()`. Its base-name loop exits either on
seeing `.` or after 8 characters — but if it exits via the 8-character
bound, it never actually consumes the `.`, so the following `if (*src ==
'.')` check (meant to detect "is there an extension to parse") sees
whatever character comes *after* the still-unconsumed 9th character
instead, and silently drops the extension entirely. Any file or directory
name with a 9+ character base collapsed to a bare, extension-less 8.3 name
— e.g. `host_test.txt` and the unrelated `host_test_dir` both truncated to
identical `HOST_TES` with no extension. Nothing in the existing test suite
had ever written a file with a long name or called `mkdir` at all (every
prior 9P test only read pre-existing short 8.3-named files), so this went
undetected. The collision made `vfs_mkdir()` (via `fat32_find_file()`
inside `fat32_mkdir()`'s own existence check) see a false "already exists"
for the *other* name, failing with "create: mkdir failed" as soon as a
long-named file and a colliding long-named directory both existed.

Fixed by rewriting `filename_to_83()` to locate the extension separator
first (scanning for the last `.`) and derive the base-name length from
that, independent of whether the base portion is truncated to 8
characters — so the base and extension are always parsed correctly
regardless of length. Rebuilt and reflashed into all three build
directories (`build/rv32`, `build/rv64`, `build/rp2350`) since `fs/
fat32.c` is shared source across every target.

**Remote-triggered Lisp execution** (the additional question raised
alongside 14a): no new protocol work. LugalOS's existing `(load path)`
primitive evaluates a `.lisp` file already on disk, so a tiny watched-
directory poll loop in `init.lisp`/`usr_init.lisp` (`(load ...)` anything
new dropped into an "inbox" directory) is enough to get remote-triggered
evaluation using nothing beyond 14a's own `write`. A more "native"
Plan-9-style mechanism — a writable `ctl` file where a write evaluates and
a read returns the result, the same shape `/proc/*` already uses read-only
— is a real possibility for later, but needs new server-side VFS support
and is out of scope here; documented in `host/p9lib/README.md` rather than
built.

**Verify.**
- Full QEMU regression suite: **245/245 tests passing**, both rv32 and
  rv64 targets (`python3 tests/runner.py`) — 243 pre-existing plus a new
  `test_9p_crud_via_p9lib` (A6) exercising `Session`'s full write/read/
  truncate/stat/mkdir/nested-write/remove cycle end to end, specifically
  the regression test that would have caught the `filename_to_83()` bug
  above (asserts the on-disk 8.3 name in a directory listing keeps its
  extension, then successfully creates a colliding-prefix directory).
- `lugal9p` CLI (`ls`, `mkdir`, `stat`, `rm`) manually verified end-to-end
  against a live QEMU instance over the virtio-console transport.
- Verified against real RP2350 hardware (chess persona), 2026-08-21: board
  reflashed with the current tree (build id `262.67433418+`, matching
  `local_build_id()`) so the `filename_to_83()` fix was actually under
  test, not the pre-fix v0.10.0 firmware it was originally running. Full
  `Session` CRUD cycle (write/read/truncate/stat/mkdir/nested-write/remove)
  run over `connect_serial("/dev/ttyACM1")` (`link_usb_cdc`), including the
  exact colliding-prefix `mkdir` case (`host_test.txt` + `host_test_dir`)
  the bug fix addresses — all passed, `/sd0` confirmed left clean
  afterward. `lugal9p ls/mkdir/stat/rm` also exercised directly as the CLI
  a user would actually run.

---

## 14a-2 — `host/fuse-p9`: mount the namespace, don't just script it *(done, 2026-08-21)*

**Goal.** After 14a landed, chess PGN save-games (14b) was explicitly
deferred in favor of a more general win: a Linux FUSE filesystem exposing
LugalOS's entire 9P namespace as a real host directory, so ordinary tools
(`cat`, `cp`, editors, `find`) work against a board unmodified instead of
needing `lugal9p` invocations for every access. Scoped deliberately small
per direction given at the time: a single shared `p9lib.Session` (no
per-mount fid pool), single-threaded dispatch, concurrency work explicitly
deferred; exposing the *entire* tree (not just `/sd0`) as the first step.

**What was built.** `host/fuse-p9/` (new `uv` package, depends on
`host/p9lib` via a local path source): `src/fuse_p9/operations.py`
(`P9FS(fuse.Operations)` wrapping one `p9lib.Session`) and `cli.py`
(`lugal9pfuse`, same `--serial`/`--unix`/`--framing`/`--aname` flags as
`lugal9p`, running `FUSE(..., nothreads=True)` so libfuse's own
single-threaded dispatch is what actually makes the shared, unlocked
Session safe — not a design assumption left unenforced). Supports
browsing, reading, writing (arbitrary offset, whole-file-buffered between
open/close), `mkdir`, `unlink`/`rmdir`. Deliberately refuses `rename`
(`ENOSYS` — 9P has no `Twstat` to call, and copy+delete would silently
misrepresent a real directory move) and no-ops `chmod`/`chown`/`utimens`
(the server has no permission or wall-clock model to persist them to).

**Bug found and fixed along the way (in `host/p9lib`, not FUSE-specific):**
mounting and running ordinary tools against it immediately wedged the
whole mount after the first write. Root cause: `Session.stat()` (and
`read()`/`write()`/`mkdir()`) walks `WORK_FID` to the target path and, on
failure, raised `P9Error` without checking whether the walk had *partially*
succeeded. Stat-ing a path whose last component doesn't exist yet — which
is exactly what FUSE's `getattr()` does before every `create()`, e.g. on
every `echo > newfile`— is a partial walk: the parent resolves, the leaf
doesn't, and per `fs/9p.c`'s `p9_handle_twalk()`, the server leaves
`WORK_FID` bound to the parent in that case rather than leaving it
untouched (that guarantee only holds for a single-element, walk-in-place
failure, which is all any earlier test had ever exercised). `Session` never
clunked it back, so the fid stayed permanently "in use" server-side, and
every subsequent call using it failed with `walk: newfid already in use` —
not a FUSE bug, but the first workload that ever exercised this path.
Fixed by adding `_walk_or_raise()`, a shared helper (used by
`_walk_to()`, `write()`'s and `mkdir()`'s parent-directory walks) that
clunks the fid before raising whenever `nwqid > 0` (partial resolution) but
correctly skips the clunk when `nwqid == 0`, since a first-component
failure is an outright `Rerror` — raised inside `P9Client.walk()` itself,
before any fid is allocated at all — and clunking an fid the server never
allocated would raise a second, more confusing error masking the original.

**Verify.**
- Full QEMU regression suite: **245/245 tests passing**, both rv32 and
  rv64 — `test_9p_crud_via_p9lib` (A6) extended with a check that stat-ing
  a not-yet-existing path raises cleanly *and* leaves the Session usable
  for every operation that follows (the exact scenario the bug above
  broke).
- Manually mounted against a dedicated QEMU instance (virtio-console) and
  exercised with real, unmodified host tools: `ls`/`cat` across the whole
  namespace (`/sd0`, `/proc`, `/ram0`, ...); `echo >`/`>>` (write, then
  append-via-reopen); `cp` a real host file in and `diff`-verified the
  round trip; `mkdir` plus a nested write/read; `rm`/`rmdir` cleanup,
  confirmed to leave `/sd0` exactly as found. Noted, not a bug: `/srv`
  *lists* in the root namespace but isn't currently walkable (nothing
  bound there this boot) — surfaces as a `?????` row in `ls -la`, an
  accurate reflection of the server's own answer, not a FUSE defect.
- Verified against real RP2350 hardware (chess persona), 2026-08-21, over
  `connect_serial("/dev/ttyACM1")`. This surfaced two more real, host-side
  bugs that QEMU's effectively-instant virtio-console link never could:
  - **Tag desync on a real serial link.** `_roundtrip()` raised on the
    first mismatched-tag reply it saw and never read again, but this
    client only ever has one request outstanding — so any wrong-tag reply
    can only be a *late* reply to something it already gave up on (e.g.
    `warm_up_9p()`'s retried Tversion attempts racing a slow first
    response), and leaving it unread desyncs every reply after it by
    exactly one frame, forever (raw framing has no other resync
    mechanism). Fixed by having `_roundtrip()` discard up to 4
    wrong-tagged frames and keep reading for the real one, instead of
    raising on the first mismatch.
  - **`connect_serial()`'s `timeout` silently discarded after warm-up.**
    It only reached the *initial* `Serial(...)` timeout — `warm_up_9p()`
    always reset the adapter back to its own hardcoded 5.0s default
    afterward, so passing a longer `timeout=` had no effect on any request
    made once the session actually settled. Fixed by threading `timeout`
    through as `warm_up_9p`'s `settle_timeout`.
  - Both fixed in `host/p9lib`; `tests/runner.py` (245/245) unaffected by
    either (QEMU's transport never exercises either failure mode).
  - After both fixes: full `lugal9pfuse` write/append/`cp`/`mkdir`/nested-
    write/`rm`/`rmdir` cycle verified clean against the real board, `/sd0`
    confirmed left exactly as found.

### Follow-up: a real firmware bug, root-caused and fixed *(done, 2026-08-21)*

The user reported `fuse-p9` as unstable in real use ("after a few
operations, system stalls... execution seems very slow") — `tree`/`ls -la`
against the board hung on specific entries (`/sd0/P1.ELF`, `/sd0/SYSTEM`,
`/dev/eeprom`) and, worse, seemed to degrade the whole mount afterward.
The `Tstat`-hangs-on-two-paths finding recorded above turned out not to be
a lock/contention issue at all once actually root-caused:

**Root cause: a missing USB zero-length packet.** RP2350's USB bulk
endpoints (EP2 console, EP4 `link_usb_cdc` net) have a 64-byte max packet
size. Whenever a 9P reply's *total* wire frame lands on an exact multiple
of 64 bytes, USB has no way to mark that bulk IN transfer complete other
than a trailing zero-length packet — the firmware wasn't sending one, so
the host's USB core held the transfer open forever, and since this server
only ever has one request in flight, every request after it hung too
("the whole system stalls" was the whole 9P server being starved by one
stuck transfer, not a per-file problem). Root-caused with a systematic
sweep on real hardware: stat-ing synthetic filenames of length 2-12 hung
*only* at length 6 (giving an exactly-64-byte `Rstat`) and worked
instantly at every other length; a parallel sweep of file-read sizes
confirmed the same boundary for `Rread` (hangs exactly at a 53-byte file,
whose reply frame is exactly 64 bytes). `P1.ELF`, `SYSTEM`, and `eeprom`
all happened to produce exactly-64-byte stat replies, hence the original,
narrower-looking symptom. Never reproducible on QEMU, which has no
packet-size concept at all.

**Fixed in `drivers/usb_cdc.c`**: `ep4_link_send_frame()` (the one
function that actually knows a queued frame's total length) now sets a
new `ep4_tx_zlp_pending` flag whenever `len % 64 == 0`; `ep4_tx_pump()`
and its U-mode twin `u_ep4_tx_pump()` check that flag once the ring
genuinely empties and send an explicit zero-length packet, clearing it —
and the blocking send's own drain-wait now waits for that flag too, so a
caller can't get its buffer back before the ZLP actually went out. EP2
(console) shares the same low-level pump but has no discrete
message-length concept to hang the fix off (it's a raw byte-stream, not
framed messages) — left alone for this fix; console output has no
systematic reason to land on a 64-byte boundary the way structured 9P
replies do, but the same class of bug is theoretically possible there too
if ever observed.

**Verified**: rebuilt all three targets, 245/245 QEMU tests unaffected
(`usb_cdc.c` isn't part of the QEMU builds). Reflashed the real board
(build id `265.794d252f+`, matching the tree) and reran the exact sweep
that found the bug — every length 2-12 now stats instantly, including the
original `P1.ELF`/`SYSTEM`/`eeprom`. `tree` over the whole namespace via
`fuse-p9` now resolves `/dev` (including `eeprom`) and all of `/flash0`
(previously unwalkable, now fully browsable down to nested `SYSTEM/BIN/
*.ELF`) correctly.

### Follow-up 2: `/sd0` still failed, permanently, across remounts *(done, 2026-08-21)*

The user reported the ZLP fix wasn't enough: `tree` still stalled ~1-2s on
`/proc` before listing it, then *always* failed opening `/sd0` -- and once
that happened, every FUSE operation failed instantly (`~0.003s`, not a
hang) until the mount was redone, even though the underlying `lugal9pfuse`
daemon process was confirmed still alive throughout.

**Root cause: a single, reused work fid can't survive an ambiguous
timeout.** `Session` used one fixed fid number for every walk. A 9P round
trip's *read* can genuinely time out on a real serial link without any
clean server error (`/proc/df`'s ~6.7s full FAT-table scan -- legitimate,
expensive work, confirmed in `fs/fat32.c`'s `fat32_statfs()`, not a bug --
was the actual source of "stalls 1-2s on /proc" and was slow enough to
occasionally trip this). When that happens *during a walk*, the client
genuinely cannot tell whether the server finished binding the fid it was
given right as the client's read gave up -- the exception is raised
inside `P9Client.walk()`'s own round trip, before `_walk_or_raise()` (the
fix from the previous follow-up) ever gets a return value to inspect, so
its clunk-on-partial-failure logic never runs. With only one fid number
ever in use, that single ambiguous case wedges it permanently server-side
-- every later call reusing that same number then sees `walk: newfid
already in use`, which is exactly `/sd0`'s deterministic failure (it's
simply next in `tree`'s alphabetical traversal after the slow `/proc`
scan) and exactly why *everything* failed afterward, forever, until a
fresh mount opened a fresh connection.

**Fixed in `host/p9lib`**: `Session` now hands out a fresh, monotonically
increasing work fid for every logical call (`_alloc_work_fid()`) instead
of reusing one fixed number. A fid left in an ambiguous state after a
timeout just becomes one permanently "used up" slot in the server's small
8-slot table (`P9_MAX_FIDS = 8`) rather than wedging the whole session --
sizable headroom before that could matter again. Also fixed the same
open()-outside-the-try/finally gap `read()` and `listdir()` still had
(mirroring the walk fix, in case `open()` itself is what times out).

**Also changed `host/fuse-p9`**: while investigating, switched
`lugal9pfuse` from `FUSE(..., nothreads=True)` to fusepy's normal
multi-threaded dispatch, with a `threading.Lock` in `P9FS` serializing
actual `Session`/wire access. `nothreads=True` meant one slow-but-legitimate
call (again, `/proc/df`) blocked libfuse's *entire* request queue for its
duration -- not the root cause of the `/sd0` failure itself, but a real,
independent stability improvement kept alongside the fid fix (see
`operations.py`'s updated docstring).

**Verified**: 245/245 QEMU tests unaffected. Reflashed nothing (both fixes
are host-side Python only); re-ran `tree` three times in a row against the
real board with no remount in between -- identical, complete output every
time (`/sd0` fully resolved, including nested `SYSTEM/BIN/PRIME.ELF`), each
run's ~6.9s consistent with `/proc/df`'s real scan cost rather than a
hang. Full write/append/`mkdir`/nested-write/`rm`/`rmdir` cycle re-verified
clean afterward, board left exactly as found.

### Follow-up 3: `/dev/uart` blocks forever on read -- a real device, not a file *(done, 2026-08-21)*

The user reported an even harder hang: browsing to `/dev/` in GNOME Files
(Nautilus) displayed its contents, then hung completely -- and
`lugal9pfuse` itself stopped responding even to Ctrl-C, recoverable only
by physically unplugging the board.

**Root cause**: `fs/vfs_server.c`'s `vfs_pread()` services `/dev/uart`
reads with a bare `uart_getc()`, which blocks on the real physical UART
line until a byte actually arrives -- correct, expected behavior for a
real serial device (mirroring how reading a real `/dev/ttyS0` with
nothing sending behaves), not a bug in the kernel. The problem is what
`fuse-p9` let happen to it: file managers automatically `open()`+`read()`
a little of every "regular-looking" file they list, to sniff its MIME
type for an icon/preview -- and `getattr()` reported every non-directory
entry as a plain `S_IFREG` regular file, giving GVFS no reason to treat
`/dev/uart` any differently. With nothing external sending data over the
real UART line, that automatic read blocked forever, and since `open()`
fetches a file's whole content before returning (see the class docstring)
while holding `P9FS`'s lock, every other operation on the mount queued up
behind it too -- Nautilus, `lugal9pfuse`, and anything else touching the
mount, all stuck on the same blocked native call, which is exactly why
Ctrl-C couldn't reach it either.

**First attempt, tried and reverted**: reporting `/dev/*` as `S_IFCHR`
(character special) instead of `S_IFREG` in `getattr()` is the standard
signal GIO/Nautilus uses to skip content-based MIME sniffing entirely --
in principle the right fix. In practice it broke *all* access, not just
the automatic kind: every FUSE mount a non-root user creates always
carries the kernel's own `nodev` option (there's no way to opt out of it
without `CAP_SYS_ADMIN`), and under `nodev` the kernel refuses to even
`open()` anything reported as a device node at all -- confirmed live,
`cat`-ing any `/dev/*` entry started returning "Permission denied"
instead of working, which is worse than the original problem for the
three paths (`null`, `zero`, `eeprom`) that were never actually dangerous
to read.

**Actual fix, in `host/fuse-p9`**: `getattr()` reports `/dev/*` as
ordinary `S_IFREG` files again (`ls`/`tree`/`stat` all still work exactly
as before), and `open()`/`truncate()` instead refuse `/dev/uart`
specifically -- a new `P9FS._NEVER_OPEN` set, checked before the shared
lock is even touched, raising a clean `EIO` immediately. This is
deliberately narrower than the reverted attempt: it accepts that a
deliberate `cat .../dev/uart` no longer behaves like a real serial port
(it fails instead of blocking), in exchange for making the catastrophic
failure mode -- an automatic, incidental read wedging the entire board's
9P server -- structurally impossible rather than merely discouraged.
`null`/`zero`/`eeprom` are untouched and still fully readable/writable,
since none of them can actually block (`vfs_pread()` answers `null`/
`zero` immediately with no I/O at all, and `eeprom`'s `at24c32_read()` is
a bounded real I2C transaction, not an open-ended wait for an external
event).

**Verified**: 245/245 QEMU tests unaffected; `_attrs()`'s mode logic and
the `_NEVER_OPEN` set unit tested directly. Reflashed nothing (host-side
Python only). Real hardware, board reattached: `ls -la /dev` shows
ordinary `-rw-r--r--` regular files again (no more `nodev`-induced
`Permission denied`); `cat /dev/uart` now fails in ~2ms with a clean I/O
error instead of hanging; `cat /dev/null` still works normally; `tree` run
twice in a row over the whole namespace succeeds identically both times;
a full write/read/`rm` cycle on `/sd0` re-verified clean afterward.

### Follow-up 4: editors couldn't actually save through the mount *(done, 2026-08-21)*

The user reported editing `/flash0/CAT.C` in Emacs: saving always warned
"File changed on disk", and regardless of the user's choice at that
prompt, the file was never actually updated.

**Two compounding host-side bugs, both in `host/fuse-p9`:**
- `_attrs()` reported `time.time()` (current wall-clock) as `st_mtime` on
  *every single* `stat()` call. Every path's mtime was therefore
  different every time anything checked it -- Emacs's save path compares
  the file's mtime against what it recorded when the buffer was loaded,
  saw a mismatch unconditionally, and always warned of an external
  change, whether or not one had happened.
- `rename()` unconditionally refused with `ENOSYS`. Emacs's (and nearly
  every other real editor's) default save strategy is to write the new
  content to a temp file, then rename it over the real target -- so even
  after confirming "save anyway" at the mtime prompt, the actual save
  silently failed at the rename step and the original file was never
  touched.

**Fixed**: `_attrs()` now reports a stable mtime for the life of the
mount for any path *this session* hasn't modified (`self._mount_time`,
captured once at `P9FS.__init__`), and a fresh timestamp only for a path
this session actually wrote, created, or renamed (`_touch()`/`_untouch()`,
called from `create()`, `_commit()`, `mkdir()`, `unlink()`, `rmdir()`, and
`rename()`). `rename()` is now emulated for *files* -- read the (already
fully written and closed) source, write it over the destination, remove
the source -- while directory renames are still refused (`ENOSYS`):
recursively copying a whole tree over 9P to fake one is a much bigger,
riskier undertaking for something that's a rare, deliberate action rather
than an automatic one every editor save triggers.

**Not a bug, and not fixable here**: `/flash0` is `fs/vfs_server.c`'s
"Embedded Flash ROMDisk" -- permanently read-only by design (confirmed:
writing there returns a clean I/O error, original content untouched).
Editing a file *there* specifically was never going to persist, with or
without this fix; `/sd0` is the writable volume to actually test editor
saves against.

**Verified**: 245/245 QEMU tests unaffected; the mtime-stability logic
unit tested directly. Real hardware: repeated `stat` on an untouched file
now returns an identical mtime every time; a live simulation of exactly
Emacs's save pattern (write a temp file on `/sd0`, `mv` it over the
target) now correctly updates the target's content; directory rename
still correctly refused; `/flash0` write attempts still cleanly fail with
original content intact, confirming that specific case is expected
behavior, not a regression.

### Follow-up 5: `/dev/eeprom` hangs too -- the uart guard alone wasn't enough *(done, 2026-08-21)*

Reported after the `/dev/uart`-open guard (follow-up 3) landed, with a
fresh daemon and a fresh Nautilus (ruling out a stale process running the
old code): GNOME Files still hung completely browsing to `/dev/`, still
unrecoverable short of unplugging the board -- and this time even Ctrl-C
on `lugal9pfuse` itself only produced a garbled, unraisable
`ctypes`-callback exception (a Python/ctypes quirk when a signal lands
mid-exception-handling inside a C callback -- itself just noise, not a
clue about the underlying cause), never actually stopping the process
short of physically unplugging the board.

The traceback did confirm the hang was inside an `open()` call -- and
since `/dev/uart` is refused before ever touching the Session (an
immediate, cheap set-membership check), it structurally cannot hang, so
this had to be a different path. `/dev/eeprom`'s own read code
(`drivers/at24c32.c`'s `i2c_read_at24()`) has bounded, small per-step
iteration-count timeouts, and reading it via the kernel's own console
`(help)`/interactive commands was assumed safe on that basis -- but a
direct read straight over `p9lib` (bypassing Nautilus, `fuse-p9`, and
this plan's own earlier guard entirely) confirmed the assumption wrong:
the read never returned, even past 30 seconds, on this board's actual
hardware configuration (no EEPROM chip physically wired -- an optional
peripheral per the README's I2C wiring section). Wherever the real block
happens (evidently not the bounded polling code that was checked, so a
lower task/IPC layer this file has no visibility into), the observed
behavior at this layer is identical to `/dev/uart`'s: an unbounded hang,
not a bounded, if slow, real operation like `/proc/df`'s FAT scan.

**Fixed**: `/dev/eeprom` added to the same `P9FS._NEVER_OPEN` set
`/dev/uart` already used -- refused at `open()`/`truncate()` with a clean
`EIO` before the shared lock is even touched, exactly the same treatment
and the same reasoning as follow-up 3.

**Verified**: 245/245 QEMU tests unaffected. Real hardware: `cat
/dev/eeprom` now fails in ~6ms with a clean I/O error instead of hanging;
`cat /dev/uart` still does too; `cat /dev/null` still works normally;
`ls -la /dev` unchanged (still ordinary regular files, `tree` run twice in
a row still succeeds identically both times). Nautilus itself not
re-verified directly (no desktop environment available to drive it from
here) -- everything checkable from the host-tooling side confirms the
same class of hang that hit `/dev/uart` no longer exists for
`/dev/eeprom` either, which was the last unguarded path under `/dev/`.

### Follow-up 6: the /dev/ hang confirmed fixed, but two more real bugs surfaced under Nautilus *(done, 2026-08-21)*

Reported: the `/dev/` hang is gone under Nautilus, but browsing file
*properties/attributes* eventually crashed `lugal9pfuse` outright, with
a `struct.error: ushort format requires 0 <= number <= 65535` repeating
on every subsequent `getattr`. Separately, `/flash0` was showing as
owner-read-write in Nautilus despite being permanently read-only.

**Bug 1 -- tag counter never wrapped.** `P9Client._next_tag` incremented
by one on every single 9P round trip, forever, with nothing capping it
at 16 bits -- but tag is a wire `u16` (`fs/9p.c` packs it with
`wcur_u16()`), so `struct.pack("<H", tag)` starts raising the moment the
counter exceeds 65535. Nautilus polling file attributes is exactly the
kind of sustained, long-running load (far more total requests over a
mount's lifetime than any manual test session) that actually reaches
that count, where a quick `tree` or a few edits never would. Fixed:
`P9Client._tag()` now cycles back to 1 once it reaches `0xFFFE`, safe
because this client only ever has one request outstanding at a time (see
the class's own docstring) -- there's never a second live tag a wrapped
value could collide with. Also skips `0xFFFF` (`P9_NOTAG`, `fs/include/
fs/9p.h`), which `fs/9p.c` reserves for its own "invalid frame" error
reply, so a wrapped real tag can never collide with that sentinel either.

**Bug 2 -- read-only volumes reported as writable.** `_attrs()` reported
a flat `0o644`/`0o755` for every file and directory regardless of which
volume it lives under. `fs/vfs_server.c`'s `vfs_pwrite()` only has real
write paths for `MOUNT_FAT32`, `MOUNT_DEV`, and `MOUNT_REMOTE9P` --
`MOUNT_PROC` falls through to its default `-1` branch, and `/flash0` is
that same branch's own named exception (its comment calls it out as the
read-only "Embedded Flash ROMDisk"). Every write attempt under `/flash0`
or `/proc` already correctly failed with an I/O error no matter what
`getattr()` said, but *advertising* write permissions on something that
can never actually be written is a real, if purely cosmetic, bug -- it's
exactly what a file manager reads to decide whether to offer editing at
all. Fixed: paths under `/flash0` or `/proc` now report `0o444`/`0o555`
(no write bits) instead of `0o644`/`0o755`; `/sd0` and `/dev/*` are
unaffected.

**Verified**: 245/245 QEMU tests unaffected. Real hardware: the tag
counter forced to just below its wraparound point and driven through it
directly -- every request past the boundary still succeeds, confirmed via
a live sequence of `stat()` calls straddling the wrap. `ls -la` on
`/flash0` and `/proc` now shows `dr-xr-xr-x`/`-r--r--r--`; `/sd0` and
`/dev/*` are still `drwxr-xr-x`/`-rw-r--r--`; `/flash0` writes still
cleanly fail (unchanged behavior, now with honest permission bits); a
full `tree` plus `/sd0` write/read/remove cycle still succeeds
afterward.

### Follow-up 7: `/dev` accepted any name at all; `/proc/df`'s timeout was too short *(done, 2026-08-21)*

Reported: `ls -la /proc` showed `df` as `?????????` (a failed `stat`), and
separately, a plain `ls -la srv` failed even though `srv` lists at the
root. Investigating the first surfaced a real, previously-undiscovered
kernel bug while ruling out other explanations for the `?????` (a
misleading first lead, kept below for the record).

**Real kernel bug found**: `fs/vfs_server.c`'s `vfs_open()` validates a
requested name against the fixed set for `MOUNT_PROC` (rejecting
anything `vfs_generate_proc_content()` doesn't recognize) but never did
the equivalent for `MOUNT_DEV` -- it accepted *any* non-empty name under
`/dev/` unconditionally, handing back a phantom zero-length, non-directory
handle instead of failing. Confirmed directly: `stat("/dev/totally-bogus-
name")` succeeded. This is what made a `.Trash-1000`/`.hidden`-style probe
(the kind GVFS/Nautilus routinely does at the root of anything it treats
as a volume) appear to find something at `/dev/.Trash-1000` that doesn't
really exist, rather than getting the clean "doesn't exist" answer FAT32-
backed `/sd0`/`/flash0` already gave for the same probe. **Fixed** in
`vfs_open()`'s `MOUNT_DEV` branch: now checks the requested name against
`g_dev_names` (`uart`/`null`/`zero`/`eeprom`) the same way the `MOUNT_PROC`
branch already did, returning -1 for anything else. A firmware change --
rebuilt all three targets, reflashed, reverified: bogus names under
`/dev/` now fail cleanly; `null`/`uart`/etc. are unaffected.

**The actual `?????` cause, unrelated to the bug above**: `/proc/df`'s
real cost (`fs/fat32.c`'s `fat32_statfs()`, a genuine full FAT-table
free-space scan across every mounted volume) measured at ~7s on one test
run and ~13s on another against a larger `/sd0` -- both comfortably past
`connect_serial()`'s previous 5-second default timeout, so `stat`/`read`
on it failed outright rather than just being slow (indistinguishable,
from the client's own perspective, from a real hang like `/dev/uart`'s).
**Fixed**: default timeout raised to 30s (from 5s, via an intermediate
20s that a second, larger-card measurement showed still wasn't
comfortable headroom), and `--timeout` exposed as a CLI flag on both
`lugal9p` and `lugal9pfuse` for a card that needs even more. Host-side
only, no firmware change.

**`srv`'s "cannot access" is not a bug, and not new**: confirmed as the
same pre-existing quirk phase 14a's own QEMU testing already found --
`/srv` *lists* at the root but isn't itself walkable, because nothing is
currently bound there this boot (an unbound Plan-9-style service mount
point). Genuinely reflects the server's own answer.

**Verified**: 245/245 QEMU tests unaffected by either fix. Real hardware,
after reflashing: bogus `/dev/*` names (including a literal
`/dev/.Trash-1000` probe) now fail cleanly instead of phantom-matching;
`/proc/df` now succeeds within the new default timeout (`ls -la /proc`
shows a real size for `df`, no more `?????????`); a full `tree` and
`/sd0` write/read/remove cycle still succeed afterward.

**Still open**: the user separately asked "is Nautilus hammering the
fs?", reporting that root-level directories (`/dev`, `/flash0`, `/sd0`,
`/srv`) take ~5s to *open* specifically in Nautilus (not in `ls`, and not
for subdirectories) -- `/proc`'s own delay there is already expected and
accounted for above. The `/dev` phantom-match bug fixed in this same
follow-up was the leading candidate, but doesn't actually explain a
5-second delay on its own (the phantom stat itself was fast, ~2ms, both
before and after the fix); querying `.Trash`/`.hidden`-style paths
directly under `/sd0` and `/flash0` was already fast (correctly failing)
even before this fix, so the delay isn't that lookup either. Not
root-caused yet -- would need a `lugal9pfuse` debug-log capture
(`logging.basicConfig(level=logging.DEBUG)`, matching earlier follow-ups'
debugging approach) taken *while* reproducing the delay in Nautilus to
see what it's actually doing differently for a root-level folder versus
a subdirectory, since nothing tested directly against the protocol so
far reproduces it.

### Follow-up 8: cache `/proc/df`'s content, since re-scanning it can't be made fast *(done, 2026-08-21)*

Following up on "is Nautilus hammering the fs?": whenever `/proc` was
visible anywhere in a Nautilus enumeration, every operation seemed to
pay `/proc/df`'s full ~7-13s FAT-table scan cost -- and critically, the
KERNEL regenerates that content not just for an actual read, but for a
bare `stat()` too (`vfs_stat()` calls the same `vfs_open()` that computes
it), so a plain `getattr()` -- which any directory enumeration does once
per entry, `ls -la`/`tree`/Nautilus alike -- pays the same cost as
actually reading the file.

**Trade-off, addressed directly rather than ignored**: caching this
trades guaranteed freshness for responsiveness. The risk is bounded and
specific, not open-ended: `/proc/df` reports free space on `/flash0` and
`/sd0`; `/flash0` is permanently read-only so its number never changes;
`/sd0`'s free space only changes via a write, and the only writes that
can happen are through *this same mount* (nothing else this bridge does
could plausibly change on-disk free space without this process knowing).
So the cache is invalidated immediately whenever this session completes
any write/create/mkdir/remove/rename, and only needs its 30-second TTL
to cover the disjoint case: the SD card being modified through some
entirely different connection at the same moment -- a risk this bridge
already doesn't protect against anywhere else (nothing here detects an
external modification to any other file either).

**Implemented** in `host/fuse-p9`: `_get_proc_df()` caches `/proc/df`'s
raw content for 30s, used by both `getattr()` (to answer `st_size`
without a real `stat()` round trip) and `open()` (to answer a read
without a real `read()`); `_touch()`/`_untouch()` (already called from
every mutating operation for the mtime-tracking follow-up 4 added)
now also invalidate the cache unconditionally on every call, regardless
of path -- simpler and just as correct as trying to match the exact set
of volumes df's own text happens to mention. `_attrs()` was refactored to
take `is_dir`/`length` directly instead of a full `p9lib.Stat`, so
`getattr()` can synthesize an entry from the cached length without a real
`Stat` object.

**Verified**: 245/245 QEMU tests unaffected; the cache/invalidate logic
unit tested directly. Real hardware: first `ls -la /proc` after mounting
still pays the real ~13.5s scan (unavoidable, and correct -- there's
nothing to cache yet); every subsequent `ls -la`/`tree` immediately after
is near-instant (0.02s, 0.23s) instead of repeating the scan; writing a
100KB file to `/sd0` and reading `/proc/df` again immediately shows the
`Used` figure correctly updated (not a stale cached value) and pays the
full scan cost again, confirming the invalidation path is real and not
just always-hit-the-cache.

**Placement, asked and settled deliberately, not by default**: the user
asked whether client-side (`host/fuse-p9`) was really the right layer,
versus caching server-side in the RP2350 firmware instead, which would
have a real advantage -- the kernel is the one place that knows about
*every* write from *any* client, so it could invalidate the cache
precisely, with no TTL needed at all, and every client would benefit,
not just this one mount. Weighed against that: this is a resource-
constrained embedded target that has been carefully budgeted throughout
(bump-pointer allocators, no free, `P9_MAX_FIDS = 8` deliberately small),
and the actual problem is specific to this one client's usage pattern --
nobody's complaining that typing `df` once at the console is slow, only
that `fuse-p9`'s automatic, repeated `getattr()` polling (driven by
Nautilus) pays that cost on every single poll. A server-side cache would
also change behavior for every client uniformly, including ones that
might specifically want a fresh scan. Decided: kept client-side as
implemented above, accepting the client-side cache's real ceiling (the
30s TTL exists specifically to cover the one thing it can't know -- a
write from some other connection -- which a server-side cache wouldn't
need at all).

---

## 14b — chess PGN save-games *(done, 2026-08-22, in phase 16)*

**Design settled with the user before building**, since three questions forked
the work materially: full SAN vs a cheaper notation, auto-save vs on-demand,
and one save slot vs many. All three took the recommended option.

### SAN is the substance, not the file I/O

PGN's movetext is Standard Algebraic Notation, and the engine had none --
`format_move()` emits long algebraic (`g1f3`) everywhere. SAN is
*position-dependent*: naming a move requires knowing what else could have
reached that square, so it cannot be derived from the Move word. That is
~150 lines (`user/chess/src/pgn.c`) and it is what makes a file another
program will open, which is the only reason to choose PGN over a private
format.

`parse_move_san()` deliberately does **not** parse SAN's grammar. It formats
every legal move and compares, so the two directions cannot disagree about
disambiguation or suffixes -- which is exactly where a hand-written parser and
formatter drift apart. It costs a move generation per call, which at load time
is nothing.

### The self-test earned its keep immediately

`pgn_selftest()` round-trips every legal move in six positions chosen for the
cases that break naive implementations, then saves and reloads two whole
games requiring an identical FEN and move count back. Run for the first time,
it found:

- **a real file/rank inversion** in the disambiguation: two knights on b1 and
  f1 bearing on d2 produced `N1d2` -- a *rank* disambiguator between pieces
  that share a rank, distinguishing nothing. Correct is `Nbd2`.
- **two of my own test positions wrong before the code was.** One expected
  `Rag1` where the white king blocks a1's path, so correct SAN is the
  undisambiguated `Rg1`. Its replacement put the black king where the a8 rook
  already checked it with White to move -- an illegal position the generator
  accepts, making every move come back with a `+`.

The round-trip is the strong half: it cannot be satisfied by a formatter that
under-disambiguates, because two moves formatting identically means one parses
back as the other. The literal expectations catch the opposite error, since a
formatter that disambiguates everything unconditionally round-trips perfectly
and is still wrong.

### Storage

`/sd0/chess/` (or `/ram0/chess/`), replacing the single FEN-plus-level
`chess.save` in the volume's `system/` area -- these are the user's data, and
there are several of them now.

- **Auto-save** to `current.pgn` after every completed move, from either input
  device. Silent in both directions: it does not announce success, and it does
  not report failure, because a full or absent card must not stop the board
  being a chess computer.
- **Auto-restore** on entering a session. *The user caught this gap*: auto-save
  without it is half a promise -- a board that saves every move and boots to an
  empty position has kept the game and hidden it. It matters most on the
  persona that boots straight into chess with no shell to type `load` at.
- **`new` archives** the outgoing game to `games/game-NNN.pgn` first (the
  user's own suggestion, and it closes a real hole auto-save would otherwise
  have opened: the next move would silently overwrite the previous game).
  Numbered rather than timestamped, because the RTC is optional on this board
  and a timestamp would collide or read as the software clock's epoch without
  one; the real date still reaches the `[Date]` tag, or `????.??.??` when
  there is no clock to ask.
- **Named saves on the terminal, single slot on the keypad.** `save <name>`,
  `load <name>` and `games` at a terminal; the keypad's SAVE/LOAD keep using
  the current game, because an eight-character seven-segment display is a poor
  file picker. Both front ends write through the same `console_write_pgn()`,
  so they cannot produce different files for the same game.

`[SetUp]`/`[FEN]` tags are emitted when a game did not start from the initial
array, so a game begun with `fen <position>` reloads as itself. That lets PGN
be the only format rather than keeping two.

### A bug the design change exposed

Adding the archive to `console_new_game()` broke session start, which called
the same function -- so merely *entering* chess would archive the game it was
about to resume. Split into `console_reset_game()` (plain) and
`console_new_game()` (archive then reset), with `console_resume_or_new()` for
session start.

### Follow-up from live use, same day

The user played a game on the board and reported the keypad path spamming the
terminal with `tm_wait_key: raw key=6 / 0 / 5 / 2` -- four lines per move.
Those traces were scaffolding from when the key protocol itself was being
worked out (H4 found the "hang" reports that way), and cost nothing while the
terminal was a debugging channel. It is now the session's other half, so they
are gone; a completed move announces itself instead, in SAN
(`Board plays: Nf3`).

That prompted the same question one level up -- should the *function* keys
report anything? Not the presses, no: which keys a human is pushing is the
board's business. But the state they change is not. A level set from the
keypad menu now prints the same line the console's `level` command does, and
the same for the auto-reply toggle, save and load. Read-only menu items
(score, side to move, halfmove clock, move count) stay silent: they are
queries, and a query leaves nothing stale.

The engine's own replies moved to SAN too, on both front ends, so one session
speaks one notation and it is the notation the PGN files use.

**And a real bug the question exposed:** `tm_new_game()` -- the keypad's own
new-game menu item -- did not archive the outgoing game, because it predated
14b and reset the board directly instead of going through
`console_new_game()`. It was the one remaining route that could still discard
a game auto-save had been carefully keeping. Now routed through the same
function, so both new-game paths archive.

### Verified

QEMU **253/253** (four new tests: the notation round-trip, and PGN
save/archive/load-by-name), hardware **24/24**, plus the self-test run
directly on the board. Live on hardware after the follow-up: resume
("Resumed game from /sd0/chess/current.pgn (4 half-moves)"), archive-on-new
("Previous game archived to /sd0/chess/games/game-003.pgn"), and SAN output
("Engine plays: e5").

**One environment bug found only on hardware:** the self-test wrote to a
hardcoded `/ram0`, which is *unmounted* on the RP2350 chess persona -- that
board has an SD card and the RAM disk costs heap. It passed on QEMU and failed
on the only hardware anyone runs it on. It now picks whichever volume is
writable.

## 14c-14e — superseded / not started

**14c (security/authentication) and 14d (real networking) are superseded by
`plan/phase18_networking_and_auth.md`** (2026-08-24), which takes them
together as one phase — auth first, then the wire — on the grounds that the
shape of the auth gate depends on what a connection is, and that phase 14's
own sequencing argument ("before 14d, not after") is an argument for one phase
rather than two.

Phase 18 also revisits one judgement made here. 14d proposed prototyping
9P-over-IP against QEMU's virtio-net before the W5500; phase 18 inverts that,
because the W5500 terminates TCP in silicon and prototyping against virtio-net
would mean writing a software IP stack purely to have something to test
against -- a layer the real hardware never runs. See phase 18 §0.

**14e (new hardware platforms: K210, ESP32-P4)** remains not started, and is
deliberately excluded from phase 18's scope: it is a port rather than a
feature, and orthogonal to everything above (as this phase's own background
section already said).

14b (chess PGN save-games) was explicitly deferred in favor of 14a-2
above. See "Background: five topics, sequenced" above for what 14c-14e
each cover.
