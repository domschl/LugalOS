# Phase 5 — Distributed Nodes & Memory-Model Isolation: Design & Roadmap

> **Status**: Planning. No Phase 5 code written yet.
> **Date**: 2026-08-07
> **Baseline**: commit `4a64b78` (Phases 0–4 + cross-cutting quick wins complete, 75/75 tests
> passing on RV64 and RV32, all three targets building clean).
>
> **Predecessors**:
> - [`plan/completed/2026-08-07_review_and_remediation.md`](completed/2026-08-07_review_and_remediation.md)
>   — the review that produced Phases 0–4. Finding IDs (B11, A2, V4, V5, …) referenced here are
>   defined there.
> - [`plan/rp2350_distributed_plan.md`](rp2350_distributed_plan.md) — the original distributed
>   vision. Still the statement of intent; this document supersedes its *sequencing* and
>   transport-option analysis with findings measured against the current code.

---

## Table of contents

1. [Goal and the core structural insight](#1-goal-and-the-core-structural-insight)
2. [Hard ordering gate: security](#2-hard-ordering-gate-security)
3. [Where the code actually stands today](#3-where-the-code-actually-stands-today)
4. [Track A — Distribution](#4-track-a--distribution)
5. [Track B — Memory model & isolation](#5-track-b--memory-model--isolation)
6. [Test topologies](#6-test-topologies)
7. [Milestones](#7-milestones)
8. [Effort and risk calibration](#8-effort-and-risk-calibration)
9. [Decisions needed before starting](#9-decisions-needed-before-starting)

---

## 1. Goal and the core structural insight

The stated goal for this phase: **make it possible to run tests with genuinely distributed,
heterogeneous topologies** — a NOMMU RV32 node and an MMU RV64 node cooperating over a real wire,
extending later to physical RP2350 hardware and eventually K210 / VisionFive 2.

The original Phase 5 list (5.1 VFS → 5.2 9P → 5.3 transport → 5.4 ELF → 5.5 MMU) reads as one
sequence, but it is actually **two nearly independent tracks**:

| | Track A — Distribution | Track B — Memory model & isolation |
|---|---|---|
| **Delivers** | Nodes talking 9P over real wires | Per-task isolation; a real microkernel |
| **Needs** | VFS handles, 9P server, link layer, test harness | MMU/PMP, address spaces, tasks, IPC |
| **Satisfies** | **This phase's stated goal** | The "microkernel" name (V1–V4) |
| **Depends on the other?** | **No** | No |

**This is the most important planning conclusion in this document**: a single-address-space
LugalOS is a perfectly good 9P node. Distribution needs no scheduler, no MMU, no user mode, and no
real IPC. The 9P server can be a poll-driven function called from the link layer's RX pump.

So Track A can be built and tested to completion on today's kernel, and the distributed-testing
goal can be met without touching the memory model at all. Coupling them — as the original
one-through-five numbering implies — would put the single largest and riskiest chunk of work
(Track B) on the critical path to the stated goal, for no technical reason.

**Recommendation: build Track A first, in full. Start Track B afterwards, or in parallel if there
is appetite for it, but never as a blocker for distributed testing.**

### On "heterogeneous"

Worth being precise, because it affects almost nothing in the design: 9P2000 is a little-endian,
byte-oriented wire protocol with explicitly-sized fields. A NOMMU RV32 node and an MMU RV64 node
already agree on the wire format with no negotiation beyond `Tversion`'s `msize`. Heterogeneity
across memory models, word widths, and physical vs. emulated hardware is therefore a *test-harness*
concern (spawning and wiring different node types) far more than a protocol one. The one real
protocol-level care point is that `p9_msg_t` currently uses a 32-bit `offset` where 9P specifies
64-bit — see [A2](#a2--9p-server-hardening-and-completion).

---

## 2. Hard ordering gate: security

**B11 (unbounded 9P deserialization) is still open, and it must be closed before any real transport
is enabled.**

Verified against the current tree — `fs/9p.c`, `p9_deserialize()`:

```c
const uint8_t *p = buf;
uint32_t size = read_u32(&p);
if (size > len) return -1;      // the ONLY length check in the function
msg->type = read_u8(&p);
msg->tag  = read_u16(&p);
switch (msg->type) {
    case P9_TWRITE:
        msg->fid    = read_u32(&p);   // no end-pointer check
        msg->offset = read_u32(&p);   // walks past buf+len on a short frame
        (void)        read_u32(&p);
        msg->count  = read_u32(&p);
        msg->data   = p;              // count is never validated against remaining length
```

`p9_serialize()` has the mirror-image defect on the write side: it validates `buf_size < 7` once,
then writes payloads — including `memcpy(p, msg->data, msg->count)` — with no further bound.

The Phase 1 review deferred this explicitly, and the reasoning is worth quoting because it is
exactly the condition that is about to change:

> Not reachable from today's local-only paths, but this is exactly the code the distributed roadmap
> intends to feed from a wire.

The moment [A3](#a3--link-layer-and-real-transport) lands, a malformed or hostile 9P frame arriving
on a UART becomes remotely-triggerable out-of-bounds read/write in the kernel. On a system with no
memory protection (which is every current target — see [Track B](#5-track-b--memory-model--isolation)),
that is unbounded kernel memory corruption from the wire.

**Gate: [A2](#a2--9p-server-hardening-and-completion) must be complete and fuzz-tested before
[A3](#a3--link-layer-and-real-transport) enables a real link.** This is an ordering constraint on
safety grounds, not a preference about tidiness.

---

## 3. Where the code actually stands today

Measured against the current tree, not assumed.

### VFS (`fs/vfs_server.c`, `fs/include/fs/vfs.h`)
- Whole-file API only: `vfs_read(path, buf, max_len)`. **No offsets, no handles, no seek.**
- `/proc` files **`printk()` directly and return an empty buffer** (`sbuf[0] = '\0'; return len;`).
  A remote 9P client can never read them. This is the structural blocker flagged as V5.
- `parse_prefix()` is a hardcoded if/else chain over 6 fixed prefixes with a fall-through default —
  no mount table, so no way to attach a remote namespace. (Also still carries the namespace
  fall-through defect A2 from the review: `/sd0/x` silently falls back to flash then ram.)
- ~20 call sites depend on the current whole-file signature.

### 9P (`fs/9p.c`, `fs/include/fs/9p.h`)
- Header already declares the full 9P2000 message enum including `Tstat`/`Tcreate`/`Tremove`.
  The implementation handles a subset and **implements none of those three**.
- **No fid table.** `Twalk` ignores the path and returns a fixed qid; `Topen` ignores the path.
- **Not connected to the VFS at all** — includes `fs/vfs.h` but never calls it. Reads and writes
  hit one global 2 KB buffer (`g_p9_storage_buf`).
- `p9_msg_t.offset` is `uint32_t`; 9P specifies 64-bit offsets.
- `p9_msg_t` has a single `const char *path`; `Twalk` carries up to 16 name components.
- B11 as above.

### Transport (`drivers/uart_net.c`, `drivers/loopback_net.c`)
- `uart_net_send_9p()` SLIP-encodes into `slip_tx`, **discards it**, and calls `p9_server_process()`
  in-process. It never touches a UART. Functionally identical to the loopback path.
- `slip_encode()` / `slip_decode()` themselves are real and bounds-checked — genuinely reusable.

### QEMU environment (measured, not assumed)
Dumped the `virt` machine device tree (`-M virt,dumpdtb=…`):
- **Exactly one UART**: `serial@10000000` (NS16550), aliased `serial0`, also `stdout-path`.
  A second `-serial` argument does **not** create a second guest UART — it lands on the QEMU
  monitor. Confirmed by DTB inspection.
- **Eight virtio-mmio slots**: `0x10001000`–`0x10008000`. `drivers/virtio_blk.c` probes this range
  and claims the first responder, so free slots remain.
- `virtio-serial-device` / `virtconsole` are available as QEMU devices.

### UART RX (`drivers/uart_16550.c`)
```c
char uart_getc(void) {
    while (!uart_has_char()) { usb_cdc_task(); }
    return (char)uart_base[UART_RBR];
}
```
Polled, blocking, **no RX buffering and no demultiplexing**. Every byte goes straight to whoever
called `uart_getc()` — normally the console line editor. A 9P frame arriving while the shell is
blocked here would be consumed as console keystrokes. This is the crux of the transport work.

### Memory model / tasks
- `arch/riscv/common/entry.S` **already** does the M→S mode transition under `CONFIG_MODE_S`,
  programs `pmpaddr0`/`pmpcfg0` wide open, and delegates all traps to S-mode. Good foundation.
- The trap vector does **not** switch stacks — it reuses the interrupted `sp`. Fine for
  kernel-only traps; a blocker for U-mode (needs `sscratch`).
- `rv64_mmu/vmm.c`: `vmm_map_page()` is `return 0;`. `satp` is never written.
- `task_t` is `{pid, state, sp, name}` — no register context, no address space, no kernel stack.
  `task_create()` discards its entry-point argument; nothing ever calls it.
- `trap.c` passes raw user pointers (`frame->a1`, `frame->a2`) straight into `printk`, `vfs_read`,
  `vfs_write`.

---

## 4. Track A — Distribution

### A1 — VFS handle API

Replace whole-file access with handles and offsets. This is the foundation everything else in
Track A stands on.

```c
typedef struct {
    uint8_t  type;        /* backend id */
    bool     is_dir;
    uint64_t size;
    uint32_t version;     /* -> 9P qid.vers */
    uint64_t ino;         /* backend-unique -> 9P qid.path */
} vfs_stat_t;

int vfs_open(const char *path, int flags);
int vfs_pread (int h, void *buf, uint32_t count, uint64_t offset);
int vfs_pwrite(int h, const void *buf, uint32_t count, uint64_t offset);
int vfs_readdir(int h, uint32_t index, char *name_out, uint32_t name_max, vfs_stat_t *st);
int vfs_stat(const char *path, vfs_stat_t *st);
int vfs_close(int h);
```

Sub-work:

- **`fat32_read_at()` / `fat32_write_at()`** — offset-based cluster-chain traversal.
  `fat32_read_file()` always starts at the first cluster. First cut: walk from the start on each
  call (O(offset)); later cache a `(last_offset → last_cluster)` hint in the handle, which turns
  sequential streaming — the dominant 9P access pattern — back into O(1) per call.
- **`/proc` as real byte streams** — generate content into a per-handle buffer at open time and
  serve reads from it. This is the standard synthetic-filesystem approach and is what finally makes
  `/proc` readable by a remote client (closes V5).
  *Care point*: the shell's `cat /proc/ps` output must stay byte-identical, because existing tests
  assert on it. The shell should read the buffer and print it, rather than the VFS printing as a
  side effect.
- **`/dev` nodes** — `uart` (stream; offset meaningless), `null`, `zero` become proper handlers.
- **Compatibility wrappers** — keep `vfs_read()`/`vfs_write()` as thin shims over
  open/pread/close. **This matters for reviewability**: without it, A1 becomes a single ~500-line
  diff touching every caller in the tree. With it, the API lands in one reviewable commit and call
  sites migrate incrementally.

#### A1 completion notes (2026-08-07)

Implemented and merged. All three targets (RV64, RV32, RP2350) build clean; full
`tests/runner.py` suite passes (75/75, RV64 + RV32).

- **API landed largely as designed**, with one simplification: `vfs_stat_t` ships as
  `{ uint32_t size; uint8_t is_dir; }` only — `type`, `version`, and `ino` (the 9P
  `qid.vers`/`qid.path` fields) were dropped for now, since nothing consumes them yet and
  they're cheap to add back when A2 actually wires a 9P fid table to real handles. Flags are
  `VFS_O_READ`/`VFS_O_WRITE`/`VFS_O_CREATE`/`VFS_O_TRUNC`; `VFS_MAX_HANDLES` is 8.
- **`/srv/` is explicitly *not* handle-addressable** — `vfs_open()` returns -1 for it. It's
  message-oriented IPC (a single request/response RPC per call, not a byte stream), so
  `vfs_read()`/`vfs_write()` keep a direct-dispatch branch for it ahead of the handle path, and
  `vfs_open()` rejects prefix-type 5 outright. Confirmed this doesn't block anything upcoming:
  A2's 9P server talks to the *VFS's* handle API for the files/dirs it serves, not to `/srv/`
  itself.
- **`fat32_read_file()`/`fat32_append_file()`** are now thin wrappers over
  `fat32_read_at()`/`fat32_write_at()`, eliminating duplicated cluster-walking logic rather than
  leaving three copies of it.
- **`/proc` generation uses a new `ksnprintf()`** (buffer-backed `printk()`-engine reuse, in
  `kernel/printk.c`) instead of `printk()`'s UART path, exactly as planned — content is
  generated once into `proc_buf` at `vfs_open()` time and served from there by `vfs_pread()`.
- **Root `"/"` and `/dev/` also got `vfs_readdir()` support**, beyond what the original
  sub-work list called for (which only mentioned `/proc`) — cheap to add since it's just static
  name tables, and it means the whole non-`/srv/` namespace is walkable through one API, which
  A2/A4 will want anyway.
- **Compat-wrapper return-value contract preserved deliberately, not by accident**: several
  existing callers (`user/lisp/lisp.c`'s `write-file`, `user/ed/ed.c`'s save) check
  `vfs_write(...) == 0` for success, matching old `fat32_write_file()`'s 0-on-success/-1-on-
  failure convention — *not* a byte count. `vfs_pwrite()` itself reports an honest byte count
  (the new, honest contract), so `vfs_write()` translates `n == len` to `0`. Missing this would
  have silently broken every `(write ...)` call in the Lisp REPL.
  `vfs_read()`/`vfs_append()` keep their pre-existing byte-count return convention unchanged.
- **Bugs found and fixed along the way (not in the original A1 scope, but exposed by it):**
  - `vfs_ls()`'s `/proc/` branch always printed a hardcoded generic listing regardless of the
    path given, so e.g. `(ps)` — which called `vfs_ls("/proc/ps")` — showed a directory listing
    instead of the process table. Root cause: `/proc` reads were a `printk()` side effect with no
    real content to list. Fixed by rewriting `prim_ps`/`prim_meminfo`/`prim_version`/`prim_df`/
    `prim_top` in `user/lisp/lisp.c` to actually `vfs_read()` and print the real content (new
    `print_proc_file()` helper), and by making `vfs_ls()`'s `/proc/` branch walk the real
    directory via `vfs_open()`/`vfs_readdir()`.
  - `kernel/shell.c`'s `cmd_uname()` read `/proc/version` into a buffer and then never printed
    it (dead read relying on the old side effect) — fixed to print what it read.
  - `vfs_cp()` copied through a single fixed 4096-byte static buffer with no chunking, silently
    truncating any source file ≥ 4096 bytes with no error reported. Rewritten to copy in 512-byte
    chunks via the new handle API with no size ceiling.
  - Fixes this A1 work depended on (`fat32.c`): FAT32 cross-volume read fallback for `/flash0`
    and `/sd0` (finding A2 in the review) is gone as a side effect of `vfs_read()` now routing
    through `vfs_open()`, which resolves a single unambiguous volume per prefix.
- **Test suite**: `tests/runner.py`'s "Lisp Microkernel VFS Primitives" test used to pass only
  because of the `vfs_ls("/proc/ps")` bug above (its generic listing happened to contain the
  literal word "synthetic", which the test asserted on). Updated the assertion to `kernel_idle`
  — a string that only appears in real `/proc/ps` content — so the test now actually exercises
  what its name claims.
- **Deferred, not forgotten**: `fat32_read_at()` still walks the cluster chain from cluster 0 on
  every call (O(offset) per read), same as the original `fat32_read_file()`. A per-handle
  `(last_offset → last_cluster)` cache — turning sequential 9P streaming into O(1) per call — is
  still worth doing once A2 makes that a hot path, exactly as the original sub-work list noted.

### A2 — 9P server hardening and completion

**Gated: must complete before A3.** See [§2](#2-hard-ordering-gate-security).

- **Close B11, both directions.** Add an explicit end pointer to the deserializer; bounds-check
  every field read and validate `count` against remaining frame length. Bound every write in
  `p9_serialize()` against `buf_size`. Clamp to the negotiated `msize`.
- **Widen `p9_msg_t.offset` to `uint64_t`.**
- **`Twalk` multi-component support** — up to 16 `nwname` elements, replacing the single `path`.
- **Real fid table** — `fid → {vfs handle, qid, path, open mode}`, with a bounded table and clean
  `Rerror` on exhaustion.
- **Implement the declared-but-missing messages** — `Tstat`/`Rstat` (needed for remote `ls`),
  `Tcreate`/`Rcreate`, `Tremove`/`Rremove`. `Tflush` can reply success as a no-op until there is
  concurrency to flush.
- **Wire the server to the VFS** from A1, replacing the single global 2 KB buffer.
- **Fuzz/conformance testing** — a Python peer (see [T1](#6-test-topologies)) that sends truncated,
  oversized, wrong-`size`, and hostile-`count` frames and asserts the node neither faults (UBSan
  panic since Phase 0 makes this detectable) nor answers incorrectly.

#### A2 completion notes (2026-08-07)

Implemented and merged. All three targets build clean; full `tests/runner.py` suite passes
(77/77, RV64 + RV32 — two new tests added by this work).

- **B11 closed.** `fs/9p.c` was rewritten around two small bounds-checked wire cursors
  (`wcur_t`/`rcur_t`, each carrying an explicit end pointer). Every write goes through
  `wcur_*()`, every read through `rcur_*()`; both set an `overflow` flag and become a no-op the
  instant a field would cross the end pointer, so a caller checks `overflow` once at the end
  instead of after every field. `rcur_data()` is the direct fix for the exact defect the review
  quoted (`msg->data = p` with `count` never validated) — it now bounds-checks `count` against
  what's actually left in the frame before handing back a pointer into it.
  `p9_deserialize()` also clamps its own working window to the frame's *declared* `size` (not
  just the caller's buffer length), so trailing bytes left over in a reused fixed-size receive
  buffer from a previous, longer message can never leak into the next one's parse.
- **`p9_msg_t.offset` is `uint64_t`** end to end (wire format already carried both 32-bit halves;
  only the in-memory struct and a `(void)read_u32(&p)` that discarded the high half needed
  fixing).
- **Real fid table** (`P9_MAX_FIDS = 8`, linear-searched by the client's `uint32_t` fid value —
  small and flat is fine for a server with one peer at a time). Each entry holds a resolved
  absolute VFS path, directory/open state, and (for open files) the underlying A1 `vfs_open()`
  handle. `Tversion` resets the whole table (connection re-init), `Tclunk`/failed `Tattach`/
  `Twalk` allocation-exhaustion all return a clean `Rerror` rather than corrupting state.
- **`Twalk` supports up to 16 components in one message**, including `..` (parent, clamped at
  `/`). Partial-walk semantics match the spec: `Rerror` only if the *first* component fails to
  resolve; a later failure just truncates `nwqid` to how far it got, per real 9P client
  expectations.
- **Every declared-but-missing message type is implemented**, not stubbed: `Tstat`/`Rstat`,
  `Tcreate`/`Rcreate` (including `DMDIR` for remote `mkdir`), `Tremove`/`Rremove` (removes then
  clunks the fid regardless of outcome, per spec), `Tflush` (no-op success; there's no
  concurrency yet to flush). `ORCLOSE` (remove-on-clunk) is also wired up since it was nearly
  free once `Tremove` existed.
- **Directory reads, not just `Tstat`** — the sub-work list named `Tstat`/`Rstat` as "needed for
  remote `ls`", but real 9P clients enumerate a directory's contents via `Tread` on an open
  directory fid (a stream of packed `stat` structures), not by statting names they'd have no way
  to already know. Implemented this too (`p9_read_dir_stream()`, built on A1's `vfs_readdir()`)
  — without it, "remote `ls`" would not actually have worked. Simplification: a directory
  `Tread`'s `offset` is only used to detect "start over" (`offset == 0`); otherwise the fid's own
  server-side cursor continues from wherever it left off, rather than treating `offset` as a
  literal byte-random-access position. This matches how directory-read offsets are conventionally
  treated in Plan 9 (an opaque, monotonic cursor, not a real byte offset) and is sufficient for
  every client this server talks to today; a client that seeks backward mid-listing isn't
  supported.
- **Server wired to the VFS handle API (A1)**, replacing the single global 2 KB echo buffer
  entirely: `Tattach` resolves `aname` to an absolute VFS path (defaulting to `/`) via
  `vfs_stat()`; `Topen` translates 9P mode bits to `VFS_O_*` flags and calls `vfs_open()`;
  `Tread`/`Twrite` call `vfs_pread()`/`vfs_pwrite()` at the real requested offset; `Tclunk` calls
  `vfs_close()`.
- **Fuzz/conformance testing deferred to A3/T1, not skipped** — the plan itself frames this as
  "a Python peer (see T1)", which is A3's milestone, not A2's. What A2 *does* provide toward it:
  every parse path is bounds-checked (the actual safety requirement the gate in §2 cares about),
  and manual testing confirmed a `p9-cat` of a nonexistent path returns a clean `Rerror` (`#f` to
  the Lisp caller) with no fault, under UBSan. Real hostile-input fuzzing against an independent
  implementation is still T1's job, once A3 exists to give it a wire to fuzz over.
- **`drivers/loopback_net.c` / `drivers/uart_net.c` rewired.** The old `loopback_9p_rpc()`/
  `uart_net_rpc()` did a bare `Tattach` + `Twrite`/`Tread` directly against the attach fid — that
  only ever worked because the old server treated every fid as the same global buffer regardless
  of what it was "supposed" to represent. With a real fid table, writing to an unopened directory
  fid now correctly fails. Both helpers were rewritten to do a real session: attach, walk to (or
  `Tcreate`) a scratch file under `/ram0`, open, write/read, clunk. Verified via QEMU that a
  `(p9-loopback "...")` write is now visible as a real FAT32 directory entry (`ls "/ram0/"` shows
  it, byte-exact) — not just an echo.
- **New `p9-cat` Lisp primitive** (`loopback_9p_cat()`), added specifically to prove the fid table
  resolves *arbitrary* pre-existing namespace paths, not just the fixed loopback scratch file: it
  drives `Tattach("/")` + a genuine multi-component `Twalk` + `Topen` + `Tread` + `Tclunk` against
  whatever path it's given. `tests/runner.py` now uses it to read `/sd0/system/init.lisp` — a file
  9P itself never wrote — over the wire and asserts on its real content. Also confirmed manually
  that `(p9-cat "/proc/version")` works, i.e. A1's `/proc` byte streams are now reachable over 9P
  too, and that a nonexistent path returns a clean `#f` rather than faulting.
- **Deferred, not forgotten:** qid uniqueness is FNV-1a of the resolved absolute path string, not
  a real inode/generation number — fine for a single-server, no-hardlinks filesystem, but a
  collision is theoretically possible (cosmetic, not memory-unsafe). Directory `Tread`'s
  offset-as-cursor simplification above. No `Twstat` (rename/chmod/truncate-via-stat) — not in the
  original sub-work list and nothing in this codebase needs it yet.

### A3 — Link layer and real transport

Introduce a transport-agnostic link interface so the 9P server never knows what wire it is on:

```c
typedef struct p9_link {
    const char *name;
    int (*poll)(struct p9_link *);                                  /* pump RX */
    int (*send_frame)(struct p9_link *, const uint8_t *, uint32_t);
    int (*recv_frame)(struct p9_link *, uint8_t *, uint32_t);
} p9_link_t;
```

Backends:

| Backend | Works on | Notes |
|---|---|---|
| `link_uart_slip` | QEMU UART, RP2350 UART, CP2102 | **Primary.** Reuses the existing, already-correct `slip_encode`/`slip_decode`. |
| `link_usb_cdc` | RP2350 `/dev/ttyACM1` | Endpoint already enumerates; data path unwired. |
| `link_virtio_console` | QEMU only | Optional. Clean dedicated channel for CI; needs a new driver. |

**The crux of A3** is not the framing — SLIP already works. It is that `uart_getc()` hands every
byte directly to the console. A shared wire needs an RX pump that demultiplexes: bytes inside SLIP
`END` (0xC0) delimiters go to the frame assembler, bytes outside go to a console queue. That means
introducing a small RX ring and a demux state machine underneath the line editor — **touching the
most load-bearing code path in the system** (every keystroke on every target).

De-risking sequence:

- **A3a — headless 9P mode.** A runtime/build flag dedicating the UART entirely to 9P, console
  disabled. No demux at all. This is the original plan document's "Option C", and it is the
  fastest path to a *real wire carrying real frames*.
- **A3b — demuxed shared wire.** Console and 9P interleaved on one UART. Needed for interactive
  debugging of a live node and for the single-cable RP2350 story.

#### A3 completion notes (2026-08-07)

Implemented and merged: the `p9_link_t` interface, `link_virtio_console` (explicitly requested
ahead of schedule, over the plan's original "probably not worth it" framing — see below), and
`link_uart_slip`/A3a headless mode. A3b (shared-wire demux) is **not** implemented — deliberately,
per this section's own de-risking sequence. All three targets build clean; full `tests/runner.py`
suite passes (81/81, RV64 + RV32 — four new tests: two per architecture).

- **`p9_link_t`** landed almost exactly as specified (`fs/include/fs/p9_link.h`,
  `fs/p9_link.c`): `name`/`poll`/`send_frame`/`recv_frame`, plus a small
  `p9_link_service()` (recv → `p9_server_process()` → send) shared by every backend, and a
  one-slot "background link" registry (`p9_link_register_background()` /
  `p9_link_background_poll()`).
- **`link_virtio_console` reprioritized ahead of A3a/A3b**, at explicit user request ("useful for
  testing down the road"), reversing the plan's own §8 framing ("probably not worth it unless
  demux proves genuinely painful"). In hindsight the reprioritization was the right call for a
  reason the original framing didn't anticipate: virtio-console is a *wire physically separate
  from the UART console*, so it needed **no RX demultiplexing at all** — the entire reason A3b
  is high-risk (touching every keystroke on every target) simply doesn't apply to it. It was the
  cheapest of the three backends to make available live, not the most expensive.
  - `drivers/virtio_console.c` is a new, self-contained MMIO driver (device id 3, non-multiport:
    RX = queue 0, TX = queue 1), modeled on `drivers/virtio_blk.c`'s style since this codebase has
    no shared `virtio.c` infrastructure to build on. RX is a single persistent 2 KB buffer,
    continuously drained (non-blocking) into an 8 KB software byte ring and re-posted; TX is
    synchronous/polling, exactly like `virtio_blk_transfer()`. No feature negotiation, matching
    `virtio_blk`'s existing precedent of accepting host defaults.
  - **Framing choice**: plain length-prefixed (9P's own 4-byte size header is sufficient framing
    over a reliable, ordered virtqueue byte stream) — deliberately *not* SLIP. SLIP's
    escaping exists to let a 9P frame share a wire with non-9P traffic and to recover
    synchronization on a possibly-lossy link; virtio-console is neither shared nor lossy, so SLIP
    would only add overhead.
  - **Genuinely live, not a special build**: rather than requiring a dedicated headless build or
    boot flag, `virtio_console_init()` (called from `kernel/main.c`, QEMU targets only) registers
    itself as the background link, and `drivers/uart_16550.c`'s `uart_getc()` busy-wait loop
    (`while (!uart_has_char())`) now also calls `p9_link_background_poll()` — the same spot
    `usb_cdc_task()` was already pumped from. This kernel has no real task scheduler (confirmed
    before writing this: `kernel/sched.c` is a bookkeeping shim, not a scheduler — see A1/A2's own
    notes on the single-call-stack model), so this busy-wait hook is the only place such a
    background pump *could* run without new concurrency machinery. Net effect: a host process can
    attach, walk, and read real files over virtio-console **while the interactive shell is sitting
    at its prompt**, with zero risk to the console.
  - Verified end-to-end with an external process: `tests/p9lib.py` (a new, independent Python 9P
    client — pack/unpack only, no LugalOS C code) connects to the chardev's unix socket and reads
    `/sd0/system/init.lisp` via `Tversion`/`Tattach("/")`/`Twalk`(3 components)/`Topen`/`Tread`/
    `Tclunk`, asserting on the real byte-exact content.
- **`link_uart_slip` (A3a)** reuses `drivers/uart_net.c`'s existing, already-correct
  `slip_encode()`/`slip_decode()` unchanged: an incremental accumulator collects raw bytes as they
  arrive and hands the whole thing to `slip_decode()` in one call the instant a `SLIP_END` closes a
  frame, rather than re-implementing incremental unescaping. Exposed as a `p9_link_t` via
  `uart_slip_get_link()`. `kernel/shell.c` gained a `p9serve` command that blocks forever servicing
  this link — genuinely headless (matches A3a's "console disabled" framing exactly: once invoked,
  only a reset gets the console back), and simpler than plumbing a build-time `CONFIG_*` flag
  through a new CMake target for the same effect.
- **Test-harness discovery, not a driver bug**: the first `p9serve` smoke test, run over QEMU's
  usual `-nographic` stdio console, failed every single time with a generic "Invalid 9P frame"
  error — bytes were vanishing from every request. Root cause, confirmed by instrumenting
  `p9_server_process()` temporarily: QEMU's stdio console/monitor multiplexer treats byte `0x01`
  (Ctrl-A) as its own escape character and swallows it (and whatever byte follows) before it ever
  reaches the guest UART — completely unrelated to SLIP, and not something any kernel-side fix can
  address, since the byte never arrives. Any binary 9P frame will eventually contain `0x01` (e.g.
  as a tag's low byte, as it did here). This is a real constraint on QEMU-based testing of raw UART
  traffic that happens to share stdio with the console, not a limitation of `p9serve` itself — a
  physical UART/CP2102 wire has no such multiplexer. Fixed for testing purposes (not a code
  change) by giving the guest's serial port its own `-serial unix:...` chardev socket (with
  `-monitor none` to free stdio entirely) instead of the implicit stdio mux; `tests/p9lib.py` grew
  SLIP-framing support (`framing="slip"`) alongside virtio-console's raw framing so the same client
  library drives both.
- **A3b (RX demux) deliberately not implemented.** Its cost/benefit is unchanged from the
  original framing — highest-risk item in Track A because a regression breaks the console on all
  three targets simultaneously — and `link_virtio_console` now covers the "live 9P wire during an
  interactive QEMU session" use case A3b would have enabled anyway, without touching the console
  path at all. Genuinely still needed eventually for the single-cable RP2350 story (a real
  CP2102/UART deployment has no separate virtio-console channel to fall back on), but not blocking
  anything in this phase.
- **`link_usb_cdc` (RP2350's ACM1/`/dev/ttyACM1`) deliberately not implemented.** Confirmed before
  starting: `drivers/usb_cdc.c` already declares dual CDC-ACM interfaces in its descriptor table,
  but only ACM0 (the console, EP2) has any runtime data path; ACM1/EP4 has zero code behind it —
  `usb_cdc_write_net()`/`usb_cdc_read_net()` are pure stubs that silently forward to
  `uart_putc()`/`uart_getc()` today. Building it means writing an EP4 ring/pump from scratch
  (mirroring EP2's ~150 lines) for RP2350-only, no-CI-value hardware support. Not requested and out
  of scope for this pass; a real candidate for whenever RP2350 hardware-in-the-loop testing (M5)
  is actually being pursued.
- **Fuzz/conformance testing** (the plan's other A2-adjacent bullet) still belongs to A4/T1's
  Python peer, unchanged from A2's own completion notes — A3 gave that peer a real wire to exist
  on, but didn't change the fuzzing scope itself.

### A4 — Multi-node test harness

`tests/runner.py` currently spawns one QEMU per architecture, sequentially, with one chardev. It
needs a multi-node session abstraction: spawn N nodes, wire their chardevs together (TCP or unix
sockets), drive each independently, tear all down deterministically.

`scripts/run-qemu-multinode.sh` exists but cross-connects the two nodes' **consoles**, not a data
channel — it will need rewriting once a real link exists.

Fuzzing and protocol conformance live here too, against the Python peer.

### A5 — Mount table and remote namespace

Not in the original Phase 5 list, but without it "9P works" never becomes "distributed namespace
works", which is the actual Plan 9 payoff.

- Replace `parse_prefix()`'s hardcoded prefix chain with a real mount table.
- Support attaching a remote 9P namespace into the local one (`/net/<node>/…`, or bind semantics).
- Fixes review finding A2 (namespace fall-through) as a side effect, since an explicit mount table
  has no reason to fall back across volumes.

---

## 5. Track B — Memory model & isolation

This is what the **"microkernel"** name needs (V1–V4), and what
[`plan/completed/…§13.2.1`](completed/2026-08-07_review_and_remediation.md) records as the condition
for restoring it to the README title. It is **not** needed for distributed testing.

### The intended split

| | NOMMU (RP2350, QEMU RV32) | MMU (QEMU RV64, later K210 / VisionFive 2) |
|---|---|---|
| Address space | Single, shared | Per-task virtual address space |
| Isolation mechanism | None today; **RISC-V PMP** is available | Sv39 page tables |
| Enforcement granularity | Region (typically 8–16 PMP entries) | Page (4 KB) |

**PMP is a genuinely available middle ground on NOMMU hardware and is worth calling out**: RP2350's
Hazard3 core supports Physical Memory Protection, and `entry.S` already programs `pmpaddr0`/
`pmpcfg0` (currently wide-open, granting S/U full access). Region-granularity protection — "a task
cannot scribble on the kernel or on another task's region" — is achievable on NOMMU without any
MMU. It is not per-task virtual memory, but it is real, enforced isolation, and it makes the
NOMMU/MMU split a difference of *granularity* rather than *presence*.

### B1 — Address-space abstraction
`vmm_space_t` already exists with `page_table_root` / `heap_start` / `heap_end`. Give it two real
implementations behind the existing interface: NOMMU (identity, optionally PMP-enforced) and MMU
(Sv39).

### B2 — Sv39
Three-level page-table walk and allocation; kernel mapping (identity-map first, for simplicity);
`satp` write plus `sfence.vma`; page-fault handling in `trap_handler` (which today halts on any
non-`ecall` exception).

### B3 — Tasks and context switch
`task_t` needs saved callee-saved registers, `sp`, `pc`, a `vmm_space_t *`, and a per-task kernel
stack. Context switch in assembly. Cooperative (`sched_yield`) first; timer preemption after.

### B4 — U-mode and the syscall ABI — **the largest hidden cost**

Two things here are much bigger than the original one-line "5.5 implement the MMU" suggests:

1. **`entry.S` does not switch stacks on trap.** U-mode requires `sscratch` holding the kernel
   stack pointer and a swap on entry/exit. This is surgery on well-tested assembly that every
   trap — including every syscall and every timer tick — flows through.

2. **Copy-in / copy-out.** `trap.c` today passes raw user pointers straight into `printk`,
   `vfs_read`, and `vfs_write`. Once user tasks live in a *different* address space, those pointers
   are meaningless at best and hostile at worst. **Every pointer-taking syscall needs validated
   copying across the address-space boundary.** This is invisible in the original plan wording and
   is realistically the single largest sub-task in Track B.

### B5 — Real IPC
`sys_ipc_call` is a 6-line stub. Real synchronous rendezvous needs B3 first: blocking send/receive
with per-task queues, and `/srv/` becoming real endpoints. Once this exists, the 9P server *could*
become a task — but per [§1](#1-goal-and-the-core-structural-insight) it never has to.

---

## 6. Test topologies

Each rung proves something the previous one cannot.

| | Topology | Proves | Hardware? |
|---|---|---|---|
| **T0** | Loopback through the *real* framing and link layer | Serialization round-trips; replaces today's in-memory shortcut | No |
| **T1** | LugalOS (QEMU) ↔ **Python 9P peer** over pty/socket | **First real wire.** Independent protocol oracle + fuzzing home | No |
| **T2** | LugalOS (QEMU **RV32 NOMMU**) ↔ LugalOS (QEMU **RV64**) over TCP | **First genuinely heterogeneous topology; CI-runnable** | No |
| **T3** | LugalOS (QEMU) ↔ LugalOS (**RP2350 hardware**) over USB CDC / UART | Physical heterogeneity, single-cable story | Yes (opt-in) |
| **T4** | 3-node: workstation + storage node + display node | Multi-hop namespace composition | Mixed |

**T1 deserves emphasis as the highest-value first rung.** A Python reference peer is an
*independent implementation*, so a protocol bug cannot be masked by both ends sharing the same
misunderstanding — which is exactly the failure mode the current loopback path has (it "passes"
while doing nothing over a wire). It is also far easier to instrument and fuzz from than a second
QEMU instance.

**T2 is the milestone that satisfies this phase's stated goal**: two different memory models, two
different word widths, real frames over a real socket, no hardware required, runnable in CI.

---

## 7. Milestones

| | Deliverable | Gates |
|---|---|---|
| **M1** | A1 VFS handles + A2 9P hardened (**B11 closed**) → T0 | — |
| **M2** | A3a headless SLIP link → **T1** (Python peer) | **M1 required** (security gate) |
| **M3** | A3b demux + A4 harness → **T2** heterogeneous CI | M2 |
| **M4** | A5 mount table / remote namespace | M3 |
| **M5** | RP2350 hardware node → **T3** | M4 |
| **M6+** | Track B: PMP / Sv39 / tasks / U-mode / IPC → restore "Microkernel" to the README title | independent of M1–M5 |

**M3 is the point at which this phase's stated goal is met.**

---

## 8. Effort and risk calibration

Relative, not absolute — intended for sequencing decisions, not scheduling.

| Item | Effort | Risk | Note |
|---|---|---|---|
| A1 VFS handles | Medium | Low–Med | Wide blast radius; compat wrappers keep it reviewable |
| A2 9P | Medium | Low | Mostly greenfield in one file, plus the security fix |
| A3a headless link | Low–Med | Low | Reuses working SLIP code |
| A3b RX demux | Medium | **High** | Touches every keystroke on every target |
| A4 harness | Medium | Low | Python only; no kernel risk |
| A5 mount table | Medium | Medium | Changes path resolution everything depends on |
| B1/B2 MMU | High | Medium | Well-understood, just large |
| B3 tasks | High | Medium | New assembly |
| B4 U-mode + copy-in/out | **Highest** | **High** | Surgery on entry.S + every syscall |
| B5 IPC | Medium | Low | Straightforward once B3 exists |

### Items worth an explicit decision before starting

1. **B4 (U-mode + copy-in/copy-out)** — the single largest chunk in Phase 5, and the one that
   destabilizes currently-working, heavily-exercised code (the trap path). It is also
   unambiguously where "microkernel" stops being aspirational. Worth deciding deliberately rather
   than drifting into.
2. **A3b (RX demux)** — moderate effort but the highest-risk item in Track A, because a regression
   there breaks the console on all three targets simultaneously. A3a exists specifically so this
   can be deferred without blocking a real wire.
3. **`link_virtio_console`** — a whole new device driver whose only benefit is nicer CI ergonomics
   on QEMU. It does not advance the hardware story at all. Probably not worth it unless demux (A3b)
   proves genuinely painful.

---

## 9. Decisions needed before starting

These change the plan's content, not just its schedule.

### D1 — Transport strategy
- **(a) SLIP-multiplexed single UART** — one code path for QEMU, RP2350 UART, and CP2102.
  Requires the A3b demux. *Recommended*, with A3a headless first to de-risk.
- **(b) virtio-console on QEMU + USB CDC on RP2350** — cleanest separation, no demux, but two
  unrelated drivers and nothing for a plain UART wire.
- **(c) Headless only** — trivial, but a node under test has no console, so failures are debugged
  blind.

*Recommendation: (a), sequenced as A3a → A3b, with (b) held in reserve.*

### D2 — NOMMU protection
Leave RP2350 as a genuinely flat single address space, or invest in **PMP** region protection?
PMP gives real enforcement on NOMMU hardware and makes the NOMMU/MMU story a difference of
granularity rather than "protected vs. not". It is meaningful extra work and is not required for
anything in Track A.

### D3 — Track priority
Track A to completion first (**recommended** — meets the stated goal soonest, leaves the riskiest
work off the critical path), or interleave Track B to make the microkernel claim real sooner?

### D4 — 9P server execution model
Keep the server poll-driven (works today, no dependencies), or make it a task once B3/B5 land?
Only worth revisiting after Track B; noted here so the A2 fid-table design does not accidentally
assume single-threaded access forever.

---

## Appendix — Review findings this phase closes

| Finding | Description | Closed by |
|---|---|---|
| **B11** | Unbounded 9P serialize/deserialize | A2 (**gates A3**) |
| **B12** | ELF loader trusts `e_phoff`/`e_phnum`/`p_offset`; `code_size` underflow | Original 5.4 — fold into Track B alongside B2, since MMU changes how segments load |
| **A2** | Namespace fall-through across volumes | A5 |
| **A3** | No file handles in the VFS | A1 |
| **V4** | "Scales to 64-bit with MMU protection" — no MMU | B1/B2 |
| **V5** | `/proc` printk's instead of filling buffers; 9P not connected to VFS; `uart_net.c` never touches a UART | A1, A2, A3 |
| **V1–V3** | Microkernel / IPC / scheduler are stubs | B3, B4, B5 |
