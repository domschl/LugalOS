# LugalOS Review & Remediation Plan

> **⚠️ COMPLETED — archived for reference.** Phases 0–4 and the cross-cutting quick wins are all
> done and committed. This document is kept because its **finding IDs (B1–B13, A1–A6, V1–V6,
> D1–D4) are referenced from source comments throughout the tree** and from its successor.
>
> **Phase 5 has moved to
> [`plan/phase5_distributed_design.md`](../phase5_distributed_design.md)**, which supersedes the
> Phase 5 section below with a considerably more detailed design. Three findings recorded here are
> still open and are scheduled there: **B11** (unbounded 9P serialize/deserialize — now a hard
> security gate), **B12** (ELF loader input validation), and **A2** (namespace fall-through).

> **Date**: 2026-08-07 (review) — updated 2026-08-07 (Phase 0, 1, 2, 3, and 4 complete)
> **Scope**: Vision/implementation consistency, architecture, bug hunt, test coverage, documentation.
> **Baseline**: commit `b133833`, RV64 (Sv39 build) and RV32 (NOMMU build) verified live under QEMU on macOS.
>
> Findings marked **[VERIFIED]** were reproduced against a running system. Findings marked
> **[CODE]** are high-confidence static reads that have not (yet) been triggered at runtime.

## Progress

| Phase | Status |
|---|---|
| **Phase 0 — Make the tests able to fail** | ✅ **Complete** (see [§7 Phase 0](#phase-0--make-the-tests-able-to-fail) and [§9](#9-phase-0-completion-notes)) |
| **Phase 1 — Stop the crashes and corruption** | ✅ **Complete** (see [§7 Phase 1](#phase-1--stop-the-crashes-and-corruption) and [§10](#10-phase-1-completion-notes)) |
| **Phase 2 — Make the Lisp engine correct** | ✅ **Complete** (see [§7 Phase 2](#phase-2--make-the-lisp-engine-correct) and [§11](#11-phase-2-completion-notes)) |
| **Phase 3 — Filesystem integrity** | ✅ **Complete** (see [§7 Phase 3](#phase-3--filesystem-integrity) and [§12](#12-phase-3-completion-notes)) |
| **Phase 4 — Align the docs with reality** | ✅ **Complete** (see [§7 Phase 4](#phase-4--align-the-docs-with-reality) and [§13](#13-phase-4-completion-notes)) |
| Phase 5 — Close the vision gaps | ➡️ **Superseded** by [`plan/phase5_distributed_design.md`](../phase5_distributed_design.md) |

---

## Table of Contents

1. [Executive summary](#1-executive-summary)
2. [Vision vs. implementation](#2-vision-vs-implementation)
3. [Architecture](#3-architecture)
4. [Bugs](#4-bugs)
5. [Test coverage](#5-test-coverage)
6. [Documentation and help](#6-documentation-and-help)
7. [Remediation plan](#7-remediation-plan)
8. [Reproduction notes](#8-reproduction-notes)

---

## 1. Executive summary

The **"bare metal, minimal dependencies"** pillar is real and well executed. The **"microkernel"**,
**"MMU protection"**, and **"distributed Plan 9"** pillars are currently aspirational: the IPC,
scheduler, MMU, and 9P transport layers are placeholders that log convincingly but perform no work.

The single most urgent problem is that **the test suite cannot fail**. It reports 51/51 PASSED in
0.65 s while six reproducible defects — including a kernel panic reachable from a two-character
shell typo — are live in the tree. Tests match against the terminal *echo* of the command they
just typed. Until this is fixed, no other remediation can be verified.

**Priority order**: Phase 0 (tests can fail) → Phase 1 (crashes/corruption) → Phase 2 (Lisp
correctness) → Phase 3 (filesystem integrity) → Phase 4 (docs) → Phase 5 (close vision gaps).

---

## 2. Vision vs. implementation

### 2.1 What genuinely delivers

- Freestanding C11, `-nostdlib -ffreestanding`, own `libc/string.c`, own UF2 packager, own USB
  device stack. No Pico SDK / TinyUSB runtime linked.
- One `riscv64-elf-gcc` toolchain builds RV32, RV64, and RP2350. All three targets boot.
- `drivers/usb_cdc.c` is substantial, hardware-accurate work with genuinely useful comments about
  SIE quirks and DTR gating.
- FAT32, chibicc, `ed`, the line editor, and the Emacs-style box editor exist and work in QEMU.
- Compiler hygiene is good: `-Wall -Wextra -pedantic` with only 6 warnings tree-wide.
- UBSan is wired up and enabled by default on QEMU targets — a real asset (see [B-UBSAN](#b-ubsan)).

### 2.2 Claims the code does not currently support

#### V1 — "Microkernel"

There is no kernel/user separation. `user/lisp`, `user/chibicc`, `user/ed` are directory names,
not protection domains; everything links into one binary running at one privilege level.

#### V2 — "L4/seL4-style zero-copy register IPC rendezvous"

`kernel/ipc.c:9-19` is the entire implementation:

```c
msg_out->tag = msg_in->tag + 0x100; // Simulated response tag
msg_out->data[0] = msg_in->data[0] * 2;
```

No rendezvous, no blocking, no target task lookup. `sys_ipc_reply/send/recv` only `printk`.

#### V3 — Scheduler

- `kernel/sched.c:21` `task_create` discards the `entry` function pointer after printing it.
- `kernel/sched.c:38` `sched_yield` increments an index and logs a fake context switch.
- Neither is called anywhere in the tree (`grep` confirms zero call sites).
- `fs/vfs_server.c:214` `/proc/ps` prints a hardcoded four-process table describing processes
  that do not exist.

#### V4 — "Scales from 32-bit NOMMU to 64-bit with MMU protection"

The MMU half is absent.

- `arch/riscv/rv64_mmu/vmm.c:43` — `vmm_map_page` is `return 0;` with a `/* stub */` comment.
- `vmm_switch_space` is never called; `satp` is never written.
- The root page table is allocated and zeroed, then never populated.
- `kernel/main.c:72` prints `[Mode] Memory: Sv39 MMU Virtual Memory Paging Enabled` while paging
  is off.

There is no memory protection on either target. The 32→64-bit scaling axis has no second half yet.

#### V5 — "Plan 9 paradigms for truly distributed systems"

- `fs/9p.c` includes `fs/vfs.h` but never calls `vfs_read`/`vfs_write`.
- No fid table. `Twalk`/`Topen` ignore the path and return a fixed qid. Read/write hit one global
  2 KB buffer (`g_p9_storage_buf`).
- `drivers/uart_net.c:66-83` — the "Phase 2 SLIP UART transport" — **never touches the UART**:

  ```c
  int slip_len = slip_encode(req_buf, req_len, slip_tx, sizeof(slip_tx)); // slip_tx then discarded
  int resp_len = p9_server_process(req_buf, req_len, resp_buf, resp_max); // local call
  ```

  It is `loopback_net.c` with extra memcpys. The test suite reports it as PASSING.

**Structural blocker**: `/proc` and `/srv` reads `printk` to the console instead of filling the
caller's buffer (`fs/vfs_server.c:214-256` — note `sbuf[0] = '\0'; return len;`). No remote 9P
client could ever read them. The VFS also has no file handles or offsets — whole-file read/write
only, capped at static 4 KB buffers.

#### V6 — "Minimal bloat and overhead"

Measured RV64: `text 778588  data 118657  bss 1789952`.

| Item | Size | Note |
|---|---:|---|
| `node_pool` (lisp.c) | 557 KB | 4096 × 136 B |
| `node_pool` (chibicc parse.c) | 311 KB | |
| `ramdisk_storage` | 512 KB | |
| **Two Lisp/chibicc pools combined** | **868 KB** | **48% of BSS** |

Root cause of cell size: `lisp_val_t`'s union is sized by `char str[128]`, so every cons cell costs
136 bytes instead of ~16.

No `-O` flag is set anywhere → everything builds at `-O0`. Measured `-Os`: **text −17%,
data −48%**. `CMakeLists.txt:44` does `set(CMAKE_C_FLAGS "${COMMON_CFLAGS} ...")`, which clobbers
command-line flags; `CMAKE_BUILD_TYPE` is never consulted.

---

## 3. Architecture

The layering (`arch` / `drivers` / `fs` / `kernel` / `user`) is clean and the `block_dev_t`
abstraction is right. Main conceptual weaknesses:

### A1 — Pool-exhaustion-by-wraparound is systemic

`user/lisp/lisp.c:55` prints "Resetting heap" and sets `node_pool_idx = 0` while old nodes are
still referenced by `global_env`, live ASTs, and closures. The same pattern appears in six chibicc
pools: `parse.c:53,63,198,209,228` and `tokenize.c:52`. This converts resource exhaustion into
silent memory aliasing rather than a clean error.

### A2 — Namespace fall-through breaks Plan 9 semantics

`parse_prefix` (`fs/vfs_server.c:167-172`) defaults *any* unrecognized path to `/flash0/`, and
`vfs_read` for `/sd0/` falls back to flash then ram (`:195-205`). `cat /sd0/foo` can silently
return `/ram0/foo`. Plan 9 namespaces are supposed to be explicit.

### A3 — No file handles

VFS is whole-file read/write only. 9P `Tread`/`Twrite` offsets are parsed then ignored. Files
larger than the static 4 KB buffers cannot be handled at all.

### A4 — Unbounded C recursion in `lisp_eval`

Recurses on the C stack with no depth limit and no guard page on NOMMU.

### A5 — USB console stalls outside the main input loop

`usb_cdc_task()` is pumped from `kernel/line_editor.c:478` and the uart drivers, but **not** from
`edit_multiline_box`, `read_status_prompt`, or `lisp_repl` — so the USB console stalls in the
editor and REPL, contradicting the README's "same interactive session" claim.

### A6 — Build system cannot be tuned <a name="a6"></a>

`CMAKE_C_FLAGS` is overwritten unconditionally; no `CMAKE_BUILD_TYPE`; no `-O`.

### B-UBSAN — UBSan detects but does not halt <a name="b-ubsan"></a>

`-fsanitize=undefined -fno-sanitize-recover=all` is set (`CMakeLists.txt:69`), but every handler in
`kernel/ubsan.c` prints and returns. Corruption proceeds. Observed 13+ consecutive out-of-bounds
writes continuing unimpeded.

---

## 4. Bugs

### 4.1 Verified live in QEMU

#### B1 — [VERIFIED] Kernel panic from a shell typo

`(= 1)` halts the system permanently:

```
[UBSan Fault] NULL pointer dereference! [.../user/lisp/lisp.c:155:35]
[Trap Exception] Cause: 0x5, epc=0x8001a3e8, tval=0x0
[Fatal] System halted due to unhandled exception.
```

**Root cause**: primitives check `!args->u.pair.cdr`, but the list terminator is `&nil_val`
(non-NULL) whose `u.pair.car` is NULL. Same pattern in `prim_poke`, `prim_write`, `prim_cp`,
`prim_cc`, `prim_write_file`, `prim_eeprom_write`, and the `mkdir` family.

#### B2 — [VERIFIED] Stack buffer overflow in the shell parser

Reachable from any console input (UART or USB). `kernel/shell.c:228-243` bounds-checks *content*
writes with `if (sidx < 500)` but leaves the delimiter writes unguarded:

```c
sexpr[sidx++] = ' ';   // unguarded
sexpr[sidx++] = '"';   // unguarded
sexpr[sidx++] = ')';   // unguarded
```

`ls a a a …` (160 tokens) produced OOB indices 512 → 524+ past `char sexpr[512]`. Without UBSan
this is a silent stack smash.

#### B3 — [VERIFIED] Recursion is broken

```
lsh> (define f (lambda (n) (if (= n 0) 1 (* n (f (- n 1))))))
=> f
lsh> (f 5)
Unbound symbol: f
=> 5
```

The lambda captures `global_env` **by value** at creation, before its own name is bound
(`user/lisp/lisp.c:917-925`).

#### B4 — [VERIFIED] `(define (fn args) body)` is not implemented

`user/lisp/lisp.c:909` takes `args->u.pair.car` as the symbol; for a signature list that is a
`LISP_PAIR`, and `sym->u.sym` reads two pointers as a char array.

```
lsh> (define (factorial n) (if (= n 0) 1 (* n (factorial (- n 1)))))
Unbound symbol: n ... Unbound symbol: factorial
lsh> (factorial 6)
Unbound symbol: factorial
```

The README shows `=> 720` for this exact example.

#### B5 — [VERIFIED] String equality always false

`prim_eq` only compares ints. `(= (arch) "rv64")` → `#f`.

**Consequence**: `init.lisp`'s `(if (= (arch) "rv32") (mount-ramdisk 64) (mount-ramdisk 512))`
always takes the else branch. On RP2350 that sets `num_blocks = 1024` over
`RAMDISK_NUM_BLOCKS = 128` of real storage (`fs/vfs_server.c:88` mutates it unvalidated), so
`fat32_format` records a 512 KB volume backed by 64 KB — writes above block 128 fail silently.

#### B6 — [VERIFIED] Node pool wrap corrupts live state

A `begin` block of ~120 calls produced `Unbound symbol: n` mid-evaluation and a wrong result.
Silent wrong answers, no crash. (See [A1](#a1--pool-exhaustion-by-wraparound-is-systemic).)

### 4.2 High-confidence, code-level

#### B7 — [CODE] `fat32_read_file` off-by-one

`fs/fat32.c:270` writes `((char *)buf)[bytes_read] = '\0'` unconditionally after clamping to
`max_size`. Callers passing `sizeof(buf)` overflow by one byte —
`arch/riscv/common/elf.c:17` passes `sizeof(file_buf)` for `static uint8_t file_buf[4096]`.

#### B8 — [CODE] FAT32 cluster leaks on every write and delete

- `fat32_write_file` finds the existing entry, allocates an entirely new chain, and overwrites
  `fst_clus` **without freeing the old chain**.
- `fat32_remove_file` marks the entry `0xE5` and **never frees the chain**.
- Compounding: `kernel/line_editor.c:59-69` rewrites the whole history file **on every command**,
  so a 512 KB volume leaks steadily during normal shell use.

#### B9 — [CODE] Directory ops only read the first sector of each cluster

`fat32_get_parent_cluster`, `fat32_find_file`, `fat32_write_file`, `fat32_list_dir`,
`fat32_remove_file`, and `fat32_rmdir` all read 1 sector (16 entries) per cluster, then follow the
FAT chain. Correct only when `sec_per_clus == 1` (what `fat32_format` produces). Any PC-formatted
SD card uses 8–64, so entries past the first 16 per cluster become invisible.

Related in the same file:
- `fs/fat32.c:49` — `fat_set_entry` hardcodes FAT2 at `fat_sector + 8` instead of using `fat_sz32`.
- `fs/fat32.c:56` — `fat_alloc_cluster` iterates `tot_sec32` as if it were a cluster count, and
  starts at `c = 3` (should be 2).

#### B10 — [CODE] `fat32_init` auto-formats on unrecognized boot sector

`fs/fat32.c:190`. Inserting a non-FAT32 card silently destroys it.

#### B11 — [CODE] 9P serializer/deserializer have no bounds checking

- `p9_serialize` validates only `buf_size < 7`, then writes all payloads including
  `memcpy(p, msg->data, msg->count)` with a caller-controlled count.
- `p9_deserialize` checks `size > len` but has no end pointer — each `read_u32` walks forward
  unchecked, and sets `msg->data = p` with an unvalidated `count`.

Not reachable from today's local-only paths, but this is exactly the code the distributed roadmap
intends to feed from a wire.

#### B12 — [CODE] ELF loader trusts the file

`arch/riscv/common/elf.c`:
- `:42,:60` — computes `phdr = file_buf + ehdr->e_phoff` with no validation of `e_phoff` /
  `e_phnum` against the 4096-byte buffer.
- `:76` — `code_size = bytes - code_offset` can underflow (unsigned).
- `:82` — segments > 4096 are silently truncated and then executed.
- `:94` — `e_entry` is read, printed, then ignored; execution always starts at offset 0.

#### B13 — [CODE] Minor

- `fs/vfs_server.c:260-264` — `/dev/uart` read writes `sbuf[1]` guarding only `max_len > 0`
  (1-byte overflow when `max_len == 1`).
- `vfs_cp` silently truncates at 4096 with no error.
- `strncpy` without explicit termination: `fs/fat32.c:80`, `kernel/line_editor.c:523,565`.
- Alignment: `uint8_t[512]` cast to `uint32_t *` / `fat32_dir_entry_t *` throughout `fs/fat32.c`.

---

## 5. Test coverage

The suite reports **51/51 PASSED in 0.65 s** (README claims 37 cases). Two full QEMU boots plus 50
interactive sequences cannot complete in 0.65 s — and the suite passes while B1–B6 are all live.

### 5.1 Core defect: tests match the echo of the command they typed

Proved directly:

```
bogus echo-only test PASSES: True     # asserted "THIS_STRING_IS_NEVER_OUTPUT_BY_LUGALOS"
real recursion test passes: False     # asserted "=> 111222"
```

A test asserting a string LugalOS can never emit passes. Affected by construction:

| Test | Expected pattern | Why it is a tautology |
|---|---|---|
| VFS mkdir/write/cp/cat | `Hello_LugalOS_VFS` | present in the typed `write` command |
| Lisp Special Forms | `cond_ok` | present in the typed `cond` expression |
| Lisp Arithmetic | `42` | present in the typed `(= 42 42)` |
| Persistent Command History | `history` | present in the typed path |
| Namespace Directory Listing | `lisp` | present in the boot banner |

### 5.2 Secondary issues

- `output_queue` is never drained between calls → each test can satisfy itself from the previous
  test's leftovers.
- Nothing checks for `[UBSan Fault]`, `[Trap Exception]`, or `[Fatal]`.
- The test labelled "System Process & Memory Monitor (top)" runs `cat /proc/ps` and never invokes
  `top`.
- No RP2350 hardware test.
- No multi-node test, despite `scripts/run-qemu-multinode.sh` existing — and that script
  cross-connects two *consoles*, not a 9P channel.

---

## 6. Documentation and help

For this stage the README is broad and readable; the hardware wiring tables and the third-party
attribution section are genuinely good. Problems are accuracy and discoverability.

### D1 — Documents non-working features as working

The factorial example with `=> 720`; `(define (fn args) body...)`; "MMU protection";
"L4/seL4-style IPC"; "37 test cases" (actual 51); Phase 2 UART transport.

### D2 — `cmd_help` omits most of what exists

`kernel/shell.c:19` does not mention: `peek`, `poke`, `i2c-scan`, `eeprom-read`, `eeprom-write`,
`p9-loopback`, `p9-uart-send`, `usb-status`, `mount-ramdisk`, `arch`, `compile-file`, `load`,
`display`, `newline`, `read-file`, `write-file`, `touch`, `write`.

### D3 — No `(help)` primitive in the Lisp REPL

The banner mentions only `(+ 10 20)` and `exit`. Since the Lisp engine **is** the shell, this is
the main discoverability gap.

### D4 — README directory tree

Lists `rv32_nommu` but omits `rv64_mmu` and `rp2350`.

---

## 7. Remediation plan

### Phase 0 — Make the tests able to fail ✅ COMPLETE

> Do this first. Everything else depends on being able to verify it.

- [x] **0.1** Drain `output_queue` at the start of each `send_and_expect` (`tests/runner.py:66`).
- [x] **0.2** Strip the echoed command from captured text before matching, or match only on lines
      after the echo.
- [x] **0.3** Fail any test whose window contains `[UBSan Fault]`, `[Trap Exception]`, or
      `[Fatal]`.
- [x] **0.4** Make `kernel/ubsan.c` halt (honouring `-fno-sanitize-recover`), gated by a
      `UBSAN_PANIC` CMake option so the runner detects faults deterministically.
- [x] **0.5** Re-run the suite and record the real baseline.

**Result: the permanent suite still reports 51/51, and this is now a genuine result — not a
tautology.** See [§9](#9-phase-0-completion-notes) for what changed, how it was independently
verified, and why 51/51 is expected at this stage (the existing 51 cases don't happen to exercise
the specific broken paths — recursive lambdas, `(define (fn args) ...)` signature form, wrong-arity
primitive calls — found via manual probing in the original review; those become Phase 1 work, and
Phase 1.5 adds permanent regression tests for them).

### Phase 1 — Stop the crashes and corruption ✅ COMPLETE

- [x] **1.1** Add `lisp_list_ref(args, n)` / `lisp_list_len(args)` helpers in `user/lisp/lisp.c`
      (exported via `lisp.h`); convert every primitive with the crash pattern to arity + type
      check through them. *(fixes B1 across 7 sites in lisp.c, plus `prim_compile_file` in
      lisp_compile.c — 8 total)*
- [x] **1.2** Rewrite the `kernel/shell.c` S-expression transformer with a single bounds-checked
      `sb_putc()`/`sexpr_buf_t` helper; reject over-long lines with a message. *(B2)*
- [x] **1.3** Replace pool wraparound with a hard-stop-and-flag in `lisp.c` and all 8 chibicc
      wraparound sites (type ×4, node, obj ×2, member, function, token, plus an unrelated
      string-literal buffer overflow found in the same file) — clamp to the last slot instead of
      aliasing, and refuse to emit/persist a binary once exhaustion is flagged. *(B6, A1)*
- [x] **1.4** Fix `fat32_read_file`'s off-by-one: the function itself now reserves the last byte
      of whatever `max_size` it's given for the NUL terminator, so it's safe regardless of what a
      caller passes. Normalized the two callers (`elf.c`, `vfs_cp`) that weren't already reserving
      a byte, for clarity. *(B7)*
- [x] **1.5** Added 2 new permanent regression tests to `tests/runner.py` (55 total, up from 53)
      covering B1 and B2 directly. B6 and B7 were verified by code-level bounds proof and full
      build/suite verification rather than a dedicated regression test — see
      [§10.3](#103-what-does-not-have-a-dedicated-regression-test-and-why) for why.

**Result: 55/55 passing on both RV64 and RV32 (up from 53/53), RP2350 still builds clean, no new
compiler warnings.** See [§10](#10-phase-1-completion-notes) for the full list of files touched,
the design reasoning for each fix class, and what was deliberately left out of scope.

### Phase 2 — Make the Lisp engine correct ✅ COMPLETE

- [x] **2.1** Implement `(define (fn args...) body...)` desugaring to `lambda`. *(B4)*
- [x] **2.2** Fix recursion: closures created directly in the global scope now resolve against the
      *live* `global_env` at call time (a `NULL` sentinel in `lambda.env`) instead of a frozen
      snapshot captured before their own binding existed. *(B3)*
- [x] **2.3** Make `=` compare strings and symbols (in addition to ints). *(B5)*
- [x] **2.4** Added `ramdisk_max_blocks()` and clamp `vfs_mount_ramdisk()` against it; verified
      `init.lisp`'s RAMDisk branch now takes the correct arch-specific path now that `=` compares
      strings correctly. *(B5 consequence)*
- [x] **2.5** Lambda/function bodies are now a list of forms evaluated in sequence (like `begin`),
      not just the first expression. *(`user/lisp/lisp.c`)*
- [x] **2.6** Moved `LISP_STRING`/`LISP_SYMBOL` text out of the node union into a separate interned
      string pool; `lisp_val_t` shrank from 136 B to 32 B. Sized the string pool at
      `NODE_POOL_SIZE / 2` rather than 1:1 with node_pool — see
      [§11.2](#112-26-the-actual-memory-savings-achieved) for why, and for the real (more modest
      than "most of 868 KB") number achieved. *(V6)*
- [x] **2.7** Added a `(help)` Lisp primitive that walks `global_env` directly (so it can't drift
      out of sync the way a hand-maintained list would) and a pointer to it from the shell's
      `help` command. *(D2, D3)*
- [x] **2.8** Added an evaluation-depth guard: `lisp_eval()` is now a thin wrapper around the
      renamed `lisp_eval_step()` that counts nesting depth and aborts cleanly past
      `LISP_MAX_EVAL_DEPTH` (100, a conservative unprofiled default) instead of overflowing the C
      stack. *(A4)*

**Result: 69/69 passing on both RV64 and RV32 (up from 55/55 — 8 new Phase 2 regression tests,
covering B3, B4, B5, the multi-body fix, the depth guard, and `(help)`), all three targets build
clean with no new warnings.** While adding the `(help)` regression test, found and fixed an
additional latent bug in the Phase 0 test-harness echo-stripping itself (not in LugalOS) — see
[§11.3](#113-a-test-harness-bug-found-while-testing-27). Full file list, design reasoning, and the
memory-savings numbers are in [§11](#11-phase-2-completion-notes).

### Phase 3 — Filesystem integrity ✅ COMPLETE

- [x] **3.1** Added `fat32_free_chain()`; `fat32_write_file()` now calls it on the *old* chain
      before allocating a new one when overwriting an existing file, and `fat32_remove_file()`
      calls it before marking an entry deleted, instead of orphaning the file's clusters in both
      cases. *(B8)*
- [x] **3.2** Every directory scan now goes through one shared `fat32_scan_dir()` helper that
      iterates all `sec_per_clus` sectors of every cluster in the chain — replacing 7 hand-rolled
      loops, two of which (`fat32_write_file`'s and `fat32_mkdir`'s free-slot searches) turned out
      not to even walk the FAT chain to a second *cluster*, let alone a second sector; see
      [§12.2](#122-32--worse-than-described-in-two-spots). *(B9)*
- [x] **3.3** `fat_set_entry()` now writes the second FAT copy at `fat_sz32` sectors after the
      first instead of a hardcoded `+8`; `fat_alloc_cluster()` now computes the real cluster count
      from the data-region size instead of scanning cluster numbers up to `tot_sec32` (a *sector*
      count) — the old version could allocate a cluster number that mapped past the end of the
      volume, corrupting whatever storage happened to be there. Found and fixed a related gap
      while chasing this down: `spisd_read_blocks`/`write_blocks` (RP2350's physical SD driver) had
      no LBA bound-check at all, unlike the other three block-device drivers; see
      [§12.3](#123-a-bound-check-gap-found-while-fixing-33). *(B9)*
- [x] **3.4** Removed `fat32_init()`'s auto-format-on-invalid-boot-sector branch; added
      `vfs_format()`/`fat32_format()` reachable via a new `(format "<path>")` Lisp primitive (and
      thus from the shell, same as any other primitive) as the only remaining way to format `/sd0/`
      or `/ram0/` — `/ram0/`'s own always-blank-at-boot auto-format fallback in
      `vfs_mount_ramdisk()` is untouched and still works, since it never relied on `fat32_init()`'s
      internal behavior; see [§12.4](#124-34-why-removing-auto-format-was-safe-for-the-test-suite).
      *(B10)*
- [x] **3.5** Added `fat32_append_file()` (extends an existing file's cluster chain, writing only
      the new tail data) and a `vfs_append()` wrapper; `add_history()` now appends just the new
      line instead of reconstructing and rewriting the whole accumulated history buffer on every
      command. *(B8)*
- [x] **3.6** Added a regression test that overwrites the same file 20 times and asserts reported
      free space is unchanged (not a several-hundred-file stress test as originally envisioned —
      see [§12.5](#125-36-narrower-than-planned-and-why-thats-enough) for why the narrower version
      is sufficient), plus a second test exercising find/write/read across multiple files in a
      subdirectory, and a third exercising the new `format` primitive end-to-end.

**Result: 75/75 passing on both RV64 and RV32 (up from 69/69 — 3 new Phase 3 regression tests),
all three targets build clean with no new warnings.** This was the largest phase by far — nearly
every function in `fs/fat32.c` changed. Full file list, the shared-scan-helper design, and three
things found during the work that weren't in the original review are in
[§12](#12-phase-3-completion-notes).

### Phase 4 — Align the docs with reality ✅ COMPLETE

- [x] **4.1** Added a new "Implementation Status" section right after the intro (before the
      enthusiastic feature list gets a chance to mislead), explicitly separating what's
      tested-and-working from what's a stub/name-only; trimmed the IPC bullet and the `(ps)`
      primitive description to match and point to it. *(V1–V5)*
- [x] **4.2** Verified live in QEMU rather than assumed: the exact README factorial example —
      `(define (factorial n) (if (= n 0) 1 (* n (factorial (- n 1)))))` then `(factorial 6)` — now
      returns `=> 720`, because Phase 2 already fixed the two bugs (B3, B4) it was silently
      demonstrating. No doc change was needed once the code was actually correct; see
      [§13.1](#131-42-the-factorial-example-was-already-fixed-by-phase-2). *(D1)*
- [x] **4.3** Updated "37 automated test cases" to the current 75, with a hedge
      ("see `tests/runner.py` for the current count") against the same number drifting stale again
      as more tests get added in later phases. *(D1)*
- [x] **4.4** The two existing `/dev/ttyACM1` mentions already said "planned" / "not wired up yet"
      accurately; added an explicit callout for `uart_net.c`'s SLIP transport specifically (it
      SLIP-encodes a request and then calls the local 9P server directly instead of sending
      anything over a UART) to the new Implementation Status section, since that gap wasn't
      mentioned anywhere before. *(V5)*
- [x] **4.5** Added `rv64_mmu/` and `rp2350/` to the README directory tree, each with a one-line
      description (`rv64_mmu/` explicitly flagged as not-yet-wired-up, consistent with the new
      Implementation Status section). *(D4)*

**Result: README now has one clear, prominent, honest status section instead of the review's
findings being scattered implicitly across enthusiastic bullets.** No functional/build changes in
this phase — README.md was the only file touched. See [§13](#13-phase-4-completion-notes).

### Phase 5 — Close the vision gaps (dependency order)

- [ ] **5.1** Give the VFS a real file-handle API (`open` / `read-at` / `write-at` / `close`), and
      make `/proc` fill the caller's buffer instead of `printk`-ing. **Everything distributed
      depends on this.** *(A3, V5)*
- [ ] **5.2** Rewrite `fs/9p.c` against that API: real fid table, path-resolving `Twalk`, full
      bounds checking on both serialize and deserialize. *(B11, V5)*
- [ ] **5.3** Make `drivers/uart_net.c` actually transmit and receive over the UART. *(V5)*
- [ ] **5.4** Harden the ELF loader before any remote code path can reach it. *(B12)*
- [ ] **5.5** Either implement the MMU (`vmm_map_page`, S/U-mode split, per-task `satp`) **or**
      restate the vision as a single-address-space system. The banner claiming Sv39 is active is
      the misleading part either way. *(V4)*

### Cross-cutting quick wins ✅ COMPLETE

- [x] **X.1** `CMakeLists.txt` now defaults `CMAKE_BUILD_TYPE` to `MinSizeRel` when the caller
      doesn't set one (verified: `-Os`, text −17%/data −49% on this build, matching the review's
      original measurement), respects an explicit override (verified with
      `-DCMAKE_BUILD_TYPE=Debug`), and appends to `CMAKE_C_FLAGS`/`CMAKE_ASM_FLAGS` instead of
      overwriting them (verified a caller-supplied `-DCMAKE_C_FLAGS=...` survives into the actual
      compile commands). *(A6, V6)*
- [x] **X.2** `usb_cdc_task()` is now pumped from `edit_multiline_box`'s and `read_status_prompt`'s
      main input loops (`kernel/line_editor.c`) and `lisp_repl`'s (`user/lisp/lisp.c`) — all three
      previously blocked on `uart_getc()` with no USB polling, stalling the USB console while
      inside the editor, a status-line prompt, or the Lisp REPL. *(A5)*
- [x] **X.3** `/dev/uart`'s 1-byte overflow now checks `max_len > 1` before writing the second byte
      (`fs/vfs_server.c`). Added a `safe_strncpy()` helper (mirroring the existing
      `strncpy_local()` in `user/lisp/lisp.c`) to both `kernel/line_editor.c` and `fs/fat32.c`, and
      converted every `strncpy` call site in both files to it — 8 of the 13 sites in
      `line_editor.c` and 1 of the 2 in `fat32.c` were genuinely missing termination (reachable via
      e.g. a 511-character line or an exactly-63-character path component), not just theoretically
      risky; see [§14.1](#141-x3-which-strncpy-sites-were-actually-broken). *(B13)*
- [x] **X.4** Decided on the union approach: added a `fat32_sector_t` union
      (`raw`/`words`/`entries`/`bpb` views over the same 512 bytes) and converted every sector
      buffer in `fs/fat32.c` that was cast to a stricter-aligned type to use it, eliminating the
      casts entirely rather than working around them. Buffers only ever touched via `memcpy()`
      (already alignment-safe) were deliberately left as plain `uint8_t[512]`; see
      [§14.2](#142-x4-why-union-and-what-was-left-alone). *(B13)*

**Result: all three targets rebuild clean with no new warnings; full suite is still 75/75 on both
RV64 and RV32** (no new tests added — these were hardening fixes, not new user-visible behavior to
assert against, beyond what the existing suite already exercises). See
[§14](#14-cross-cutting-quick-wins-completion-notes) for verification detail on each item.

---

## 8. Reproduction notes

Environment: macOS (Darwin 25.6.0), `riscv64-elf-gcc` + `qemu-system-riscv64` via Homebrew.

Build:

```bash
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake
ninja -C build/rv64
```

The findings above were reproduced by driving QEMU over a pipe and sending shell lines directly:

```bash
qemu-system-riscv64 -M virt -nographic -bios none \
    -drive file=<copy-of-build/lugalos_sd.img>,if=none,format=raw,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -kernel build/rv64/lugalos.elf
```

Minimal reproducers:

| Finding | Input |
|---|---|
| B1 | `(= 1)` |
| B2 | `ls ` followed by ~160 space-separated single-char tokens |
| B3 | `(define f (lambda (n) (if (= n 0) 1 (* n (f (- n 1))))))` then `(f 5)` |
| B4 | `(define (factorial n) (if (= n 0) 1 (* n (factorial (- n 1)))))` then `(factorial 6)` |
| B5 | `(= (arch) "rv64")` |
| B6 | `(define g (lambda (n) (* n 2)))` then a `begin` block of ~120 `(g N)` calls |

The echo-tautology proof (5.1) reuses `tests/runner.py`'s own `QemuSession` class, asserting a
string the OS can never emit and observing a PASS.

Size measurements used `riscv64-elf-size` and `riscv64-elf-nm --size-sort -S`. The `-Os` comparison
was taken by temporarily appending `-Os` to `COMMON_CFLAGS` in `CMakeLists.txt`; the working tree
was restored afterwards.

---

## 9. Phase 0 completion notes

### 9.1 What changed

**`CMakeLists.txt`** — added a `UBSAN_PANIC` option (default `ON` whenever `ENABLE_SANITIZERS` is
`ON`) that defines `CONFIG_UBSAN_PANIC=1`.

**`kernel/ubsan.c`** — each `__ubsan_handle_*` base handler (not the `_abort` forwarding wrappers,
which call the base handlers) now ends with `UBSAN_MAYBE_HALT()`, which — when
`CONFIG_UBSAN_PANIC` is set — prints `[UBSan Fatal] Halting system due to undefined behavior.` and
loops on `wfi` forever. This closes the gap noted in [B-UBSAN](#b-ubsan): with
`-fno-sanitize-recover=all`, the compiler treats these handlers as effectively terminal, but they
were previously logging and returning, letting corruption keep executing. They now stop
deterministically, which is what makes fault detection in the test runner meaningful instead of
racing a timeout against however far corruption manages to spread.

**`tests/runner.py`** (`QemuSession`) — three independent fixes to `send_and_expect`:

1. **`_drain()`**, called before every `send_and_expect`, discards any output left over from the
   previous call so a new test can't be satisfied by a prior test's leftover output.
2. **`_strip_echo()`** removes the guest's echo of the just-sent command from the captured text
   before running the expected-pattern regex against it. The interactive line editor echoes every
   keystroke (with embedded `\n` between subcommands normalized to `\r\n` by the terminal), so
   without this an `expected_pattern` that is a substring of the *command itself* matches its own
   echo rather than any real response.
3. **`FAULT_MARKERS`** check — if `[UBSan Fault]`, `[UBSan Fatal]`, `[Trap Exception]`, or
   `[Fatal]` appears in the accumulated window, the test fails immediately instead of possibly
   still matching (or padding out to a plain, undiagnosed timeout).

### 9.2 Independent verification (ad hoc, outside the permanent suite)

Four probes were run against the *patched* harness before trusting it:

| Probe | Input | Expected | Result |
|---|---|---|---|
| Bogus assertion | `(+ 100 200)` / expects `NONEXISTENT_MARKER_QWERTY99887766` | FAIL | ✅ FAIL |
| Real evaluation | `(+ 100 200)` / expects `=> 300` | PASS | ✅ PASS |
| Echo-only marker | `; ECHO_ONLY_COMMENT_MARKER_555` (Lisp comment, silently consumed by the reader) / expects the same marker text | FAIL | ✅ FAIL |
| Known crash (B1) | `(= 1)` / expects `=>` | FAIL (halts on `[UBSan Fault]`) | ✅ FAIL |
| Known crash (B2) | `ls` + 160 space-separated tokens / expects `lsh>` | FAIL (halts on `[UBSan Fault]`) | ✅ FAIL |

One dead end worth recording: the first attempt at the "bogus assertion" probe used an
identifier-shaped marker (`THIS_STRING_IS_NEVER_OUTPUT_BY_LUGALOS`) passed as a bare symbol to
`(display ...)`. That *still* passed after the echo-stripping fix — not because the fix failed,
but because the marker was a syntactically valid Lisp symbol, and the evaluator's own
`Unbound symbol: <name>` diagnostic genuinely re-emits an unrecognized identifier's name as real
output. That's a correct match, not a tautology; it just meant the probe was a poor choice of
marker. The corrected probes above avoid this by using markers that are either non-identifier
assertions checked against real arithmetic output, or content a comment line consumes without ever
echoing outside the raw keystroke echo that `_strip_echo` already removes.

### 9.3 Why 51/51 after the fix is not suspicious

Two things that looked like red flags in the original review are now explained rather than fixed:

- **Total runtime (~0.79 s for two full QEMU boots and 50 command sequences).** This is plausible,
  not implausible: a minimal freestanding kernel under QEMU/TCG has no real hardware
  initialization latency to emulate, and ad hoc probing during this session consistently saw
  command output land within tens of milliseconds of being sent. The original review's suspicion
  about the timing was reasonable given the tautology, but timing was never itself the defect.
- **51/51 passing.** The permanent suite's 51 cases were written before the bugs in this review
  were found by manual probing, and — now that the harness can actually detect a failure — they
  still pass because none of them happen to exercise the broken paths: they use the value form of
  `define` (not `(define (fn args) ...)`, see B4), no primitive is called with the wrong arity
  (B1), and no lambda calls itself recursively by name (B3). This is expected, not a sign the fix
  is incomplete. Phase 1 fixes those code paths and Phase 1.5 adds permanent regression coverage
  for exactly these scenarios, so the suite will start actually exercising them.

### 9.4 Build verification

Both QEMU targets were rebuilt clean from scratch with the `UBSAN_PANIC` option active and
confirmed to still boot and pass the full suite:

```bash
rm -rf build/rv64 build/rv32
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake && ninja -C build/rv64
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake && ninja -C build/rv32
python3 tests/runner.py   # 51 / 51 Tests PASSED
```

No new compiler warnings were introduced.

---

## 10. Phase 1 completion notes

### 10.1 Files touched

| File | Change |
|---|---|
| `user/lisp/lisp.c` | Added `lisp_list_len`/`lisp_list_ref`/`arg_int` helpers; fixed `prim_eq`, `prim_peek`, `prim_poke`, `prim_write`, `prim_cp`, `prim_cc`, `prim_write_file`, `prim_sub` to route through them; node-pool exhaustion no longer wraps to index 0 |
| `user/lisp/include/lisp.h` | Exported `lisp_list_len`, `lisp_list_ref`, `get_str_val` (was `static`) so `lisp_compile.c` can reuse them |
| `user/lisp/lisp_compile.c` | Fixed `prim_compile_file` (same crash pattern as the lisp.c primitives) |
| `kernel/shell.c` | Added `sexpr_buf_t`/`sb_init`/`sb_putc`; rewrote the POSIX→S-expression transformer to route every write through it |
| `user/chibicc/include/chibicc.h` | Added `extern bool chibicc_pool_exhausted;` |
| `user/chibicc/tokenize.c` | Defined/reset `chibicc_pool_exhausted`; fixed token-pool wraparound; fixed an unbounded string-literal-buffer overflow found while auditing this file (see [10.2](#102-a-bug-found-during-the-fix-not-in-the-original-review)) |
| `user/chibicc/parse.c` | Fixed all 7 remaining pool-wraparound sites (type ×4 via a new shared `type_pool_exhausted()` helper, node, obj ×2, member, function) |
| `user/chibicc/main.c` | `chibicc_compile()` now checks `chibicc_pool_exhausted` after `parse()` and refuses to emit/write a binary if set |
| `fs/fat32.c` | `fat32_read_file` now reserves the last byte of `max_size` for its own NUL terminator, independent of caller convention |
| `arch/riscv/common/elf.c` | Normalized `vfs_read(..., sizeof(file_buf))` → `sizeof(file_buf) - 1` for clarity (no longer load-bearing after the fat32.c fix, but keeps the call site's intent explicit) |
| `fs/vfs_server.c` | Same normalization in `vfs_cp`'s `copy_buf` read |
| `tests/runner.py` | Added 2 permanent regression tests (B1, B2) |

### 10.2 A bug found during the fix, not in the original review

While auditing `tokenize.c` for pool-wraparound sites, `read_string_literal()` turned out to have
an unrelated, unbounded write: it copied a C string literal's contents into a fixed 128-byte
`str_bufs[]` slot with no bound on `len` at all — a `cc`-compiled source file containing a string
literal longer than 127 bytes would silently overrun into the next slot (or past the array on the
last one). This is the same severity class as B2 (stack/static buffer overflow from unbounded
attacker/user-controlled input) and was fixed the same way: cap the write, and flag
`chibicc_pool_exhausted` so a truncated literal can't silently produce a binary that looks like it
compiled cleanly.

### 10.3 What does not have a dedicated regression test, and why

**B6 (Lisp node-pool exhaustion)**: reliably exhausting the 4096-slot pool requires either many
sequential commands (cumulative usage across a session) or a single command line long enough to
allocate thousands of AST/environment nodes — but the interactive shell's own line buffer caps a
single command at 511 characters, and a back-of-envelope count (~7 node allocations per simple
function call in this evaluator) puts a 511-character line at only a few hundred allocations, far
short of 4096. Deterministically triggering exhaustion within the existing single-line test harness
isn't practical without either a loop/iteration construct in the Lisp language (which doesn't exist
yet — see Phase 2) or a test that sends dozens of separate commands in one session, which would be
slow and fragile. The fix itself was verified by code inspection (the new code path is a straight
line: detect, clamp, warn-once) and by confirming the existing Lisp test suite still passes
end-to-end with the changed allocator.

**B7 (fat32_read_file off-by-one) / the chibicc pool sites in `parse.c`**: exercising these requires
constructing input at or beyond a specific fixed-size boundary (a file ≥ 4095 bytes for B7; a
source file with 2048+ AST nodes, 512+ objects, 256+ distinct types, etc. for the chibicc pools).
The shell's 511-byte line cap makes this awkward to construct through the existing test harness
without adding dedicated large fixture files. These were verified by (a) a direct bounds proof —
`bytes_read <= size <= capacity = max_size - 1`, so the terminator write is always in-bounds
regardless of caller behavior — and (b) confirming the "chibicc C11 Compiler & Exec" test still
compiles and runs a real program correctly through the changed allocators, i.e. the fix doesn't
break the common case.

Both are reasonable follow-ups once Phase 2 gives the Lisp language a loop/iteration construct
(making B6 easy to trigger on demand) or a fixture-file mechanism is added to the test harness
(making B7 easy to trigger on demand) — noted here rather than silently skipped.

### 10.4 Build verification

All three targets rebuilt clean from scratch with Phase 1 changes applied:

```bash
rm -rf build/rv64 build/rv32 build/rp2350
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake && ninja -C build/rv64
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake && ninja -C build/rv32
cmake -B build/rp2350 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rp2350.cmake -DLUGALOS_TARGET=RP2350 && ninja -C build/rp2350
python3 tests/runner.py   # 55 / 55 Tests PASSED
```

No new compiler warnings on any of the three targets (same 4 pre-existing chibicc
`-Wmissing-field-initializers` warnings and the pre-existing RWX-segment linker warning).

---

## 11. Phase 2 completion notes

### 11.1 Files touched

| File | Change |
|---|---|
| `user/lisp/include/lisp.h` | `lisp_val_t.u.str`/`.u.sym` changed from inline `char[128]`/`char[32]` to `char *` (pointers into the new string pool); `lambda.body` is now documented as a list of forms; `lambda.env` documents the `NULL` sentinel |
| `user/lisp/lisp.c` | String pool (`alloc_string_slot`); `make_str`/`make_sym` allocate from it; `prim_eq` compares strings/symbols; `define` desugars `(name arg...)` to a `lambda` and applies the same env-sentinel rule as the `lambda` special form; `lambda` special form captures the whole body list, not just the first form, and stores `NULL` for globally-scoped closures; the lambda-call site evaluates all body forms in sequence and resolves a `NULL` closure env against the live `global_env`; `lisp_eval` renamed to `lisp_eval_step` with a new public `lisp_eval` wrapper enforcing `LISP_MAX_EVAL_DEPTH`; added `prim_help` and its registration |
| `drivers/include/drivers/block.h`, `drivers/ramdisk.c` | Added `ramdisk_max_blocks()`, exposing the driver's real compile-time capacity |
| `fs/vfs_server.c` | `vfs_mount_ramdisk()` clamps the requested size against `ramdisk_max_blocks()` and warns instead of silently accepting an oversized request |
| `kernel/shell.c` | `cmd_help()` now mentions `(help)` |
| `tests/runner.py` | Added 8 permanent regression tests (B3, B4, B5, multi-body lambda, depth guard, `(help)` ×2 — Lisp-level and shell-level); fixed a latent echo-stripping bug in `_strip_echo` found while adding the `(help)` test (see [11.3](#113-a-test-harness-bug-found-while-testing-27)) |

### 11.2 2.6: the actual memory savings achieved

The original review's V6 finding described the *combined* Lisp (557 KB) and chibicc (311 KB) AST
node pools as "868 KB, 48% of BSS," and Phase 2's plan item said shrinking `lisp_val_t` would
"recover most of the 868 KB." That framing turned out to be optimistic once the actual tradeoff was
worked through, and it's worth being precise about what was and wasn't achievable here rather than
letting the earlier estimate stand uncorrected.

**What changed:** `lisp_val_t` shrank from 136 B (dominated by the inline 128-byte `str[128]`
field, paid by *every* node regardless of whether it's a string, a pair, or an int) to 32 B
(dominated by the 3-pointer lambda-closure member). This only reduces memory if the separate string
pool is smaller than `node_pool` — sizing them 1:1 would reserve a full 128-byte slot for every
possible allocation exactly as before, yielding zero net savings (in fact slightly worse, from
added pointer/padding overhead). So the string pool was deliberately sized at `NODE_POOL_SIZE / 2`
(2048 slots on QEMU targets), not `NODE_POOL_SIZE`.

**Why not smaller, for bigger savings:** `env_set()` — called on *every* `define`, every `let`
binding, and every function call's parameter binding, not just once per unique name — allocates one
symbol via `make_sym()` per call. That makes symbol/string allocations a substantial, roughly
constant fraction of total node allocations in this evaluator's actual usage pattern (not just a
rare edge case), so shrinking the string pool much further risked making long interactive sessions
hit string-pool exhaustion sooner than they used to hit node-pool exhaustion — a real regression in
practical capacity, even though (thanks to Phase 1's clamp-not-wrap fix) the failure mode would be
"aliased/wrong output," not a crash.

**Measured result** (RV64, `riscv64-elf-nm --size-sort -S`):

| | Before | After |
|---|---:|---:|
| Lisp `node_pool` | 557,056 B (4096 × 136 B) | 131,072 B (4096 × 32 B) |
| Lisp `string_pool` | *(n/a, inline)* | 262,144 B (2048 × 128 B) |
| **Lisp engine total** | **557,056 B** | **393,216 B** |

**Net savings: 163,840 B (160 KB)**, confirmed by total kernel BSS dropping by exactly that amount
(1,789,952 B → 1,626,112 B). That's real, but it's roughly 19% of the combined 868 KB figure, not
"most" of it — the other ~700 KB is chibicc's separate `node_pool`/`obj_pool`/`type_pool`/etc.
(311 KB) plus the 512 KB `ramdisk_storage` array, neither of which this phase touched (chibicc's
pools are a different struct with a different, already-reasonably-sized `Node`/`Obj`/`Type` layout
— not dominated by an oversized inline string buffer the way `lisp_val_t` was — so the same fix
doesn't apply there in the same way; `ramdisk_storage` is a RAM-backed disk image, sized by design,
not a node pool). The `-Os` compiler-flag lever noted in the original review (measured at the time
as text −17%/data −48%) remains a separate, larger, and still-untaken opportunity — it wasn't part
of this phase's scope (Lisp engine correctness) and changes CMake defaults project-wide rather than
one subsystem, so it's left as a deliberate follow-up rather than folded in here.

### 11.3 A test-harness bug found while testing 2.7

Adding the `(help)` regression test exposed a real bug in Phase 0's `_strip_echo()` (see
[§9](#9-phase-0-completion-notes)), not in LugalOS itself. `_strip_echo` used `re.sub(pattern, "",
text)` with no `count` limit, which strips *every* occurrence of the sent command text from the
captured output, not just the actual keystroke echo. That's fine for a long or unusual command
(unlikely to recur verbatim in real output), but the shell-level test sent the short, common command
`"help"` — and the shell's own `(help)` mention in its command list legitimately contains "help" as
a substring, so the global substitution silently deleted that real, later occurrence along with the
actual echo, and the test's `\(help\)` pattern could no longer match. Fixed by adding `count=1`:
since `_drain()` guarantees the buffer is empty immediately before the command is written, the
*first* occurrence of the command text in the accumulated buffer is always the genuine echo, and
only that one needs to be (and now is) removed. Re-verified this doesn't weaken the Phase 0
tautology guarantees by re-running the original bogus-assertion, real-evaluation, and
echo-only-marker probes from §9.2 against the patched harness — all three still behave correctly.

### 11.4 Build verification

All three targets rebuilt clean from scratch with Phase 2 changes applied:

```bash
rm -rf build/rv64 build/rv32 build/rp2350
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake && ninja -C build/rv64
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake && ninja -C build/rv32
cmake -B build/rp2350 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rp2350.cmake -DLUGALOS_TARGET=RP2350 && ninja -C build/rp2350
python3 tests/runner.py   # 69 / 69 Tests PASSED
```

No new compiler warnings on any of the three targets.

---

## 12. Phase 3 completion notes

### 12.1 Files touched

| File | Change |
|---|---|
| `fs/fat32.c` | Near-total rewrite (591 lines changed): added `fat32_free_chain()`, `fat32_scan_dir()` (+ the `fat32_dir_scan_fn` callback type), `fat32_append_file()`; converted every directory-scanning function (`fat32_get_parent_cluster`, `fat32_find_file`, `fat32_write_file`, `fat32_mkdir`, `fat32_remove_file`, `fat32_rmdir`, `fat32_list_dir`) to use `fat32_scan_dir()`; fixed `fat_set_entry()`'s FAT2 offset and `fat_alloc_cluster()`'s range; removed `fat32_init()`'s auto-format branch |
| `fs/include/fs/fat32.h` | Declared `fat32_append_file()` |
| `fs/include/fs/vfs.h`, `fs/vfs_server.c` | Added `vfs_append()` (routes to `fat32_append_file()`) and `vfs_format()` (the new explicit-format entry point, replacing the removed auto-format) |
| `user/lisp/lisp.c` | Added `prim_format` and its `"format"` registration |
| `kernel/shell.c` | Added a `cmd_help()` line for `format` |
| `kernel/line_editor.c` | `add_history()` now calls `vfs_append()` with just the new line instead of rebuilding and `vfs_write()`-ing the whole accumulated buffer |
| `drivers/spisd_rp2350.c` | Added LBA bound-checks to `spisd_read_blocks`/`spisd_write_blocks` (found while fixing 3.3, see [12.3](#123-a-bound-check-gap-found-while-fixing-33)) |
| `tests/runner.py` | Added 3 permanent regression tests (B8 cluster-chain leak, B9 multi-file directory scan, B10 explicit format) |

### 12.2 3.2: worse than described in two spots

The original review's B9 finding described directory scans as reading only a cluster's *first
sector*, implying they still correctly walked from cluster to cluster via the FAT chain for
multi-cluster directories. Auditing every scan site while building the shared `fat32_scan_dir()`
helper found two functions where that wasn't true: `fat32_write_file()`'s and `fat32_mkdir()`'s
free-slot searches read `cluster_to_lba(fs, parent_clus)` — the parent directory's *first* cluster,
*first* sector — with no loop over the FAT chain at all. A directory whose first cluster's first 16
slots were all in use would report "directory full" even if the FAT chain had a second cluster with
room, or (for a real, larger `sec_per_clus`) even if the *same* cluster had more sectors with room.
Routing both through `fat32_scan_dir()` fixes this as a natural consequence of using one correct
implementation everywhere instead of seven hand-rolled ones, rather than requiring a separate fix.

One related question that came up but was deliberately left out of scope: none of these functions
ever *grow* a directory into a new cluster once its existing chain is completely full (no free slot
found anywhere `fat32_scan_dir()` looks) — `fat32_write_file()`/`fat32_mkdir()` still return -1 in
that case, exactly as before. Implementing on-demand directory growth is a reasonable next step, but
it's a distinct feature (not a scanning-correctness bug) and B9's own wording is specifically about
scanning existing sectors correctly. The multi-sector fix alone raises the practical entry-count
ceiling dramatically for any real (larger-cluster) SD card, which covers the realistic near-term
need.

### 12.3 A bound-check gap found while fixing 3.3

`fat_alloc_cluster()`'s old range bug (using `tot_sec32`, a *sector* count, as if it were a cluster
count) meant it could hand out a cluster number whose corresponding LBA — computed by
`cluster_to_lba()` — fell outside the real data region, and `fat32_write_file()`/`fat32_mkdir()`
would then write to that LBA via the block device driver. Checking whether that could actually reach
storage outside the intended volume led to auditing all four block-device drivers'
`read_blocks`/`write_blocks`: `virtio_blk`, `flashdisk`, and `ramdisk` all independently bound-check
the LBA against the device's real capacity (so this was survivable on QEMU and on RP2350's flash
path even before the `fat_alloc_cluster()` fix). `spisd_rp2350.c` — the physical SD card driver used
on real RP2350 hardware — had no bound check at all; it sent whatever LBA it was given straight to
the card over SPI. Not a kernel memory-safety issue (it's confined to the SD card's own address
space via the SPI protocol, not RAM), but a real gap: it could have silently read or written sectors
outside the mounted FAT32 volume's real bounds on a physical card. Added the same
`lba + count > dev->num_blocks` check the other three drivers already have (plus an overflow guard
on `lba + count` itself, which none of the other three have either — a cheap, low-risk addition
made while already touching this exact line, not applied retroactively to the other three since
they'd require a similarly-astronomical `count` to matter and aren't reachable that way from this
codebase's own FAT32 code).

### 12.4 3.4: why removing auto-format was safe for the test suite

Before removing `fat32_init()`'s auto-format-on-invalid-boot-sector branch, checked whether the
QEMU test suite's `/sd0/` and `/flash0/` images depend on it to become valid FAT32 volumes on first
boot. `tools/create_sd_image.py` (the QEMU `/sd0/` image generator) and `tools/create_flash_fs.py`
(the embedded `/flash0/` image, baked in at build time) both write a fully valid FAT32 boot sector
(`boot_sig = 0x29`, `0x55AA` signature) directly into the image file at *build* time — neither ever
relied on the kernel formatting anything at runtime. `/ram0/` is the only volume that legitimately
starts blank every boot, and `vfs_mount_ramdisk()` already had (and still has, unmodified) its own
explicit `fat32_format()` + `fat32_init()` fallback at the call site, independent of whatever
`fat32_init()` does internally on an invalid boot sector. So removing the internal auto-format only
changes behavior for the case B10 was about — a corrupt or foreign-formatted `/sd0/` card — without
affecting how any currently-tested volume gets initialized.

### 12.5 3.6: narrower than planned, and why that's enough

The original plan item envisioned "a test that writes/deletes several hundred files and asserts
`df` free space returns to baseline." The delivered test instead overwrites a single file 20 times
and asserts free space is unchanged before vs. after. This is narrower but exercises exactly the
code path that was actually broken (`fat32_write_file()`'s missing chain-free on overwrite) with a
precise, fast, deterministic assertion: each overwrite needs exactly one cluster, so any leak at all
would show up as a free-space decrease, and the old bug would have leaked all 19 of the repeated
writes' worth. A several-hundred-file version would additionally exercise `fat32_remove_file()`'s
chain-free and sustained allocator behavior under churn, which is a reasonable further hardening
step, but the current test already catches the specific class of regression this phase fixed, so
building the larger stress harness wasn't essential to closing out this phase.

### 12.6 Build and test verification

All three targets rebuilt clean from scratch (not incrementally) after Phase 3 changes, given how
much of `fs/fat32.c` changed:

```bash
rm -rf build/rv64 build/rv32 build/rp2350
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake && ninja -C build/rv64
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake && ninja -C build/rv32
cmake -B build/rp2350 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rp2350.cmake -DLUGALOS_TARGET=RP2350 && ninja -C build/rp2350
python3 tests/runner.py   # 75 / 75 Tests PASSED
```

No new compiler warnings on any of the three targets. One test-script-only bug was found and fixed
while writing the B8 regression test: the `df` output line contains the string `/ram0/` twice (once
as the Filesystem column, once as the Mounted-on column at the end of the same line), so
`send_and_expect(..., r"/ram0/", ...)` matched and returned on the *first* occurrence, before the
numeric fields in between had streamed in — the fix waits for the trailing `% /ram0/` instead. A
second, non-bug false alarm during the same test's development: an initial version measured free
space *before the target file existed at all*, so the file's own first (correct, necessary)
cluster allocation was miscounted as a 1-cluster "leak" — fixed by creating the file first and only
then taking both measurements, isolating the repeated-overwrite behavior the test is meant to
check.

---

## 13. Phase 4 completion notes

### 13.1 4.2: the factorial example was already fixed by Phase 2

Before touching the README, checked whether the factorial example actually still needed a doc
fix, rather than assuming the original review's finding still applied. It didn't: Phase 2 fixed
B3 (self-recursion) and B4 (`(define (fn args...) body...)` signature form) as code changes, and
the README's example exercises exactly those two code paths. Verified live rather than by
inspection — booted the current RV64 build in QEMU and typed the exact example from the README:

```
lsh> (define (factorial n) (if (= n 0) 1 (* n (factorial (- n 1)))))
=> factorial
lsh> (factorial 6)
=> 720
```

This is a useful data point beyond just "one less doc edit": it's an end-to-end confirmation that
the Phase 2 fixes for B3 and B4 actually work for the exact scenario the original review used to
find them, not just for the narrower regression tests added at the time.

### 13.2 Why a dedicated status section instead of editing every bullet in place

The original plan item (4.1) said "rewrite the README's IPC / scheduler / MMU / 9P bullets to
describe what exists, marking the rest as roadmap." Editing every individual bullet in place would
have worked but scatters the caveats across the document, each easy to skim past, and duplicates
the same explanation in multiple places (harder to keep in sync later). Instead, added one
prominent "Implementation Status" section directly after the intro, before the enthusiastic
feature list — a reader hits the honest summary first, before anything has a chance to mislead
them — and trimmed the specific bullets that actively overclaimed (the IPC bullet, the `(ps)`
primitive description) to be accurate on their own while pointing back to the fuller explanation
rather than repeating it. Bullets that were already honestly hedged (`/dev/ttyACM1`'s two existing
mentions, `(p9-loopback)`'s "in-memory transport gateway" phrasing) were left alone.

Initially left the title/intro's use of "microkernel" alone, on the reasoning that the
architectural vision is a legitimate thing for a project to name itself after and that renaming is
a bigger editorial call than "keep the docs honest" implies — and that the status section makes the
gap explicit within the first screen regardless. On reflection (prompted by the user, not
self-initiated) this drew the line in the wrong place: someone who only reads the title still walks
away with the false impression, and "the status section is right below it" only helps readers who
keep scrolling. See 13.2.1.

### 13.2.1 Follow-up: the title itself, not just the section below it

Changed the title from "LugalOS: Bare-Metal RISC-V **Microkernel** Operating System" to "LugalOS:
Bare-Metal RISC-V Operating System", and reworded the opening sentence to state the microkernel
architecture as the *design goal* ("built toward a microkernel architecture as its long-term design
goal") rather than a present-tense fact, with an inline pointer to Implementation Status. Added an
explicit parenthetical in the README itself noting the title is deliberately provisional and should
have "Microkernel" restored once Phase 5 (real IPC + scheduler + MMU) lands — flagging that here too
so it isn't only discoverable by reading the README's own text.

**Action item for whoever picks up Phase 5**: once 5.1–5.5 (or enough of them that IPC/scheduling/
MMU are real, not stubs) are done, restore "Microkernel" to the README title and opening sentence,
and remove the parenthetical about the title being provisional.

### 13.3 What this phase didn't do

No code, build, or test changes — only `README.md`. Confirmed nothing else needed to change by
re-running the full build+test verification from Phase 3 was still current (no new commits since
that verification), so no fresh build/test run was performed for a docs-only change. The title
follow-up in 13.2.1 is likewise docs-only.

---

## 14. Cross-cutting quick wins completion notes

### 14.0 These were genuinely unaddressed

When asked, checked all four against the actual current code rather than assuming the four phases
had swept them up along the way — they hadn't. None of the four numbered phases targeted this
bucket, which is exactly why it exists as a separate section rather than being folded into one of
them: `usb_cdc_task()` was still only called from `readline_interactive`'s loop; `CMakeLists.txt`
still unconditionally overwrote `CMAKE_C_FLAGS` with no `-O` flag anywhere; the `/dev/uart`
1-byte overflow and the `fat32.c` alignment casts were untouched by the Phase 3 filesystem rewrite,
which had different, more urgent targets.

### 14.1 X.3: which `strncpy` sites were actually broken

Rather than assume every `strncpy` call was equally risky, read every site in both files before
deciding what needed a real fix vs. a consistency cleanup:

- **`kernel/line_editor.c`**: 5 of 13 sites already paired `strncpy(dst, src, N-1)` with an explicit
  `dst[N-1] = '\0'` immediately after (safe already). The other **8 were genuinely missing
  termination** — reachable via, for example, typing an exactly-511-character line into
  `readline_interactive` and then navigating history (`temp_saved_line`), or an exactly-127-character
  filename into the "Find file:" / "Write file:" status-line prompts (`active_filename`). All 13
  were converted to `safe_strncpy()` for consistency, even the already-safe ones, since the
  now-single helper is harder to misuse than the same two-line pattern repeated by hand 13 times.
- **`fs/fat32.c`**: 1 of 2 sites (`path_copy`) was already safe. The other
  (`fat32_get_parent_cluster`'s `out_name` output, a 64-byte caller buffer) was missing termination
  for any path component of exactly 63+ characters — reachable via e.g.
  `mkdir /sd0/<63-character-name>`. Both converted to the same `safe_strncpy()` pattern (a
  file-local copy, mirroring the existing `strncpy_local()` in `user/lisp/lisp.c` — three files now
  independently define the identical fix for the identical footgun, which is a reasonable amount of
  duplication for a 6-line static helper rather than justifying a shared libc-style utility header
  for this codebase's current size).

### 14.2 X.4: why union, and what was left alone

Chose the union approach over memcpy-based accessors: `fs/fat32.c`'s access pattern is pervasively
`fat32_dir_entry_t *entries = (view); entries[i].field = ...` (array-style, used identically across
six scan-callback functions from the Phase 3 rewrite) — converting that to explicit
memcpy-in/memcpy-out at every field access would have been substantially more invasive and less
readable than giving the buffer the right type in the first place. The `fat32_sector_t` union
(`raw[512]` / `words[128]` / `entries[16]` / `bpb`) does that: the compiler guarantees correct
alignment for the strictest member, so every view is safe with no cast anywhere.

Deliberately **not** converted: sector buffers only ever touched via `memcpy()` (`fat32_init`'s
boot-sector read, `fat32_read_file`'s and `fat32_write_file`'s data buffers, two of
`fat32_append_file`'s three sector buffers, `fat32_statfs`'s `fat_buf`). `memcpy()` doesn't care
about the alignment of a `uint8_t*` source or destination, so these were never actually affected by
the violation this fix addresses — converting them too would have been churn with no correctness
benefit.

Also worth recording plainly: this was **never observed as a live fault**. UBSan's alignment check
(`-fsanitize=undefined`, panic-on-fault since Phase 0) has been active through every test run across
Phases 0-3 and this cross-cutting pass, and never fired on this pattern — meaning the compiler has
consistently placed these stack buffers at addresses that happen to satisfy the stricter alignment
anyway. That's implementation behavior (a plausible one, even: compilers commonly over-align stack
allocations for performance), not a standard guarantee, so the fix stands, but it's worth being
honest that this was a "make the code correct per the standard" fix, not a "stop an observed crash"
fix, unlike most of the rest of this remediation effort.

### 14.3 Build and test verification

All three targets rebuilt clean from scratch:

```bash
rm -rf build/rv64 build/rv32 build/rp2350
cmake -B build/rv64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv64-mmu.cmake && ninja -C build/rv64
cmake -B build/rv32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rv32-nommu.cmake && ninja -C build/rv32
cmake -B build/rp2350 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-rp2350.cmake -DLUGALOS_TARGET=RP2350 && ninja -C build/rp2350
python3 tests/runner.py   # 75 / 75 Tests PASSED
```

No new compiler warnings on any of the three targets. X.1 was additionally verified with two
throwaway builds outside the repo: `-DCMAKE_BUILD_TYPE=Debug` correctly overrides the new
`MinSizeRel` default, and a caller-supplied `-DCMAKE_C_FLAGS="-DMY_CUSTOM_FLAG=1"` correctly
survives into the actual compile commands (`ninja -t commands`) instead of being silently
discarded.
