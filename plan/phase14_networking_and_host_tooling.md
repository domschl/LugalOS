# Phase 14 — 9P distribution, real networking & host tooling

**Status: 14a CONCLUDED, 2026-08-21. 14a-2 (`host/fuse-p9`) CONCLUDED,
2026-08-21.** 14b explicitly deferred by user request in favor of 14a-2;
14c-14e not started.

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
- **New, separate finding — not fixed, not in scope here:** `Tstat` on two
  specific real-hardware paths (`/sd0/P1.ELF`, `/sd0/SYSTEM`) never
  returns at all — confirmed via a bare `P9Client.stat()` call (no
  `Session`, no FUSE) with a 30-second timeout, while `Twalk`/`Topen` on
  the exact same paths return in ~4ms. Genuinely hangs, not just slow;
  reproduces every time; unrelated to anything changed this session (both
  fat32.c's `filename_to_83()` and the two p9lib fixes above were traced
  through by hand for these exact names — neither is implicated). Most
  likely a pre-existing server-side (`fs/9p.c`/`fs/fat32.c`) lock or
  contention issue specific to a file/directory a currently-running
  on-board process has open (`P1.ELF` reads like "process 1's own
  backing binary"). Surfaces in `fuse-p9` as those two names showing
  `?????` rows in `ls -la` (each one blocking for the full timeout before
  FUSE reports `ENOENT`) — worth a dedicated investigation later, not
  patched over here.

---

## 14b, 14c-14e — not started

14b (chess PGN save-games) was explicitly deferred in favor of 14a-2
above. See "Background: five topics, sequenced" above for what 14c-14e
each cover.
