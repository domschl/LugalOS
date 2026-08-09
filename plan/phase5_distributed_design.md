# Phase 5 — Distributed Nodes & Memory-Model Isolation: Design & Roadmap

> **Status**: **Track A (Distribution) complete — M1 through M5 all done, M5 verified against real
> RP2350 hardware** (see §7's milestone table and each subsection's dated completion notes below).
> **Track B was redesigned on 2026-08-09**; it is no longer "memory model & isolation" but *a
> microkernel on both memory models* — see §5.0 for the assumption that was dropped and why.
> **B0 (M6), B1 (M7) and B2 (M8) are complete as of 2026-08-09** — the [D5](#d5--track-a-regression-policy-under-a-scheduler--resolved-2026-08-09-hard-gate)
> gate is met. **B3 (M9, U-mode + PMP, NOMMU leads) is next** and is the riskiest milestone in the track. D2, D5 and D6 are resolved;
> D4 is subsumed by B4.
> **Date**: 2026-08-07 (original), updated same day through Track A completion; Track B rewritten
> 2026-08-09.
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
5. [Track B — A microkernel on both memory models](#5-track-b--a-microkernel-on-both-memory-models)
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
memory protection (which is every current target — see [Track B](#5-track-b--a-microkernel-on-both-memory-models)),
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
- **A3b (RX demux) deliberately not implemented in this pass.** Its cost/benefit was unchanged
  from the original framing — highest-risk item in Track A because a regression breaks the console
  on all three targets simultaneously — and `link_virtio_console` covered the "live 9P wire during
  an interactive QEMU session" use case A3b would have enabled anyway, without touching the console
  path at all. Genuinely still needed for the single-cable RP2350 story (a real CP2102/UART
  deployment has no separate virtio-console channel to fall back on) — **built in the A3b pass
  below.**
- **`link_usb_cdc` (RP2350's ACM1/`/dev/ttyACM1`) deliberately not implemented in this pass.**
  `drivers/usb_cdc.c` declared dual CDC-ACM interfaces in its descriptor table, but only ACM0 (the
  console, EP2) had any runtime data path; ACM1/EP4 had zero code behind it. Flagged as "a real
  candidate for whenever RP2350 hardware-in-the-loop testing (M5) is actually being pursued" —
  **built in the A3b pass below.**
- **Fuzz/conformance testing** (the plan's other A2-adjacent bullet) still belongs to A4/T1's
  Python peer, unchanged from A2's own completion notes — A3 gave that peer a real wire to exist
  on, but didn't change the fuzzing scope itself.

#### A3b completion notes (2026-08-07)

Implemented and merged: the shared-wire UART demux, and `link_usb_cdc` (RP2350's ACM1/EP4) — the
two items A3 explicitly deferred, both required for **M5** (an RP2350 hardware node with a real
console). All three targets build clean; full `tests/runner.py` suite passes (85/85, RV64 + RV32 —
one new test, run once per architecture). **Additionally verified against a real, physical RP2350
board** — see "Hardware validation (T3)" below — the first time any Phase 5 work has been checked
against actual hardware rather than only QEMU and clean builds.

- **Opt-in, not always-on — the deliberate de-risking move.** The plan's own risk table flagged
  A3b as high-risk specifically because "a regression breaks the console on all three targets
  simultaneously." Rather than accept that risk unconditionally, the demux ships **disabled by
  default**: `uart_has_char()`/`uart_getc()` on both `drivers/uart_16550.c` (QEMU) and
  `drivers/uart_rp2350.c` read the hardware register directly, exactly as before A3b, unless a user
  explicitly arms it with the new `p9share` shell command (`kernel/shell.c`, alongside the existing
  `p9serve`). Every existing test, every existing interactive session, and every byte path that
  doesn't call `p9share` is provably unchanged — the new code is additional, not a rewrite of the
  console's hot path. `p9share off` reverses it. This is a different tradeoff from `p9serve`
  (A3a): that command *never* returns to the shell (headless, reset to undo); `p9share` returns
  immediately and can be toggled back off, because unlike headless mode it isn't giving up the
  console to get a wire.
- **Demux state machine** (`drivers/uart_net.c`, `uart_demux_*()`): a byte-routing variant of
  `link_uart_slip`'s (A3a) own accumulator. Tracks whether it's currently inside an open SLIP frame
  (started by a `SLIP_END` not yet matched by a closing one); bytes inside go to the same
  accumulate-until-END-then-`slip_decode()` path A3a already has; bytes outside go to a new
  256-byte console ring instead of being discarded. **Known, accepted tradeoff**, documented in the
  header: this framing has no switch character distinct from SLIP's own `END` (`0xC0`) byte, so a
  console byte that happened to *be* `0xC0` would be misread as the start of a 9P frame and
  swallow subsequent keystrokes until the next `0xC0` or a raw-buffer-overflow resync. Plain ASCII
  terminal input — including every VT100/xterm escape sequence `kernel/line_editor.c` parses —
  never produces `0xC0`, so this doesn't bite in practice; it's the other half of why this stays
  opt-in rather than silently always-on.
- **One consumer of the hardware register, not two.** The platform drivers no longer read their
  UART/PL011 RX register directly from `uart_has_char()`/`uart_getc()`; that logic moved into
  private `hw_uart_has_char()`/`hw_uart_getc()` functions, handed to `uart_demux_init()` once at
  boot. When the demux is disabled, `uart_has_char()`/`uart_getc()` call those private functions
  directly (byte-for-byte the old code path). When enabled, only the demux calls them (via its
  `poll()`/background pump), and the console reads exclusively from the ring the demux fills —
  avoiding a race where two independent readers could each consume half of what should have been
  one side's byte.
- **RP2350's `uart_getc()` was missing `p9_link_background_poll()` entirely** — found while wiring
  this up, not something A3b set out to fix. `drivers/uart_16550.c` (QEMU) has called it from
  inside `uart_getc()`'s busy-wait since A3; `drivers/uart_rp2350.c` never did, because
  `kernel/main.c`'s registration of `virtio_console_get_link()` as the background link is itself
  `#if !defined(CONFIG_BOARD_RP2350)`-guarded (virtio-console is QEMU-only). The result: **no
  background 9P link has ever actually run on RP2350 hardware**, regardless of A3/A4/A5 all being
  "done" — those milestones were only ever proven on QEMU. Fixed as part of this pass (now that
  RP2350 has two real background-link candidates to service, below), otherwise `link_usb_cdc` and
  `p9share` would have shipped on RP2350 with no code path that ever polled them.
- **Background link registry generalized from one slot to two**
  (`fs/p9_link.c`/`fs/include/fs/p9_link.h`): `p9_link_register_background()` was a single global
  pointer, sufficient while only one background link (`virtio_console`) ever existed on any one
  target. RP2350 now wants two running at once — `link_usb_cdc` (auto-registered at boot, argued
  below) and, once a user opts in, the UART demux link — and QEMU gains a second slot too (used by
  the new `p9share` regression test below, alongside `virtio_console`). Re-registering an
  already-registered link is a no-op; registering past the two-slot limit is logged and dropped
  rather than silently replacing an existing registration (the old single-slot behavior). Added
  `p9_link_unregister_background()` since passing `NULL` to "unregister" stops being unambiguous
  once more than one slot exists; `p9share off` uses it. This is a config-table change, not a
  concurrency change — the A4 completion notes' own reasoning for why servicing one background link
  from a single-call-stack busy-wait is safe applies identically to servicing two.
- **`link_usb_cdc` (`drivers/usb_cdc.c`, ACM1/EP4)**: a real bulk IN/OUT data path, built by mirroring
  EP2's already-working console implementation (byte rings, `BUFF_STATUS` completion bits, DATA0/1
  PID toggling) at the DPRAM offsets EP4 owns per the existing descriptor table (endpoint-control
  `0x20`/`0x24`, buffer-control `0xA0`/`0xA4`, data buffers `0x200`/`0x240` — verified to not
  overlap EP2's, which end at `0x200`). `ep4_configure()` is called alongside `ep2_configure()` from
  the same `SET_CONFIGURATION` handler; EP4 state is reset on bus reset exactly where EP2's already
  was. **No DTR gating**, unlike EP2: EP2 gates on DTR to avoid replaying a boot-time `printk()`
  backlog to a terminal that opens the port late; this link never proactively sends anything until
  a 9P request arrives, so there's no backlog to avoid, and RX bytes buffered before a peer
  "opens" the port are simply harmless (unread until asked for).
- **Framing: plain length-prefixed, not SLIP** — the same reasoning `link_virtio_console` (A3)
  already established: 9P's own 4-byte size header is sufficient framing over a reliable, ordered
  channel (a USB bulk pipe, like a virtqueue, doesn't need SLIP's escape-and-resync machinery,
  which exists for sharing a wire with non-9P traffic or recovering from a lossy link — neither
  applies here). `usb_cdc_get_net_link()` exposes it as a `p9_link_t`; `send_frame()` is blocking
  (queues into the TX ring, pumping `usb_cdc_task()` if briefly full, then waits for the ring to
  fully drain before returning), matching `virtio_console_send()`'s and `virtio_blk_transfer()`'s
  own busy-wait-until-done precedent rather than inventing a timeout mechanism for just this one
  call site.
- **Auto-registered at boot on RP2350** (`kernel/main.c`), not gated behind an explicit command
  like `p9share`: ACM1/EP4 is a dedicated channel with its own USB endpoint pair, so — like
  `virtio_console` on QEMU — it carries no shared-wire ambiguity and no console risk. The `p9share`
  opt-in gate exists specifically because the UART demux *does* share a wire with the console;
  that reasoning doesn't apply to a second, physically separate USB interface.
- **No interrupt endpoint (EP3, ACM1's CDC notification endpoint) implemented**, matching the
  existing, already-working precedent for ACM0's own EP1: the descriptor table declares it, no
  runtime code backs it, and Linux's `cdc_acm` driver has already been observed tolerating exactly
  this for ACM0 (creates `/dev/ttyACM0` and works regardless). Same expectation applies to ACM1.
- **QEMU regression coverage for the demux** (`tests/runner.py`,
  `test_9p_uart_demux_shared_wire()`): `link_usb_cdc` is RP2350-only and untestable without
  hardware, but the demux is target-agnostic — it's implemented once in `drivers/uart_net.c` and
  used by both platform UART drivers — so it's fully exercisable on QEMU. Modeled on A3a's own
  `test_9p_uart_slip_link()` (same `-serial unix:...` / `-monitor none` workaround for QEMU's
  stdio Ctrl-A mux, described in that test's own docstring), but goes one step further: after
  arming the demux (`p9share`) and completing one real SLIP-framed 9P transaction
  (`tests/p9lib.py`, reading `/sd0/system/init.lisp` end to end), it sends a plain-text console
  command (`help`) over the *same* socket and asserts on a real shell response. This is the one
  proof point p9serve's own headless-mode test structurally cannot offer — its whole premise is
  that the console never comes back — and it's the actual claim A3b makes over A3a: the wire
  carries both, at the same time, without either breaking the other.

#### Hardware validation (T3) — 2026-08-07

With a physical RP2350 board wired up (two USB-CDC ports, plus a CP2102 dongle on GP0/GP1 for the
physical UART), all of the above was exercised directly, not just built. What follows was first
run ad hoc; it's now a permanent, repeatable suite at [`tests/hw/`](../tests/hw/) (a self-contained
`uv` project — `pyproject.toml` + committed `uv.lock`, `pyserial` as the only dependency, kept
isolated from `tests/runner.py`'s deliberately zero-dependency QEMU suite). Every test there skips
cleanly rather than failing when no board is attached, so it's safe to leave out of CI. Building it
surfaced two real bugs, both fixed: port auto-detection was probing with a bare newline, which
desyncs `link_usb_cdc`'s frame parser (no SLIP-style resync-on-garbage) until a real bus reset —
fixed to probe with a complete, self-contained Tversion frame instead; and the QEMU-bridge test's
own cleanup had a race that could abort a USB transfer mid-flight, fixed to join the relay threads
before closing the serial handle.

- **`link_usb_cdc` standalone**: a host Python `p9lib.P9Client` (raw framing) attached over ACM1
  and read `/proc/version` — first response was a genuine, protocol-correct `Rversion`
  (`msize=4096`, `"9P2000"`), then a full `cat()` round trip.
- **`p9share` standalone**: over the CP2102 physical UART, armed the demux, drove one complete
  SLIP-framed 9P transaction, then sent a plain-text `help` command over the *same* connection
  immediately afterward and got a real shell response — console and 9P coexisting on one wire, on
  real hardware, not just in the QEMU regression test.
- **T3 itself — RP2350 talking to a live QEMU node**: bridged RP2350's ACM1 to a QEMU RV64 guest's
  `virtio-console` chardev with a plain byte relay (no re-framing needed — both ends already use
  the same length-prefixed framing, A3/A3b's independent framing choices turning out to be
  interoperable for free). From *inside that QEMU guest's own Lisp REPL*,
  `(p9-remote-cat "/sd0/TEXT.TXT")` returned `"Hello, world!"` — content that exists only on the
  RP2350's physical SD card (confirmed via the RP2350's own `ls`/`cat` first, since it's real
  hardware with its own SD content, not the auto-generated QEMU test image). The request crossed
  QEMU's virtio-console → the host relay → USB → RP2350's `link_usb_cdc` → the 9P server → the VFS
  → the physical SPI SD card, and the response made the same trip back. This is the actual T3
  proof point: two genuinely different LugalOS instances, one of them real silicon, exchanging real
  files over a real wire.
- **Bug found by this, not by any QEMU test**: `tests/p9lib.py`'s `P9Client.cat()` ignored
  `Twalk`'s `nwqid` return value. Pointed at a path that only exists on the QEMU test image
  (`/sd0/system/init.lisp`, absent from this board's actual card), the walk silently stopped one
  component short and `cat()` returned the parent directory's packed-stat listing instead of
  erroring — indistinguishable from a successful read of the wrong thing until inspected closely.
  Every existing QEMU-side caller happened to target paths that always fully resolve, so this had
  never been exercised before. Fixed: `cat()` now raises `P9Error` naming exactly which path
  component the walk got stuck on.

### A4 — Multi-node test harness

`tests/runner.py` currently spawns one QEMU per architecture, sequentially, with one chardev. It
needs a multi-node session abstraction: spawn N nodes, wire their chardevs together (TCP or unix
sockets), drive each independently, tear all down deterministically.

`scripts/run-qemu-multinode.sh` exists but cross-connects the two nodes' **consoles**, not a data
channel — it will need rewriting once a real link exists.

Fuzzing and protocol conformance live here too, against the Python peer.

#### A4 completion notes (2026-08-07)

Implemented and merged: genuine node-to-node 9P over a bridged wire between two heterogeneous
QEMU architectures, satisfying **T2** — the milestone this phase's stated goal is measured
against — without needing A3b or hardware. All three targets build clean; full `tests/runner.py`
suite passes (82/82, RV64 + RV32 + one new multi-node test).

- **The "multi-node session abstraction" turned out to need no new Python plumbing.** QEMU's own
  socket chardev backend bridges two nodes' `virtconsole` ports directly to each other over TCP
  (`server=on,wait=off` on one side, a bare connecting `-chardev socket,...` on the other) — no
  host-side relay process, no new session class. `tests/runner.py`'s existing `QemuSession` already
  supported everything needed (via A3's `extra_qemu_args`); the new test
  (`test_9p_multinode_heterogeneous()`) just boots two independent `QemuSession`s with that flag
  and lets QEMU do the wiring.
- **Genuine node-to-node, not host-to-node** — the thing A3's tests deliberately didn't prove.
  Node B (RV64 Sv39 MMU) writes a marker file to its own `/ram0` that Node A never touches
  directly; Node A (RV32 NOMMU) fetches it via a brand new `(p9-remote-cat "<path>")` Lisp
  primitive and the test asserts on the fetched content. Since the file only exists on B, a match
  is only possible if the bytes genuinely crossed the wire between the two *guest kernels* — proof
  that's stronger than A3's external-Python-client tests could offer (those talked to a single
  node from the host; nothing here has ever proven two LugalOS instances can talk to each other).
- **New shared client capability, not test-only scaffolding**: `p9_link_cat()`
  (`fs/p9_link.c`/`.h`) is a link-agnostic synchronous 9P client — attach at `/`, multi-component
  walk, open, read-loop, clunk — that works over *any* `p9_link_t`, generalizing the pattern
  `loopback_9p_cat()` (A2) established for the loopback-only case. `(p9-remote-cat ...)` is a thin
  Lisp wrapper calling it over `virtio_console_get_link()`; QEMU-only, guarded out on RP2350 like
  the rest of the virtio-console surface.
- **Asymmetric client/server roles, by convention rather than negotiation** — and why that's
  fine. Every node still auto-registers its virtio-console link as a background *server*
  (unchanged from A3). `p9_link_cat()` additionally lets a node act as a *client* on that same
  link for the duration of one synchronous call. These two roles could in principle collide (a
  reply meant for the client being misread as a fresh request by the same node's own background
  pump) — but this kernel has no real task scheduler or interrupt-driven preemption (confirmed
  before relying on it: `kernel/sched.c` is a bookkeeping shim, not a scheduler), so nothing can
  run concurrently with a synchronous C function call. While `p9_link_cat()` is executing, this
  node's own background pump provably cannot run, and the peer never originates traffic of its
  own (it only replies), so there is no actual ambiguity for the "one node queries another" shape
  every current test uses. A true peer-to-peer protocol, where both sides might originate
  requests at arbitrary times, would need real tag-aware multiplexing this doesn't attempt — not
  needed yet, and worth flagging explicitly rather than discovering by accident later.
- **`p9_link_cat()`'s reply-wait is unbounded**, deliberately matching `drivers/virtio_blk.c`'s
  own `virtio_blk_transfer()` precedent (busy-wait until the used ring advances, no timeout)
  rather than inventing a new, bespoke timeout mechanism for just this one call site.
- **`scripts/run-qemu-multinode.sh` rewritten** to bridge the real link instead of the console
  crosswire the plan called out as stale. Node 1 (RV32) stays fully interactive on the terminal as
  before; Node 2 (RV64) runs headless with its console piped through a FIFO so the script can feed
  it one setup command (writing the marker file) without a fragile bash-coprocess dependency.
  Manually verified end-to-end: piping `(p9-remote-cat "/ram0/multinode_marker.txt")` into Node
  1's stdin returns Node 2's real marker content.
- **Fixed TCP port (15590), not ephemeral** — matches the old script's own precedent (which used
  4444) and is simple, but means the multi-node test can't run concurrently with another instance
  of itself on the same host. Acceptable for a sequential CI run; flagged rather than silently
  assumed, and worth an ephemeral-port-plus-readback fix if concurrent runs ever matter.
- **Fuzz/conformance testing against a Python peer**, the other bullet this section names, remains
  explicitly deferred — unchanged from A2 and A3's own notes on this. `tests/p9lib.py` (A3) is
  exactly that Python peer's starting point; nothing in A4 changed the fuzzing scope itself, only
  gave it a second, genuinely independent node to eventually fuzz *between* as well as a host
  client to fuzz *with*.

### A5 — Mount table and remote namespace

Not in the original Phase 5 list, but without it "9P works" never becomes "distributed namespace
works", which is the actual Plan 9 payoff.

- Replace `parse_prefix()`'s hardcoded prefix chain with a real mount table.
- Support attaching a remote 9P namespace into the local one (`/net/<node>/…`, or bind semantics).
- Fixes review finding A2 (namespace fall-through) as a side effect, since an explicit mount table
  has no reason to fall back across volumes.

#### A5 completion notes (2026-08-07)

Implemented and merged: a real mount table, and genuine read/write/`ls` access to another live
node's filesystem through the standard command surface — not a special-purpose primitive, the
actual "distributed namespace" payoff this section is named for. All three targets build clean;
full `tests/runner.py` suite passes (83/83, RV64 + RV32 — one new multi-node test).

- **`parse_prefix()` replaced by `vfs_resolve()` against a real mount table** (`fs/vfs_server.c`).
  Flat, single-component matching only — a mount name is exactly the path's first segment (e.g.
  `sd0`, `proc`, or a user-chosen remote mount name) — deliberately simpler than Plan 9's general
  bind/union-directory model, since a flat table is enough to satisfy this section's actual goal.
  `vfs_resolve()` returns `NULL` for a path whose first component matches no mount, full stop —
  **review finding A2 (namespace fall-through) is now fully closed**: A1 already removed the
  cross-volume *read* fallback as a side effect of routing through `vfs_open()`; this closes the
  other half, the *unrecognized-path-defaults-to-`/flash0/`* branch `parse_prefix()` always had.
- **FAT32 mounts keep the existing `g_flash_mounted`/`g_sd_mounted`/`g_ram_mounted` globals as
  their source of truth**, referenced from the mount table via a `mounted_ptr` indirection, rather
  than folding mounted-state into the mount table entries directly. Deliberate: `vfs_mount_ramdisk()`
  and `vfs_format()`'s already-tested logic keeps flipping the exact same booleans it always did;
  only the *routing* mechanism around them changed, minimizing risk in an already-large refactor.
- **New persistent remote-connection client** (`p9_remote_mount_t`, `fs/p9_link.c`), distinct from
  A4's `p9_link_cat()`: `p9_server_process()`'s `Tversion` handler resets *all* server-side fid
  state by design (a connection reset) — so a mount, which needs file handles to survive across
  separate later `vfs_open()`/`vfs_pread()`/... calls, cannot re-issue `Tversion`+`Tattach` on
  every operation the way `p9_link_cat()`'s one-shot round trips do. `p9_remote_mount_open()` does
  that handshake exactly once and keeps the connection (link + root fid + fid/tag allocators)
  alive for the mount's lifetime. `p9_remote_open/pread/pwrite/readdir/fstat/close/remove/mkdir()`
  mirror the local `vfs_*` handle contracts closely enough that `fs/vfs_server.c`'s dispatch on
  `MOUNT_REMOTE9P` is a thin pass-through — and because `vfs_read()`/`vfs_write()`/`vfs_cp()`/
  `vfs_append()` were already generic over whatever `vfs_open()`/`vfs_pread()`/`vfs_pwrite()`
  resolve to, remote-mount support for all of them came "for free" once `vfs_open()` itself handled
  the new mount kind — no changes needed to those wrapper functions at all.
- **Remote directory listing re-walks the stream from scratch on every `vfs_readdir()` call**,
  rather than caching decoded entries in the handle. `vfs_readdir()`'s contract is index-based
  (0, 1, 2, ...); 9P's `Tread`-on-a-directory-fid is a byte stream of packed `stat` entries. A
  per-handle cache would cost real memory across `VFS_MAX_HANDLES` handles for a feature only
  remote directory handles ever use; re-walking costs O(directory size) network traffic per call
  instead, with zero retained state. Every directory in this system is small, so this is the same
  tradeoff A1's own `fat32_read_at()` already made ("walk from the start every call, revisit if
  it's ever a hot path") — applied to the new remote case rather than invented fresh. Required a
  new client-side `p9_unpack_stat_entry()` (the inverse of `fs/9p.c`'s server-side
  `p9_pack_stat()`), bounds-checked at every step even though the only producer today is this
  project's own trusted server — matching the general B11-era posture of not trusting wire data
  just because nothing hostile sends it yet.
- **`vfs_mount_remote(name, link)` attaches the peer's entire namespace root** at `/<name>/`, not
  just one subtree (`aname` is left empty in the `Tattach`, matching how `p9_link_cat()` already
  attaches at root) — so mounting node B's virtio-console link as `/netb/` exposes B's whole
  namespace: `/netb/flash0/`, `/netb/sd0/`, `/netb/ram0/`, `/netb/proc/`, etc. Exposed as
  `(mount-remote "name")` / `(unmount "name")` Lisp primitives; `mount-remote` is QEMU-only
  (guarded like `p9-remote-cat`, since it currently only ever mounts
  `virtio_console_get_link()` — mounting a different link by name is a reasonable future
  extension once more than one outbound-capable link exists, not needed for this pass).
  `vfs_unmount()` only ever removes mounts added this way; the six built-in mounts aren't
  unmountable through it.
- **Verified through the standard command surface, not a bespoke test primitive**: the new
  `test_9p_remote_mount()` boots two nodes, has Node A `(mount-remote "netb")`, then runs plain
  `ls /netb/ram0/shared`, `cat /netb/ram0/shared/greeting.txt`, and
  `write /netb/ram0/shared/from_a.txt ...` from Node A's ordinary shell — and confirms the write
  landed by reading it back from Node B's own **local** `/ram0/` afterward (not just trusting Node
  A's own success return), so the proof is that bytes genuinely reached Node B's disk, not that a
  command claimed they did.
- **Root `ls` is now genuinely dynamic**, walking the live mount table instead of a hardcoded
  6-entry name array — a remote mount shows up in `ls /` immediately once attached, with no
  separate code path needed.
- **Remote `rmdir` reuses the same `p9_remote_remove()` as remote `rm`** (one `Tremove` call);
  the *server* side (A2's `p9_handle_tremove()`) already dispatches file-vs-directory removal
  itself based on the fid's tracked type, so the client doesn't need to know which one it's asking
  for.
- **Deferred, not forgotten**: `mount-remote` can currently only target the virtio-console link
  (matching `p9-remote-cat`'s existing A4 scope, not a new limitation this section introduced).
  Mounting the *same* underlying link twice isn't meaningful and isn't guarded against — a second
  mount's `Tversion` would reset the first mount's fids server-side (documented in
  `fs/p9_link.c`); real usage is one mount per distinct remote peer, which is what every test here
  does.

---

## 5. Track B — A microkernel on both memory models

> **Revised 2026-08-09.** This section was rewritten after challenging the assumption the original
> version rested on. The old text (B1 Sv39 → B2 tasks → B3 U-mode → B5 IPC) is superseded; its
> technical content survives, resequenced, in B3–B6 below.

This is what the **"microkernel"** name needs (V1–V4), and what
[`plan/completed/…§13.2.1`](completed/2026-08-07_review_and_remediation.md) records as the condition
for restoring it to the README title. It is **not** needed for distributed testing.

### 5.0 — The assumption this track no longer makes

The original Track B was written around an implicit claim: *a true microkernel requires an MMU and
per-process address-space isolation*, so the NOMMU targets get a lesser, monolithic variant and only
the MMU build earns the name.

**That claim is false, and Track A already disproves it in this repository.** It conflates two
separable things:

| | Meaning | Needs an MMU? |
|---|---|---|
| **Structure** | Minimal kernel; drivers, filesystems and services are separate components; all interaction is message-passing over named channels | **No** |
| **Enforcement** | Hardware makes a component's bugs *unable* to reach across the boundary | Yes, for page granularity — but PMP gives region granularity without one |

The proof is [M5](#7-milestones): `(p9-remote-cat "/sd0/TEXT.TXT")`, evaluated inside a QEMU RV64
guest, returned the contents of a file that exists only on a physical RP2350's SD card. A 9P server
on the far end of a USB cable is *already* a separate component sharing no address space with its
client. **A local server in the same address space is a strictly easier case than the one that
already works.** Nothing about the memory model was load-bearing for that result.

Prior art agrees: **F9** is an L4-family microkernel for ARM Cortex-M isolating with only an MPU
(RISC-V PMP is the direct analogue), and **Singularity** was microkernel-structured with no hardware
memory protection whatsoever. The MMU is a dial on enforcement quality, not a switch that turns the
architecture on.

**So both builds get the same microkernel.** The MMU build adds page-granular hardware enforcement;
the NOMMU build adds region-granular PMP enforcement (see [D2](#d2--nommu-protection--resolved-2026-08-09-pmp-early-nommu-leads)).
The value of that symmetry is cross-validation: the same server sources run in both, and the
stricter target catches the bugs the looser one would silently tolerate.

### 5.1 — The three design rules that make this real

These are not descriptions of the outcome; they are constraints that must be imposed from the first
commit, because each one is cheap to honor up front and expensive to retrofit.

#### Rule 0 — The NOMMU build obeys the MMU build's design constraints

**The general paradigm, of which Rules 1–3 are instances.** Wherever the two builds could differ,
the constrained one wins: NOMMU is written as though an address-space boundary were present, even
though it is not. The NOMMU build is never allowed to take a shortcut that the MMU build cannot
also take.

This costs something real (a redundant memcpy here, an extra indirection there) and buys three
things:

1. **One set of server sources**, correct under both builds — the entire justification for building
   both.
2. **The stricter target catches the looser target's bugs**, which is only true if they are running
   the same code.
3. **Remoting comes out nearly free, and this is not a coincidence.** An address-space boundary and
   a network boundary impose the *same* requirement: no shared pointers, everything explicitly
   serialized, failure always possible. Code written to survive the first already survives the
   second. Track A is the existing proof — a 9P server across a USB cable needed no protocol design
   beyond what a local server needs — and B1 deliberately reuses that machinery for the local case
   rather than shortcutting past it.

The inverse framing is the trap to avoid: *"NOMMU is simpler, so let it be simpler."* That produces
two systems, two IPC designs, and no cross-validation.

#### Rule 1 — Copy-always IPC, even where copying is "unnecessary"

The predictable failure mode: on NOMMU it costs nothing to pass a pointer, so the NOMMU IPC quietly
becomes pointer-passing; then the MMU build cannot implement the same ABI without copying; and the
two builds have two different IPC systems, which destroys the entire cross-validation rationale.

**Rule: no syscall or IPC path ever dereferences a caller-supplied pointer. Messages are copied,
always — including on NOMMU where the copy is provably redundant.** A memcpy of a 9P frame is
negligible beside an SPI SD-card read. What it buys is that the *same server source file* is
correct in both builds, which is the whole point.

This is already being violated in embryo. `fs/vfs_server.c`'s `/srv/` write path does:

```c
ipc_msg_t msg_in = { .tag = VFS_TAG_WRITE, .data = { (uintptr_t)buf, len, 0, 0, 0 } };
sys_ipc_call(g_services[i].target_pid, &msg_in, &msg_out);
```

`data[0]` is a raw pointer into the caller's buffer. `kernel/ipc.h`'s `ipc_msg_t` is
register-shaped (`{tag, data[5]}`), which is a fine *notification* primitive but the wrong shape for
services, because `data[N]` inevitably becomes a pointer — as it already has here. B1 replaces this
path.

#### Rule 2 — 9P *is* the service IPC

There is no need to invent an IPC discipline. This tree already has one that is serialized by
construction (hence location-transparent for free), fully bounds-checked (A2 closed B11), and backed
by an independent test oracle (`tests/p9lib.py`). Two layers, and only two:

- **`chan_t`** — the kernel primitive: a bounded, copy-based message channel between two endpoints.
  Small, and the only IPC the kernel itself knows about.
- **9P over `chan_t`** — the service protocol. A local FAT32 server and a remote node's FAT32 server
  then become *the same thing*, differing only in which channel the frames cross.

**Why this is incremental rather than a rewrite**: `fs/vfs_server.c` is already a namespace server
with a real mount table (A5), and `MOUNT_REMOTE9P` is already the mount kind meaning "this subtree
lives in another address space, reached by 9P frames over a link." The microkernel transition is
adding **one more mount kind** — a channel-backed *local* server — through dispatch machinery that
is already written, already tested, and already carrying real traffic between two machines.

#### Rule 3 — One kernel, not a fork

The user framing that prompted this rewrite called Track B a "fork of LugalOS into two
architectures." **It must not be one.** One source tree, one `mem_domain` interface, two backends,
both built and both tested on every commit.

Evidence this rule needs teeth: `arch/riscv/rv64_mmu/vmm.c`'s `vmm_map_page()` is `return 0;` and
`satp` is never written outside `vmm_switch_space()`, which nothing calls. **RV64 and RV32 are today
both effectively flat**, and the MMU backend has already bit-rotted once precisely because nothing
exercised it. If the MMU build is not the primary CI target with the strictest checks, and if every
test does not run on both, the cross-validation benefit is zero and this track's central claim is
decoration.

#### Corollary — "dynamically started drivers" means different things per target

Worth pinning down, because the phrase hides a real capability difference:

- **MMU**: a server can be an ELF loaded at a fixed virtual address per process (and this is where
  finding **B12**, the ELF loader's unvalidated `e_phoff`/`e_phnum`/`p_offset`, finally gets fixed).
- **NOMMU**: there is no per-process link address, so servers are **compiled into the image and
  started dynamically** — `init.lisp` decides *what runs and what it is bound to*, not *which binary
  is loaded from disk*.

That still delivers every scenario driving this track. But "load a third-party server binary at
runtime" is an **MMU-only capability** unless a PIC toolchain is taken on, and the plan should not
imply otherwise.

### 5.2 — What actually blocks the motivating scenarios (measured, not assumed)

Three concrete limitations motivated this rewrite. Checked against the tree, the memory model is not
the cause of any of them:

| Scenario | Actual blocker | Needs a scheduler? |
|---|---|---|
| Kernel logs lost when the UART becomes a 9P wire or a login shell | `printk()` calls `uart_putc()` directly (`kernel/printk.c:169`). No log ring, no sink registry. | **No** |
| Drivers/filesystems should start dynamically, configured from `init.lisp` | `kernel_main()` is a fixed init sequence with `#if defined(CONFIG_BOARD_RP2350)` inline. No device or service registry. | **No** |
| Bind a login shell to a channel that was carrying kernel log | `shell_run()` is called from inside `init.lisp`'s eval (`user/lisp/lisp.c:714`) and never returns. Everything is one call stack — a bound shell has nowhere to stand. | **Yes** |

Two of the three are solvable before any scheduler exists. They are **the first milestones of this
track, not preconditions to it**: "who owns this device, and who decides" is the microkernel's
central question with concurrency subtracted.

### 5.3 — Hard finding: the scheduler invalidates Track A's correctness argument

**This was not recorded anywhere before this revision and is a gate on B2.** A4's completion notes
justify a node acting as both 9P client and server on one link like this:

> *this kernel has no real task scheduler or interrupt-driven preemption … While `p9_link_cat()` is
> executing, this node's own background pump provably cannot run*

That argument dies the moment a scheduler exists. `p9_link_cat()` (`fs/p9_link.c:451`) spins
unboundedly awaiting a reply; under a scheduler that spin becomes a yield point, this node's
background pump *does* get to run, and it will consume the reply frame as though it were a fresh
inbound request. **Tag-aware 9P multiplexing is therefore not future work — it is a hard
prerequisite of the first scheduler milestone**, or M3/M4/M5 silently regress with no test failing
for the right reason.

The same shape recurs across the tree: every blocking path is a busy-wait that assumes nothing else
can run — `uart_getc()` (`drivers/uart_16550.c:61`, `drivers/uart_rp2350.c:190`),
`virtio_blk_transfer()`, the RP2350 I²C/SPI/USB polls. Each must become an explicit yield point or
it becomes a starvation bug.

This is the main argument for **cooperative scheduling first**: with no preemption there is no
locking to design, and the conversion is mechanical (`while (!done) sched_yield();`). Preemption is
where NOMMU turns genuinely dangerous (a preempted server caught mid-update of a global, with no
hardware to stop the damage) and belongs behind its own milestone, after PMP.

### 5.4 — Milestone ladder

Ordering rationale: **B0–B2 deliver every structural benefit above, are testable on both targets,
and carry low risk.** B3–B5 then add *enforcement* to a structure that already works and is already
under test — the reverse of the original plan, which put MMU and U-mode surgery first, before there
was anything to isolate.

#### B0 — Structure without concurrency

No scheduler. Kills two of the three §5.2 blockers outright.

- **Kernel log ring + sink registry.** `printk()` writes to a ring; sinks (UART, USB CDC, later a
  `/dev` node or a 9P-served `/proc/kmsg`) subscribe. A UART that becomes a 9P wire or a login shell
  detaches its sink instead of silently swallowing the log. Preserve `printk_debug()`'s existing
  UART-only guarantee (it exists so USB tracing cannot recurse into USB traffic).
- **Device + service registry.** Drivers register capabilities; nothing is bound at compile time.
- **`kernel_main()` reduced** to registry bring-up plus "run init", with the `#if
  defined(CONFIG_BOARD_RP2350)` blocks becoming per-board registration tables.
- **`init.lisp` binding primitives** — bind a channel to a service, list/attach/detach.

*Risk: low. Blast radius is `printk()` and boot ordering, both well covered by the existing 85-test
suite.*

##### Known issue — `uart_putc()` blocks unbounded (found 2026-08-09, not fixed)

`drivers/uart_16550.c`'s `uart_putc()` spins on the 16550's THRE bit with no
bound and no yield:

```c
while ((uart_base[UART_LSR] & UART_LSR_THRE) == 0);
```

If console output ever backs up, the guest wedges there permanently — no
timeout, no yield, no recovery. It was found by a real symptom: adding ~27
bytes to `/proc/version` deterministically tipped `tests/runner.py` into a
state where `cat /proc/kmsg` (a ~4 KB dump of the whole log ring) went silent
mid-write and every subsequent test failed with empty output. The read itself
completes correctly; the stall is in emitting.

**Not root-caused, and deliberately not papered over in code.** What is
established: the read returns its full 4094 bytes; QEMU and the harness reader
thread both stay alive; a wrapped-ring dump in isolation completes in 0.1 s.
What is not established is why the same dump wedges only after ~25 preceding
tests.

Worked around by *not* growing `/proc/version` — the build id lives in its own
`/proc/buildid` instead. That removes the trigger without pretending the
underlying fragility is gone.

**`sched_yield()` was deliberately NOT added to this spin**, unlike
`uart_getc()`'s in B2. Yielding mid-`printk()` would let two tasks interleave
output character by character, since `printk()` is not atomic and there are no
locks yet. That trade only becomes acceptable once B4 gives the console a
single owning server. Recorded here so B4 picks it up rather than rediscovering
it.

##### B0 completion notes — part 1 of 3 (2026-08-09)

**Log ring + sink registry implemented and merged.** The device/service registry and the
`kernel_main()` reduction / `init.lisp` binding primitives are **not** done — B0 remains open. All
three targets build clean; `tests/runner.py` passes **89/89** (RV64 + RV32, four new tests: two per
architecture, up from 85).

- **`kernel/klog.c` + `kernel/include/kernel/klog.h`**: a 4 KB ring plus a 4-slot sink registry.
  `printk()` now writes to `klog_putc()` instead of `uart_putc()`; boot registers a `"console"` sink
  whose putc *is* `uart_putc`, so default output is byte-identical to the pre-B0 path. The ring is
  written unconditionally, which is the actual fix — detaching a sink is now non-destructive.
- **Sinks are addressed by name, not by index**, and re-registering an existing name replaces its
  function rather than consuming a second slot. `klog_sink_register()` must be called before
  anything can `printk()`; it is the first statement in `kernel_main()` after `uart_init()`, ahead
  of `time_init()`, which logs.
- **`printk_debug()` deliberately left on the direct path**, not routed through klog. Its documented
  guarantee is "physical UART only, never mirrored to USB", and the ring is served by `/proc/kmsg`,
  which a *remote 9P client can read* — low-level USB/I²C/SPI tracing does not belong in a file
  other nodes fetch. Keeping it direct preserves the guarantee without needing a sink-policy
  argument.
- **`/proc/kmsg`** serves the ring through A1's existing handle API, so it is readable over 9P by
  another node — kernel logs were never remotely readable before. It does **not** use the 512-byte
  per-handle `proc_buf` (far too small); instead the handle snapshots the window
  `[klog_oldest(), klog_total())` at `vfs_open()` time and `vfs_pread()` serves from the ring by
  absolute position. **The snapshot is load-bearing, not tidiness**: `cat /proc/kmsg` prints via
  `printk()`, which appends to the same ring, so a read tracking the live end would feed itself its
  own output and never terminate.
- **`klog_read()` clamps a caller that fell off the back of the ring** to the oldest byte still
  held, rather than returning wrapped garbage.
- **Re-entrancy guard** (`g_in_fanout`) so a future sink whose putc logs about its own failures
  can't recurse until the stack dies. It is *not* a concurrency lock — B2 must revisit this.
- **New `klog` shell command** (list / `klog attach <sink>` / `klog detach <sink>`), matching the
  `p9serve`/`p9share` precedent of exposing new plumbing through an explicit command.
- **Bug avoided, worth recording**: the first version of the `klog` listing used `printk("%-10s")`.
  This kernel's format engine (`kernel/printk.c`) accepts only `0`, width, `.prec` and `l` — there
  is **no `-` (left-justify) flag**, so that would have printed the format spec literally.
- **`vfs_readdir()`'s `/proc` branch had a hardcoded `index >= 4`** next to a 4-entry name table;
  adding `kmsg` required changing a literal in a second place. Replaced with a `sizeof()`-derived
  count so the next addition can't drift.
- **Test falsification, not just a green run**: `klog_sink_detach()` was temporarily sabotaged to a
  no-op and the suite re-run, confirming the new test fails on both architectures (87/89) rather
  than passing vacuously. Restored afterwards. The test stages a marker into a file *before*
  detaching, so the marker never appears in a typed command — `kernel/line_editor.c` echoes
  keystrokes via `uart_putc()`, bypassing klog entirely (which is also why typing still works with
  every sink detached), so a marker in the typed text would have made the silence assertion
  meaningless.

##### B0 completion notes — part 2 of 3 (2026-08-09)

**Device registry implemented and merged.** The `kernel_main()` reduction and `init.lisp` binding
primitives remain — B0 is still open. All three targets build clean; `tests/runner.py` passes
**91/91** (RV64 + RV32, two more new tests, up from 89).

- **`kernel/device.c` + `kernel/include/kernel/device.h`**: drivers publish a `dev_driver_t`
  (`name`, `kind`, `flags`, `probe`, `get`); a per-board table decides which exist; `kernel_main()`
  just probes the table. Kinds are `CONSOLE`/`P9LINK`/`CLOCK`/`EEPROM`/`BLOCK`.
- **`kernel/board.c` concentrates the `#if`s.** They did not disappear — CMake compiles a different
  driver set per board, so they never could — but they no longer interleave with initialization
  order and 9P link policy inside the boot path. `board_uart_base()` replaces the inline
  `0x40070000` / `0x10000000` split.
- **`DEV_F_BACKGROUND_9P` replaced the `#if defined(CONFIG_BOARD_RP2350)` block that chose which
  link serves inbound 9P.** `kernel_main()` now iterates `dev_next_with_flags()` and registers
  whatever the board's table flagged — `vconsole` on QEMU, `usbnet` (ACM1/EP4) on RP2350. The
  UART-backed links (`uartslip`, `uartdemux`) are registered as devices but deliberately *without*
  the flag, so they remain behind explicit `p9serve` / `p9share`, exactly as A3b established.
- **Layering kept clean deliberately**: `dev_next_with_flags()` returns opaque objects so
  `kernel/device.c` needs no `fs/p9_link.h` dependency — the 9P knowledge stays in `kernel_main()`.
  The obvious shortcut (having `dev_probe_all()` register background links itself) would have put
  a filesystem dependency in the generic device layer to save four lines.
- **Probe reordering checked, not assumed.** `virtio_console_init()` used to run *after*
  `vfs_server_init()`, and virtio-blk claims an MMIO slot lazily during mount. Both probes match on
  `REG_DEVICE_ID` (block=2, console=3), so neither can claim the other's slot at any ordering —
  verified by reading both probe loops before moving anything.
- **`/proc/devices`** exposes the registry, so it is readable over 9P from another node like every
  other `/proc` file.
- **The `-` flag trap bit again**: the first `/proc/devices` version produced ragged columns for the
  same reason the `klog` listing nearly did. Fixed properly this time with an `append_col()` helper
  in `fs/vfs_server.c` rather than by giving up on alignment.
- **Test falsification, again non-vacuous**: removing `DEV_F_BACKGROUND_9P` from the `vconsole`
  table entry fails exactly the four virtio-console 9P tests (87/91) — confirming the new
  flag-driven path really is what registers the background link, not a leftover from the old `#if`.
  Restored afterwards.
- **`/srv/`'s service registry (`vfs_register_service()`) left alone.** It maps a name to a
  placeholder PID and is the stub [B1](#b1--chan_t-and-the-local-channel-backed-mount) replaces
  with real `chan_t` endpoints; half-merging it into the device table now would mean rewriting it
  twice.

##### B0 completion notes — part 3 of 3 (2026-08-09) — **B0 COMPLETE**

**Binding primitives implemented and merged. B0 is done; M6 is complete.** All three targets build
clean; `tests/runner.py` passes **95/95** (RV64 + RV32, four more new tests, up from 91).

- **New Lisp primitives**: `(devices)`, `(dev-present? "name")`, `(klog-sinks)`,
  `(klog-detach "name")`, `(klog-attach "name")`, `(p9-serve "dev")`, `(p9-unserve "dev")`.
  Policy — which link serves 9P, where the kernel log goes, what hardware a boot script assumes —
  is now expressible in `init.lisp` rather than compiled into `kernel_main()`.
- **`(devices)` reads `/proc/devices`** rather than formatting the table a second time, so what the
  REPL prints is byte-identical to what a remote 9P client reading that file sees. No divergent
  second formatting path.
- **`mount-remote` and `p9-remote-cat` are no longer RP2350-guarded.** Both took their link from a
  hardcoded `virtio_console_get_link()`; they now resolve through the registry
  (`lisp_resolve_link()`), so both gained an optional device-name argument *and* RP2350 can use them
  over its `usbnet` (ACM1/EP4) link. **This closes A5's "deferred, not forgotten" item** —
  "`mount-remote` can currently only target the virtio-console link". Omitting the argument selects
  the board's `DEV_F_BACKGROUND_9P` link, so every existing call site keeps working unchanged.
- **`(p9-serve ...)` returns `#t` for "requested", not "succeeded"**, and says so:
  `p9_link_register_background()` returns void and logs-and-drops past its two-slot limit (A3b), so
  there is no success code to forward honestly. It does *not* arm the UART demux — `uartdemux` still
  needs `p9share`, because sharing a wire with the console is a driver-mode change, not just a
  registration.
- **`init.lisp` documents the new surface** in comments rather than gaining active lines that do
  nothing: dedicated links are already served from boot via `DEV_F_BACKGROUND_9P`, so no binding is
  *required* there by default.
- **A brittle test assertion fixed at the cause.** Three 9P transport tests asserted
  `len(data) == 515` — the literal byte length of `init.lisp` — so editing the boot script broke
  three unrelated tests with a confusing "unexpected content" message. The length check is worth
  keeping (it proves a complete multi-read transfer, not a truncated one), so it now derives the
  expected length from the source file instead of hardcoding it.
- **Falsification caught a vacuous test of mine — the most useful result of this pass.** The
  assertion "an unknown link name must be rejected" was first placed in the single-node arch suite,
  where it passed *even with name resolution deliberately deleted*: no virtconsole is attached
  there, so the fallback link is absent too and `#f` comes back either way. Moved to
  `test_9p_remote_mount()`, where the default link genuinely exists and works, the sabotage is
  caught (94/95). **The lesson generalizes: a negative assertion only discriminates when the thing
  it would have fallen back to is present and working.**

**Known limitation, deferred to B4 — `printk()` is still one stream carrying two things.** It is
both kernel diagnostics *and* user-facing output (shell command results, Lisp REPL output — its own
header comment says so). So `klog detach console` currently silences **everything** on that
terminal, not just log lines. That is the correct semantic for `p9serve` (the terminal has become a
9P wire; nothing should print to it) but it is *not yet* the full §5.2 scenario-1 story, which wants
kernel log and login-shell output to be separately routable. Splitting them means touching several
hundred `printk()` call sites and properly belongs to **B4**, where the console becomes a server
that owns its own output stream. What B0 delivers today is the part that needed no scheduler: the
log is retained regardless of sink state, and it is readable locally *and remotely* after the fact.

#### B1 — `chan_t`, and the local channel-backed mount

- **`chan_t`**: bounded, copy-based message channel. **Rule 1 applies from this commit.**
- **Replaces the `/srv/` pointer-passing path** in `fs/vfs_server.c` and retires the `ipc_msg_t`
  pointer-in-`data[0]` pattern.
- **`MOUNT_LOCAL9P`** — a mount served by a local channel endpoint, alongside A5's existing
  `MOUNT_REMOTE9P`.
- **Testable without tasks**: `drivers/loopback_net.c` becomes a `chan_t`-backed local 9P link, so
  B1 is provable by the existing loopback tests plus a new local-mount test — a real proof that a
  local server and a remote server are the same code path.

*Risk: low–medium. New primitive, but it lands beside an already-working remote equivalent.*

##### B1 completion notes (2026-08-09) — **B1/M7 COMPLETE**

All three targets build clean; `tests/runner.py` passes **103/103** (RV64 + RV32, eight new tests —
four per architecture, up from 95).

- **`kernel/chan.c` / `kernel/include/kernel/chan.h`**: named server endpoints, synchronous
  request/response, **two copies per call** (request in, response out) so a handler only ever
  touches endpoint-owned memory. Buffers are supplied by the registrant — this kernel has no
  malloc, and it keeps the layer free of any assumption about message size.
- **No queue depth, deliberately.** With no scheduler a message can never wait for a receiver that
  isn't already running, so a depth-N ring would be untestable capacity — a liability, not a
  feature. **B2 must revisit**: `chan_call()` splits into a real blocking rendezvous and the `busy`
  flag becomes a wait queue.
- **`fs/p9_chan.c`**: the local 9P server as a channel endpoint, exposed as an ordinary `p9_link_t`.
  `p9_server_process()` already had exactly the handler shape required, which is not luck — A3 wrote
  it to be driven from a wire.
- **There is no `MOUNT_LOCAL9P`, and that is the result rather than a shortcut.** `vfs_mount_local()`
  is `vfs_mount_remote()` handed a channel-backed link; every layer below — `p9_remote_open/pread/
  pwrite/readdir`, `vfs_open()`'s `MOUNT_REMOTE9P` branch — is reused unchanged. Adding a parallel
  local mount kind would have *weakened* the demonstration it exists to make.
- **`/srv/` no longer passes a caller pointer.** The write path built
  `ipc_msg_t{.data = {(uintptr_t)buf, len, …}}` and handed it to `sys_ipc_call()` — the exact
  shortcut §5.1 predicted would force the two builds apart. It now goes through `chan_call()`. A
  declared service with no channel bound reports failure instead of the old unconditional `0`:
  claiming a write succeeded when nothing consumed it is worse than an honest error.
- **`loopback_9p_cat()` deleted (~60 lines)** in favour of `p9_link_cat()` over the local link — the
  *same client code* now drives the local server and a peer across a USB cable. `loopback_send_9p()`
  also routes through the channel, so every pre-existing loopback test exercises copy-always IPC.
- **New `(mount-local "name")` Lisp primitive.**

**Bug found by this work, and the reason the local case was worth building
(`fs/vfs_server.c`, `vfs_open()`):** the handle table's slot was marked `in_use` only on the success
path at the very end. Harmless while every backend was a straight-line local call. Once a mount can
be served by *this same node* over a channel, opening `/<local>/proc/version` makes the 9P server
re-enter `vfs_open()` for `/proc/version` while the outer call is still in flight; the inner call
found the slot still free, took it, and the outer call then overwrote the inner handle with its own
remote state. The server's fid then referred to a `MOUNT_REMOTE9P` handle, so its next `vfs_pread()`
bounced straight back into the channel and was refused as re-entrant — surfacing as a bare
"cannot read path" two layers from the cause. Fixed by reserving the slot *before* any work that can
re-enter, and releasing it on failure.

**A truly remote peer could never have exposed this** — its `vfs_open()` runs on another machine.
That is the concrete argument for Rule 0 beyond tidiness: forcing the local case through the remote
machinery puts client and server in one address space, where shared-state bugs become reachable
*and* reproducible in CI, instead of waiting for B3's hardware boundary to turn them into faults.

- **Verified by falsification**: reverting the slot reservation to its old position fails the
  local-mount read test on both architectures (101/103). Recursive `/self/self/…` is covered too —
  it must fail cleanly rather than hang or corrupt the outer call's single-slot buffer.
- **Honest limit on what the tests prove.** The copies themselves are *not* observable on a
  single-address-space build: deleting them and passing the caller's pointer through would pass
  every test here. Rule 1 is therefore enforced by construction and review, not by assertion, until
  **B3** puts a real boundary in place and makes a violation fault. What the tests *do* cover is the
  bounded behaviour around the copies — capacity rejection and the re-entrancy guard.

#### B2 — Tasks, cooperative scheduling — **gated on Track A staying green**

The first genuinely large step.

- **Real `task_t`**: saved callee-saved registers, `sp`, `pc`, a `mem_domain *`, and a per-task
  kernel stack. Today it is `{pid, state, sp, name}` and `task_create()` discards its entry point.
- **A real allocator.** `vmm_alloc_page()` is a bump pointer that never frees
  (`arch/riscv/*/vmm.c`), and each target has exactly one `_stack_top`. Per-task stacks need
  genuine allocation — this is an unglamorous prerequisite that the original plan never named.
- **Cooperative context switch in assembly**; `sched_yield()` becomes real. `kernel/sched.c` is
  today a 50-line bookkeeping shim that only prints.
- **Convert every busy-wait to a yield point** (§5.3).
- **9P tag multiplexing** (§5.3).

**Exit criterion, non-negotiable**: `test_9p_multinode_heterogeneous()` and `test_9p_remote_mount()`
still pass **with the scheduler enabled**. Track A is not allowed to regress silently to buy Track B
progress.

*Risk: medium–high. New assembly, and it touches every blocking path in the tree.*

##### B2 completion notes (2026-08-09) — **B2/M8 COMPLETE**

All three targets build clean; `tests/runner.py` passes **107/107** (RV64 + RV32, four new tests,
up from 103). Landed in two commits: allocator + tasks + switch, then yield points + tag
multiplexing.

- **`kernel/palloc.c`** — bitmap page allocator. What it replaced could neither free nor fail: each
  arch's `vmm_alloc_page()` was a bump pointer with **no upper bound**, so on RP2350 enough
  allocations would have walked through the scratch regions and the boot stack with no diagnostic.
  The linker scripts now export `_heap_end`, making the bound a property of each board's memory map.
  `/proc/meminfo` reports real counters.
- **`arch/riscv/common/switch.S`** — one cooperative switch for both word widths via the existing
  `REG_S`/`REG_L` macros (Rule 0 at the lowest level). **FP state is deliberately not saved, and the
  reason was checked rather than assumed**: `entry.S` does `csrw mstatus, zero` and never sets
  `mstatus.FS`, so the FPU is off and no task can hold live FP state. If FS is ever enabled this
  must grow `fs0-fs11` saves (RV64 uses lp64d, where those are callee-saved).
- **`kernel/sched.c`** — real tasks, palloc'd stacks, round-robin, block/unblock, and a
  `task_exit()` that reclaims the stack. The boot context becomes task 0. `task_t` carries a
  `domain` pointer, NULL everywhere today, that B3 fills with a PMP region set and B5 with an Sv39
  page table — the NOMMU build does not get a simplified task.
- **`/proc/ps` renders the real table.** It was a hardcoded string naming four tasks that never
  existed; two tests had been asserting on those invented names.

**Bug caught by a test rather than by review.** An earlier `sched.c` had a "currently switching"
guard that silently broke cooperative scheduling: a freshly created task enters at
`task_trampoline` and never returns from `ctx_switch()`, so it never reached the line clearing the
flag — its own `sched_yield()` then saw the flag still set and returned immediately, running the
task to completion instead of yielding. The guard was unnecessary (no window exists between the
state updates and `ctx_switch()`) and is gone. **This is why the test asserts on interleaving order
(`A1 B1 A2 B2 A3 B3`) rather than on output appearing**: every marker still printed with switching
completely broken.

**Yield points**: `uart_getc()` on both platform UARTs (the console blocking on a keystroke is the
longest wait in the system and so the most important one), and `virtio_blk_transfer()`'s completion
poll.

**D5 gate — satisfied, and genuinely exercised.** Inbound frames now pass through one routing point
(`p9_route_frame()`) shared by the background pump and by client waits. 9P makes the classification
exact: T-messages (requests) have even type numbers, R-messages (replies) odd ones, for every pair.
A reply is matched to a registered waiter by tag; a request goes to the server. Waiters live in a
small fixed table rather than in `p9_link_t`, so no backend changed and links that never act as
client pay no per-link reply buffer.

- **A4's safety comment was false as written and has been rewritten, not deleted.** It argued the
  client/server overlap was safe *because there was no scheduler*. That reasoning is now wrong; the
  code is safe for a different reason, and recording the change matters more than quietly fixing it.
- **Passing "with the scheduler on" is not enough on its own, which nearly produced a hollow gate.**
  With only the boot task alive, `sched_yield()` has nobody to switch to, so the dangerous
  interleaving never happens and the multi-node tests would pass whether or not frames were routed.
  A new `(spawn-pump n)` primitive creates a task that services background links and yields, so the
  client's reply really is read off the wire by *another task* mid-exchange. The multi-node test now
  spawns it before `(p9-remote-cat …)`.
- **Verified by falsification**: routing every inbound frame to the server, as before B2, collapses
  the suite to **35/107** — including both multi-node tests. The breadth is expected, since the
  local channel link's replies are misrouted too.

**Deferred to B6, explicitly**: `task_exit()` frees its own stack while still running on it. Safe
only because cooperative scheduling means nothing can allocate and reuse those pages before the
final `ctx_switch()`. Under preemption this must move to a reaper that frees after the switch.
Likewise `palloc` and `sched` take no locks, which is correct only without preemption.

#### B3 — U-mode + PMP on the NOMMU targets — **NOMMU leads**

Per [D2](#d2--nommu-protection--resolved-2026-08-09-pmp-early-nommu-leads), enforcement arrives on RV32/RP2350 *first*, and Sv39
follows the pattern it establishes rather than the other way round.

- **Trap-path stack switch.** `mscratch` (RV32/RP2350 are `CONFIG_MODE_M`) / `sscratch` (RV64 is
  `CONFIG_MODE_S`) holding the kernel stack, swapped on entry and exit. Verified: **neither CSR is
  referenced anywhere in the tree today**, and `entry.S`'s trap vector reuses the interrupted `sp`.
  This is surgery on assembly that every trap flows through.
- **M→U transition** on RV32/RP2350 (`mstatus.MPP = 0`), mirroring the existing M→S sequence.
- **PMP region programming per task.** Note `entry.S` touches `pmpaddr0`/`pmpcfg0` **only under
  `CONFIG_MODE_S`** — so RP2350 (`CONFIG_MODE_M`) starts from zero here, not from the wide-open
  configuration the original Track B text implied was already in place.
  - *Measure first*: the RISC-V spec permits 0, 16, or 64 PMP entries and Hazard3's actual count
    must be read back from silicon (write all-ones to `pmpaddrN`, read back) rather than assumed.
    The region budget shapes how many isolated servers a NOMMU node can host.
- **Copy-in / copy-out at the syscall boundary.** `arch/riscv/common/trap.c` passes `frame->a1`,
  `frame->a2` straight into `printk`, `vfs_read`, `vfs_write`. Every pointer-taking syscall needs
  validated copying. **Rule 1 means the ABI needs no redesign at this point — only enforcement**,
  which is precisely the payoff for having imposed it in B1.
- Brought up against a **minimal U-mode test task**, not a real server — there must be something in
  U-mode to protect before B4 puts anything valuable there.

*Risk: high. The single riskiest milestone in the track.*

#### B4 — Servers leave the main call stack

- **Console/log server** first (owns the UART/USB-CDC console and the B0 log ring), then a
  filesystem server.
- `init.lisp` binds channels to services — **this is scenario 1 delivered in full**: a UART carries
  kernel log output until `init.lisp` binds a login shell to it.
- The 9P server itself may become a task here, resolving [D4](#d4--9p-server-execution-model--subsumed-by-b4).

#### B5 — Sv39, the MMU backend

Unchanged in content from the original B1/B2, but now implementing an interface B3 already defined
and tested: three-level walk and allocation, kernel identity map first, `satp` + `sfence.vma`,
page-fault handling in `trap_handler` (which today halts on any non-`ecall` exception). Closes
**V4**.

#### B6 — Preemption, and ELF-loaded servers

- Timer preemption (the point at which NOMMU genuinely needs B3's PMP to already be in place).
- ELF-loaded servers, **MMU only** per §5.1's corollary. Closes **B12**.

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

| | Deliverable | Gates | Status |
|---|---|---|---|
| **M1** | A1 VFS handles + A2 9P hardened (**B11 closed**) → T0 | — | Done |
| **M2** | A3a headless SLIP link → **T1** (Python peer) | **M1 required** (security gate) | Done |
| **M3** | A3b demux + A4 harness → **T2** heterogeneous CI | M2 | Done — A4 (2026-08-07) reached T2 over `virtio_console`/TCP without needing A3b, per its own completion notes; A3b itself (shared-wire demux) landed separately, below, once RP2350 hardware support made it relevant rather than as a T2 dependency. |
| **M4** | A5 mount table / remote namespace | M3 | Done |
| **M5** | RP2350 hardware node → **T3** | M4 | **Done (2026-08-07) — verified against real RP2350 hardware, not just clean builds.** A3b demux + `link_usb_cdc` (below) gave RP2350 both a single-cable UART story (`p9share`) and a dedicated USB channel (ACM1/EP4); with a physical board wired up, both were exercised directly: `link_usb_cdc` served a real `/proc/version` read to a host Python 9P client over ACM1, and `p9share` carried a real SLIP-framed 9P transaction *and* a live console command over the same physical UART. The actual T3 milestone — **RP2350 hardware talking 9P to a QEMU node** — was then proven for real: RP2350's ACM1 was bridged (a plain byte relay; both ends already speak the same length-prefixed framing) to a QEMU RV64 guest's `virtio-console` chardev, and `(p9-remote-cat "/sd0/TEXT.TXT")`, run from *inside that QEMU guest's own Lisp REPL*, returned `"Hello, world!"` — content that exists only on the RP2350's physical SD card. The round trip crossed QEMU's virtio-console → a host relay → USB → RP2350's `link_usb_cdc` → the 9P server → the VFS → the physical SPI SD card, and back. Found and fixed along the way: `tests/p9lib.py`'s `P9Client.cat()` discarded `Twalk`'s `nwqid`, so a partial walk (e.g. a path that doesn't exist on this board's card) silently returned the wrong directory's listing instead of erroring — it now raises `P9Error` naming exactly which path component it got stuck on. |
| **M6** | **B0** log ring + sink registry, device registry, `init.lisp` binding | independent of M1–M5 | **Done (2026-08-09)** — see B0's three completion notes. Delivered in three commits: klog ring + detachable sinks + `/proc/kmsg`; per-board device registry + `/proc/devices` replacing `kernel_main()`'s `#if` blocks; Lisp binding primitives. Side effects beyond scope: A5's "`mount-remote` can only target virtio-console" limitation is closed, and `mount-remote`/`p9-remote-cat` now work on RP2350 over `usbnet`. Tests 85→95. One limitation deferred to B4: `printk()` is still a single stream carrying both kernel diagnostics and user-facing output. |
| **M7** | **B1** `chan_t` copy-always channels + local channel-backed mount | M6 | **Done (2026-08-09)** — see B1's completion notes. `kernel/chan.c` (copy-always endpoints), `fs/p9_chan.c` (local 9P server as an ordinary `p9_link_t`), `vfs_mount_local()`. Notably there is **no** `MOUNT_LOCAL9P` kind: a local mount is `vfs_mount_remote()` with a channel-backed link, reusing every layer below unchanged. `/srv/`'s pointer-passing retired; `loopback_9p_cat()` (~60 lines) deleted in favour of the shared `p9_link_cat()`. Found and fixed a latent `vfs_open()` handle-slot reservation bug that only a re-entrant (local) mount can expose. Tests 95→103. |
| **M8** | **B2** tasks + cooperative scheduler + 9P tag multiplexing | M7 | **Done (2026-08-09)** — see B2's completion notes. Page allocator (replacing an unbounded bump pointer), `switch.S` cooperative context switch shared by both word widths, real `task_t`/`sched.c`, yield points in `uart_getc()`/`virtio_blk`, and `p9_route_frame()` type-parity + tag demultiplexing. **D5 gate met and genuinely exercised**: a new `(spawn-pump)` task runs concurrently with the client exchange, since with only the boot task alive the hazard never occurs and the gate would have been hollow. Tests 103→107. |
| **M9** | **B3** U-mode + PMP on RV32/RP2350; trap-path stack switch; copy-in/out | M8 | Not started |
| **M10** | **B4** servers off the main call stack; `init.lisp`-bound console/log and filesystem | M9 | Not started |
| **M11** | **B5** Sv39 on RV64 → restore "Microkernel" to the README title (V1–V4 closed) | M9 | Not started |
| **M12** | **B6** preemption; ELF-loaded servers (MMU only, closes B12) | M10, M11 | Not started |

**M3 is the point at which this phase's stated goal is met** — already true as of A4/A5. A3b and
`link_usb_cdc` were aimed at M5, the RP2350 hardware milestone, also done.

**M6 onward is Track B as revised in [§5](#5-track-b--a-microkernel-on-both-memory-models).** Note
M11 (Sv39) is gated on M9, not on M10 — once B3 has defined and tested the `mem_domain` interface on
the NOMMU targets, the MMU backend and the server migration are independent and can proceed in
either order or in parallel.

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
| **B0** registries + log ring | Low–Med | **Low** | No scheduler; blast radius is `printk()` and boot order |
| **B1** `chan_t` + local mount | Medium | Low–Med | New primitive, but lands beside a working remote equivalent |
| **B2** tasks + tag multiplexing | High | **Med–High** | New assembly; touches every blocking path; **gated on Track A staying green** |
| **B3** U-mode + PMP + copy-in/out | **Highest** | **High** | `mscratch`/`sscratch` surgery on the trap path every syscall flows through |
| **B4** servers as tasks | Medium | Medium | Mostly mechanical once B1–B3 exist |
| **B5** Sv39 | High | Medium | Well-understood, just large — and B3 already defined the interface |
| **B6** preemption + ELF | High | **High** | Preemption on NOMMU is only safe once B3's PMP is in place |

### Items worth an explicit decision before starting

1. **B3 (U-mode + PMP + copy-in/copy-out)** — the single largest chunk in Phase 5, and the one that
   destabilizes currently-working, heavily-exercised code (the trap path). Worth deciding
   deliberately rather than drifting into. *Note the revised §5 no longer treats this as the point
   where "microkernel" stops being aspirational — B0–B2 deliver the microkernel structure, and B3
   adds hardware enforcement to it. That reordering is the main practical consequence of §5.0.*
2. **A3b (RX demux)** — moderate effort but the highest-risk item in Track A, because a regression
   there breaks the console on all three targets simultaneously. A3a exists specifically so this
   can be deferred without blocking a real wire. **Resolved (2026-08-07, see the A3b completion
   notes): shipped opt-in behind the `p9share` command rather than always-on**, so the risk this
   item calls out never applies to a session that doesn't explicitly arm it.
3. **`link_virtio_console`** — a whole new device driver whose only benefit is nicer CI ergonomics
   on QEMU. It does not advance the hardware story at all. Probably not worth it unless demux (A3b)
   proves genuinely painful.

---

## 9. Decisions needed before starting

These change the plan's content, not just its schedule.

### D1 — Transport strategy — **Resolved**
- **(a) SLIP-multiplexed single UART** — one code path for QEMU, RP2350 UART, and CP2102.
  Requires the A3b demux. *Recommended*, with A3a headless first to de-risk.
- **(b) virtio-console on QEMU + USB CDC on RP2350** — cleanest separation, no demux, but two
  unrelated drivers and nothing for a plain UART wire.
- **(c) Headless only** — trivial, but a node under test has no console, so failures are debugged
  blind.

*Recommendation: (a), sequenced as A3a → A3b, with (b) held in reserve.* Both (a) and (b) ended up
built, not just one: A3a → A3b landed as recommended (`p9serve` then `p9share`), and (b) also
landed in full (`link_virtio_console` on QEMU, `link_usb_cdc` on RP2350) rather than staying in
reserve — see the A3/A3b completion notes.

### D2 — NOMMU protection — **Resolved (2026-08-09): PMP early, NOMMU leads**
Leave RP2350 as a genuinely flat single address space, or invest in **PMP** region protection?

**Resolved: invest in PMP, and bring it up *before* Sv39.** RV32/RP2350 gets U-mode + PMP in
[B3](#b3--u-mode--pmp-on-the-nommu-targets--nommu-leads) (M9), and Sv39 follows in
[B5](#b5--sv39-the-mmu-backend) (M11) implementing the `mem_domain` interface B3 has already defined
and tested. Rationale for that ordering: PMP is the simpler enforcement model (flat regions, no page
tables, no faulting-in), so it is the cheaper place to get the *interface* right — and it means the
constrained target is the one with proven enforcement rather than the one perpetually waiting for
it. Makes the NOMMU/MMU story a difference of **granularity** rather than "protected vs. not",
consistent with [§5.0](#50--the-assumption-this-track-no-longer-makes).

*Prerequisite that is easy to miss*: `entry.S` programs `pmpaddr0`/`pmpcfg0` only under
`CONFIG_MODE_S`, and RV32/RP2350 build with `CONFIG_MODE_M` — so **PMP on RP2350 starts from zero**,
not from the wide-open configuration the pre-revision text implied. Hazard3's PMP region count must
be measured from silicon before the region budget can be planned.

### D5 — Track A regression policy under a scheduler — **Resolved (2026-08-09): hard gate**
A4's client/server safety argument explicitly depends on there being no scheduler
([§5.3](#53--hard-finding-the-scheduler-invalidates-track-as-correctness-argument)). **Resolved:
9P tag multiplexing lands inside B2 (M8), and B2 is not complete until
`test_9p_multinode_heterogeneous()` and `test_9p_remote_mount()` pass with the scheduler enabled.**
Track A does not get to regress silently to buy Track B progress.

### D6 — Dynamic server loading — **Resolved (2026-08-09) by capability, per target**
"Started dynamically" means different things on the two targets, and conflating them would overpromise.
**MMU**: ELF loaded at a fixed per-process virtual address (B6, closes B12). **NOMMU**: compiled into
the image, *started and bound* dynamically from `init.lisp`. Loading a third-party binary at runtime
is MMU-only unless a PIC toolchain is taken on — not currently planned. See
[§5.1](#corollary--dynamically-started-drivers-means-different-things-per-target).

### D3 — Track priority — **Resolved**
Track A to completion first (**recommended** — meets the stated goal soonest, leaves the riskiest
work off the critical path), or interleave Track B to make the microkernel claim real sooner?
Track A was taken to completion first, exactly as recommended (M1 through M5, all done
2026-08-07) — Track B has not been started.

### D4 — 9P server execution model — **Subsumed by B4**
Keep the server poll-driven (works today, no dependencies), or make it a task? The revised Track B
answers this: it becomes a task in [B4](#b4--servers-leave-the-main-call-stack), once B1's channels
and B2's scheduler exist. Note the related constraint is *earlier* and harder than this decision —
A2's fid table and A4's client/server role assumption both stop being safe at B2, not B4; see
[D5](#d5--track-a-regression-policy-under-a-scheduler--resolved-2026-08-09-hard-gate).

---

## Appendix — Review findings this phase closes

| Finding | Description | Closed by |
|---|---|---|
| **B11** | Unbounded 9P serialize/deserialize | A2 (**gates A3**) |
| **B12** | ELF loader trusts `e_phoff`/`e_phnum`/`p_offset`; `code_size` underflow | **B6** — ELF-loaded servers are MMU-only (see D6), so this closes with them |
| **A2** | Namespace fall-through across volumes | A5 |
| **A3** | No file handles in the VFS | A1 |
| **V4** | "Scales to 64-bit with MMU protection" — no MMU | **B5** (Sv39); **B3** delivers enforcement on NOMMU first |
| **V5** | `/proc` printk's instead of filling buffers; 9P not connected to VFS; `uart_net.c` never touches a UART | A1, A2, A3 |
| **V1–V3** | Microkernel / IPC / scheduler are stubs | **B1** (IPC), **B2** (scheduler), **B4** (servers) — enforcement then hardens in B3/B5 |
