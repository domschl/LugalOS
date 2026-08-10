# Phase 6 — Reclaim, Then Subdivide

**Status:** proposed, 2026-08-10. Successor to `phase5_distributed_design.md` (Tracks A and B, complete at v0.8.0).

**Theme.** Phase 5 proved a microkernel *shape*: tasks, channels, U-mode, hardware-enforced
domains, a distributed namespace. It did so inside a single linked image. Phase 6 makes the shape
real by taking the image apart — which is only possible if there is RAM to put the pieces in, and
that is the same problem stated twice.

---

## 0. Verdict on the proposal

The three objectives and their interdependence are correctly identified, and the `/system/bin`
convention is the right organising idea. Three corrections change the order of work:

1. **"Break chibicc out into an executable" does not save RAM on its own — and today it cannot
   even run.** The loader's ceiling is not the 60 KB heap; it is a NAPOT alignment property that
   caps any user image at **32 KB** regardless of how the heap is otherwise doing. chibicc needs
   ~128 KB. §2.1 works this out; the conclusion is that the loader must be fixed *before*
   extraction, not as part of it.

2. **The extraction pays for itself, but only just, and only in the right order.** Freeing
   chibicc's 108 KB of `.bss` lowers `_kernel_end` by the same 108 KB, which is what makes a
   128 KB region placeable at all. It closes with ~40 KB to spare — enough for chibicc alone,
   not enough for a second process. "More than one user program at a time" therefore depends on
   the *other* reclamations (§3.5), not on the loader work.

3. **A process that cannot reach a service is not a component.** Extracting `cc` requires argv,
   an exit status, and a way to call the VFS — none of which the current user ABI has. The
   `sys_ipc_*` stubs should not merely be deleted, as the proposal suggests: they should be
   *replaced* by the one syscall that makes componentisation possible, `SYS_CHAN_CALL` (§3.3).
   Deleting them alone would remove four stubs and leave user processes exactly as isolated.

One scoping disagreement, in §6: the editor's redraw bug should be fixed now, in place, and not
folded into "make `e` an executable". They are a two-line fix and a multi-milestone refactor, and
tying them together delays the fix by the length of the refactor.

---

## 1. Where the memory actually is

Measured from `build/rp2350/lugalos.elf` at v0.8.0 + the meminfo work of 2026-08-10. RP2350 has
520 KB SRAM (512 KB main + 2×4 KB scratch) and 4 MB flash.

### Flash: 629 KB of 4096 KB — not a constraint

| | |
|---|---|
| Embedded FAT32 payload (`g_flash_fs_start`) | 512 KB |
| Kernel `.text` | 87 KB |
| `.rodata` (excl. payload) | 22 KB |
| `.utext`, `.data` LMA, binary info | ~8 KB |

**Flash is 15% used.** Anything that moves work from RAM to flash is a good trade on this board,
and there is room for a much larger `/flash0/system/bin`.

### RAM: 452 KB static, 60 KB heap — the whole problem

Static RAM by subsystem (`.data` + `.bss`, symbol-attributed):

| Subsystem | RAM | Character |
|---|---|---|
| `drivers/` | 121.0 KB | `ramdisk_storage` 64 KB, `uart_net` 34 KB, `usb_cdc` 16 KB |
| `user/chibicc` | 108.1 KB | eleven fixed pools; **cold** — used only by `cc` |
| `user/lisp` | 64.6 KB | `string_pool` 32 KB, `node_pool` 8 KB; **hot** — this is the shell |
| `fs/` | 57.0 KB | nine separate 4 KB 9P buffers + `g_waiters` 8 KB + `g_handles` 5.6 KB |
| `user/ed` | 44.1 KB | `ed_buf` 32 KB, `out_buf` 8 KB; **cold** |
| `kernel/` | 27.0 KB | `history_stack` 16 KB, `g_ring` 4 KB, `g_user_stack` 4 KB |
| `arch/` | 2.1 KB | |

Heap: `[_kernel_end 0x20071000, _heap_end 0x20080000)` = **60 KB, 15 pages**. Boot stack 16 KB,
measured peak ~6 KB on the QEMU targets after a full suite run.

The shape of the problem in one line: **152 KB of the 452 KB is two programs that are almost never
running**, and another ~100 KB is buffers sized for convenience rather than for this board.

---

## 2. The three constraints that reorder the work

### 2.1 NAPOT alignment caps the user image at 32 KB today

Hazard3 implements only NAPOT PMP regions (`mem_domain.h`), so every granted range is
power-of-two sized *and self-aligned*. `palloc_pages_aligned()` therefore has to find a
`k`-aligned run of `k` bytes inside the heap.

Because `_heap_end` is the end of RAM and therefore 512 KB-aligned, a `k`-byte NAPOT block exists
in the heap **iff the heap is at least `k` bytes and its top `k` bytes are free** — the aligned
candidate is always `_heap_end - k`.

With a 60 KB heap:

| Region size | Aligned candidate | In heap? |
|---|---|---|
| 32 KB | `0x20078000` | yes — exactly fits at the top |
| 64 KB | `0x20070000` | **no** — below `_kernel_end` (`0x20071000`) |
| 128 KB | `0x20060000` | no |

So the current image ceiling is 32 KB, and `USER_IMAGE_PAGES = 2` (8 KB) is well under even that.
This is worth stating precisely because it kills a tempting shortcut: **no amount of careful
allocation gets a 128 KB region out of a 60 KB heap.** The heap has to grow first, and the only
place that growth can come from is static `.bss`.

It also kills a second tempting move — reserving a NAPOT-aligned arena in the linker script. That
statically spends the RAM whether or not a process is running, which is the cost we are trying to
remove.

### 2.2 The arithmetic closes, in one specific order

Per-*segment* NAPOT regions (as `README.md` already identifies as the requirement) are much
cheaper than one arena per process, because a 152 KB process does not need a 256 KB power of two:

```
chibicc as a process:  text 12 KB → 16 KB region
                       data+bss 108 KB → 128 KB region
                       stack → 8 KB region
                                        3 regions, 152 KB
```

Three regions fits `MEM_DOMAIN_MAX_REGIONS = 4`, and RP2350 has 5 dynamic PMP entries free after
the three hardwired-grant shadows. Now check placement after chibicc's `.bss` leaves the image
(`_kernel_end` drops 108 KB to `0x20056000`, heap becomes 168 KB):

| Need | Aligned candidate | Fits? |
|---|---|---|
| 128 KB data | `0x20060000` | yes — occupies the top 128 KB |
| 16 KB text | `0x20058000` | yes |
| 8 KB stack | `0x20056000` | yes |
| remaining free | | **~16 KB** |

**It closes — with almost nothing left over.** One user process, and the page allocator down to
its last few pages. That is the quantitative reason objectives 1, 2 and 3 are one piece of work:
extracting chibicc makes `cc` runnable and simultaneously exhausts the heap, so *concurrent*
processes require the §3.5 reclamations as a hard prerequisite rather than a nice-to-have.

Two contingencies, in preference order:

- **Trim chibicc's pools** on the way out. Note the step function: NAPOT means anything between
  65 KB and 128 KB costs a 128 KB region, so trimming 108 KB → 80 KB buys *nothing*. The pools
  have to come down to **≤ 64 KB** to halve the region, which means roughly halving
  `node_pool` (23.5 KB), `macros` (20 KB), `preproc_buf` (16 KB) and `token_pool` (12 KB).
  Smaller programs compile; on an MCU that is the right trade, and it doubles the spare heap from
  16 KB to 80 KB. **Recommended.**
- **XIP the text from flash.** Flash is at `0x10000000`, outside all three hardwired-grant
  shadows, so a domain can grant it `R|X` directly and the text costs no RAM at all. Elegant, and
  the biggest single win available — but it needs the executable's bytes to be contiguous and
  NAPOT-aligned *inside the FAT32 image*, which is a build-time layout problem. **Prototype only;
  not on the critical path.**

### 2.3 Componentisation needs a syscall surface that does not exist

The user ABI (`user/progs/usys.h`) is `print`, `putnum`, `putchar`, `read_file`, `write_file`,
plus `SYS_UEXIT`. A `cc.elf` invoked as `cc test.c /ram0/system/bin/test.elf` needs, at minimum:

- **argv** — there is no mechanism at all today. Not a new syscall: a parameter block written into
  the process's data region before entry (§3.3).
- **an exit status** — `SYS_UEXIT` carries one but nothing propagates it to the shell.
- **service access** — `read_file`/`write_file` happen to be enough for `cc`, but not for anything
  that talks to a *server*. The moment the VFS or the console becomes a process, every other
  process needs `chan_call()`, and U-mode has no route to it.

---

## 3. The plan

Eight items. C0 and C1 are independent and land first; C2–C4 are the loader keystone and can be
developed entirely on QEMU (16 MB heap) before RP2350 has room for them; C5 is what makes RP2350
able to run the result; C6–C8 are the payoff.

Per `plan/` convention and the falsification rule that Phase 5 learned four times over: **every
item below is validated on real RP2350 silicon, not on QEMU alone.** QEMU has 128 MB of RAM and
no NAPOT restriction; it cannot fail any of the memory constraints in §2.

### C0 — Console/stream hygiene *(independent; land first)* — **Done (2026-08-10)**

The three reported bugs are three instances of one incomplete migration: B4 split the kernel log
from the interactive console, and a set of call sites and one whole subsystem never moved. Details
and root causes in §6. No dependencies, no memory cost, and it unblocks C7 (an editor that is a
process must write through the console, not through `uart_puts()`).

**Completed** on branch `phase6-c0-console-streams`. Four changes:

1. CRLF conversion moved from `vprintk_to()` to `console_emit()` (`kernel/console.c`), with a
   shim in `kernel/main.c` giving the klog console sink the same treatment. See the correction in
   §6.1 — the layer originally proposed here would have corrupted SLIP frames.
2. `kernel/line_editor.c`'s 29 `uart_puts()` calls routed through `console_puts()`. This was the
   unreported half of bug 3: the editor painted to the physical UART regardless of what the
   console was bound to, so on RP2350 with a USB console it drew to the wrong device.
3. `\033[J` after the status line in `redraw_box()`.
4. 19 call sites moved from `printk()` to `cprintf()`: the FAT32 directory listing and `rmdir`
   refusals (`fs/fat32.c`, 11) and the whole of `i2c_scan_bus()` (`drivers/i2c_rtc.c`, 8).
   Mount/format/parse messages stay on the log, which is what they are.

Tests 133 → **141 QEMU**, 7 → **9 hardware**. The CRLF test boots its own QEMU in binary mode: `QemuSession` opens the
process with `text=True`, so Python's universal-newline translation rewrites `\r\n` to `\n` before
any assertion can see it — fixed and broken output are byte-identical to the entire existing
suite, which is how the defect survived this long. All three new tests were falsified by
re-injecting their defect and confirming each one fails alone.

**Still to validate on RP2350 silicon**: the console-binding half of (2) is precisely the part
QEMU cannot exercise, since QEMU has no USB CDC console to bind to.

### C1 — `/system/bin` and `/system/etc` *(independent)* — **Done (2026-08-10)**

Adopt the proposed convention. Refinements:

- **Search path is a variable, not a constant.** Default `("/ram0" "/sd0" "/flash0")`, set in
  `init.lisp` and readable as `/proc/path`. The proposal hardcodes the order; making it policy
  costs nothing now and is consistent with "init.lisp starts the components".
- **Name the trust decision.** ram0-first means anything that can write `/ram0/system/bin` shadows
  every system utility. For a single-user machine that is the *desired* override semantics — but
  it should be written down as a decision, not discovered later as a surprise.
- **Builtins win until they are replaced.** A `cc` builtin and a `cc.elf` on the path must never
  both be live: the builtin shadows the binary and the extraction looks like it silently did
  nothing. Removing the builtin belongs in the *same commit* as landing its replacement.
- **Migration has teeth.** `/sd0/system/init.lisp` and `stdlib.lisp` move to `/system/etc/`;
  `tools/sd_root/`, the generated flash image, the hardcoded paths in `user/lisp/lisp.c`, and at
  least one runner assertion move with them.

Deliverable: a bare name at the prompt runs `/<vol>/system/bin/<name>.elf`; a copy written to a
higher-priority volume takes precedence; a full path always wins.

**Completed** on branch `phase6-c1-search-path`. The deliverable is `uhello` rather than `hello` --
the three existing user programs are what ship, and inventing a fourth to match a draft sentence
would have been the wrong way round.

- `kernel/path.c` holds the path and resolves `/<vol>/system/<subdir>/<name><suffix>`, used for
  both `bin` (programs) and `etc` (boot scripts). Default order ram0, sd0, flash0.
- The shell resolves a bare word after builtins and **before** Lisp, and skips anything containing
  a `/`. Precedence and the reasoning are written out at the call site in `kernel/shell.c`.
- `lisp_init()`'s hardcoded "try /sd0 then /flash0" for `stdlib.lisp` and `init.lisp` is gone: those
  *were* a search path that only one function knew about. They now go through `path_resolve("etc", …)`.
- `/proc/path` reports it; `(path-set "…")` reorders it from `init.lisp`; `(which "name")` reports
  where a name resolves without running it.
- `tools/sd_root/system/{init,stdlib}.lisp` moved to `system/etc/`, and the user programs stage into
  `system/bin/` instead of the volume root. Migration touched the CMake staging, `lisp.c`, the QEMU
  runner (7 references) and the hardware suite (3).

**A build bug fell out of the migration and is fixed here.** The image generator's `DEPENDS` never
listed the staged files, so editing `init.lisp` left both images holding the previous copy with no
build step disagreeing. It surfaced as three unrelated *9P transport* tests failing on content that
should have matched -- they compare what the image serves against what the source now says. Now a
`GLOB_RECURSE CONFIGURE_DEPENDS` over `tools/sd_root/`.

**Note for existing checkouts**: `build/lugalos_sd.img` is created `--if-missing` and deliberately
survives rebuilds, so an image made before this change still has the old layout. Delete it once (or
the whole `build/`) to pick up `system/etc` and `system/bin`.

Tests 141 → **153 QEMU** (six new C1 tests per architecture). The precedence test asserts by
*moving the answer* -- `(which)` pointing at `/ram0` after a copy when it pointed at `/sd0` before
-- rather than by reading the path back, so it cannot pass on a path that is merely stored
correctly and never consulted.

**Deliberately not done here**: `ls /sd0/system/` with a trailing slash reports "not found" while
`ls /sd0/system` works. Pre-existing for every subdirectory (`ls /sd0/docs/` fails the same way),
unrelated to the search path, and worth its own small fix in `fs/fat32.c`'s path resolution rather
than being smuggled into this one.

### C2 — Loader: per-process domains, and freeing them — **Done (2026-08-10)**

The documented blocker for concurrency was real: the Sv39 backend caches a page table per domain
and nothing could free a page-table tree, so a domain per exec leaked pages. Fixed first —
`vmm_free_table()` walking and returning the tree — then the single `g_udomain`/`g_image` slot was
replaced with per-process allocation. Developed on QEMU, where the heap is 16 MB, then validated on
the board where it is fifteen pages.

**Completed** on branch `phase6-c2-per-process-domains`.

- `vmm_free_table()` (Sv39) walks the tree and returns only the *tables*, never the memory they
  map. The distinction is the same R/W/X test `vmm_map_range()` uses on the way down: an entry with
  any of those bits is a leaf describing memory this tree does not own. Freeing on the wrong side of
  it would hand live kernel RAM back to the allocator. A no-op on NOMMU, so the loader never
  branches on the memory model.
- `mem_domain_destroy()` sits on top of it, and is where the two models stop differing again.
- The loader keeps a fixed table of process slots, each owning an image, a user stack and a domain,
  all returned when the program ends.
- `elf_spawn()` / `(spawn "path")` starts a program without waiting. This is what makes concurrency
  *observable*: `elf_load_and_run()` spins until its program is dead, so a caller using it can only
  ever have one resident however many slots exist.

**A bug found by the test rather than by reading.** Reaping first asked "is the recorded pid dead?",
which is unsound: `task_create()` recycles DEAD slots, so a stale pid can name a different, living
task, and the slot then never reaps. It showed up as a steady three-page climb across repeated
loads on the MMU build — one Sv39 tree per load. Liveness is now asked by *domain pointer*, which is
unique to a slot for as long as the slot is in use, and the domain is bound at `task_create()`
rather than only in the task body so there is no window where a live task looks finished.

**Measured.** An Sv39 user domain's page-table tree is exactly **3 pages** — the falsification run,
with `vmm_free_table()` stubbed, leaks 15 pages over 5 loads. On RP2350 two concurrent programs plus
their kernel stacks peak at **12 of 15 heap pages**, which is the number that will decide how much
of C5 is needed before a third.

Tests 153 → **157 QEMU**, 9 → **10 hardware**. Both new tests falsified: stubbing the free path
makes the reclamation test fail, and `USER_PROC_MAX = 1` makes the concurrency test fail.

### C3 — Process ABI — **Done (2026-08-10)**

- **Parameter block**: argv strings plus argc written into the top of the process's *stack* page
  before entry, with `a0`/`a1` set to argc/argv. No new syscall, and no kernel pointer handed to
  U-mode. The stack page rather than the data page: a program that used all of its .data/.bss would
  otherwise find its globals overwritten by its own arguments, a corruption whose presence depended
  on how big the program was.
- **Exit status** propagated from `SYS_UEXIT` to the shell (on the *console* stream, per C0) and to
  `/proc/ps`, which reports `killed` rather than a number for a task the fault handler ended. A
  status alone cannot distinguish them: 0 is both an ordinary return value and what a killed task
  leaves behind.
- **`SYS_CHAN_CALL`**: `(name, req, req_len, resp, resp_max)` → `chan_call()`, with both buffers
  validated through `kernel/uaccess.c` exactly as `SYS_READ_FILE` is. Services are named, never
  addressed by pointer or pid, so a program cannot express a reference to something it was not
  given.
- **`sys_ipc_call`/`_reply`/`_send`/`_recv` and `ipc_msg_t` deleted**, in the same change. Numbers
  1–4 stay burned rather than reused, so a binary built against the old ABI gets a clean "unknown
  syscall" instead of whatever later took its number.

Two new user programs carry the tests. `uargs` reads its arguments *through the pointers* — a
vector built at a kernel address would fault there rather than appearing to work — and returns 42,
a status that is neither 0 nor the 7 `uhello` returns, so a pass cannot come from a defaulted field.
`uchan` makes four claims: a marker emitted by the *console server* rather than by its own
`SYS_PRINT` (so the message demonstrably crossed the channel), an unregistered name refused, a
request buffer it does not own refused, and syscall 1 refused.

That third one is the confused-deputy case at the channel boundary, and it is the reason this item
has a hardware test: on RP2350 the refusal is decided against PMP regions on Hazard3, with NAPOT-only
matching and erratum E6's reversed permission bits. A boundary that holds under Sv39 demonstrates
nothing about that.

Tests 157 → **165 QEMU**, 10 → **11 hardware**.


### C4 — Loader: multi-page images, per-segment NAPOT — **Done (2026-08-10)**

Ran after C5, because until the heap could place a region worth having none of this was
demonstrable on the board where the constraint is real.

The image is now sized from the program headers and rounded up to a power-of-two page run;
`linker/user.ld`'s `_udata_end <= 0x2000` assert is gone. Each segment is granted what its own ELF
`p_flags` declare, so W^X follows from what the linker said rather than from the loader assuming
page 0 is text. `MEM_DOMAIN_MAX_REGIONS` 4 → 5, which is exactly RP2350's dynamic budget.

**A correction to §2.2.** It proposed allocating text and data as *separate* NAPOT regions —
"16 KB text + 128 KB data + 8 KB stack". That does not work. The programs are built
`-mcmodel=medany -mno-relax`, so data is reached PC-relative from text; separate allocations put
them at an unpredictable distance and every data reference breaks. The image has to stay
contiguous, with W^X granted as sub-regions inside one aligned block. Which is why a segment whose
page count is not a power of two is decomposed into aligned pieces — `grant_napot_span()`, the
standard largest-aligned-piece-first walk.

**The region budget, not placement, is what limits image shape.** This is the useful thing the
implementation taught, and it is worth writing down as arithmetic. With one page of text, a data
segment starting at page 1 decomposes into pieces of 1, 2, 4, …, so *B* regions buy 2^B − 1 data
pages. With the four regions an image gets (five less the stack), that is a 7-page data segment and
an 8-page image. But the ceiling scales with where data starts:

| Text pages | Data starts at | Max data pages with 3 regions | Max image |
|---|---|---|---|
| 1 | page 1 | 7 (1+2+4) | 8 pages / 32 KB |
| 2 | page 2 | 14 (2+4+8) | 16 pages / 64 KB |
| 4 | page 4 | 28 (4+8+16) | 32 pages / 128 KB |

So a program with a few pages of code can carry a lot of data, and one with a single page cannot.
That is a real property of NAPOT rather than an implementation limit, and the loader now reports it
by name: exceeding the budget says which segment, which pages, and that a non-power-of-two page
count costs one region per set bit.

**Two prerequisites this leaves for C6**, both small and both now visible rather than latent:

1. *The exit stub is at the top of page 0*, which caps text at one page (`user.ld` still asserts
   4080 bytes). chibicc's text is ~12 KB, so the stub has to move to the end of the text
   *segment* first.
2. *Data wants to start at a well-aligned page*, or the decomposition is wasteful — data at page 3
   costs pieces of 1+4+8 where page 4 would cost 4+8+16. A `. = ALIGN()` in the linker script,
   not a loader change.

**Measured.** `ubig` spans six pages in an eight-page run — deliberately at the region budget, its
data decomposing into 1+2+2 — writes across all 20 KB of its `.bss` and reads it back. Under the
previous model it would not have linked. `/proc/meminfo` reports the padding
(`User Images: 8 pages, 6 spanned (2 padding)`), which on a 40-page heap is the difference between
"the heap is full" and "the heap is full of padding". `uwx` stores into its own text and is
terminated, on both memory models.

Tests 165 → **171 QEMU**, 11 → **12 hardware**.


### C5 — Static RAM reclamation — **Done (2026-08-10)**

**Reordered before C4**, on the reasoning that C4 is fully developable on QEMU but not
*demonstrable* on RP2350 until the heap can place a region worth having. Doing C5 first keeps every
step hardware-validatable, which has been worth a great deal so far.

Measured on RP2350: `_kernel_end` `0x20071000` → `0x20058000`, so **image 434 KB → 335 KB and heap
60 KB → 160 KB** (15 → 40 pages). What that buys C4 is the point: a *k*-byte NAPOT region needs a
heap of at least *k* (§2.1), so the ceiling went from 32 KB to **128 KB**.

| Reclaimed | KB | How |
|---|---|---|
| `ramdisk_storage` | 64 | Heap-backed, and on RP2350 not mounted at all when `/sd0` is writable |
| `uart_net` SLIP buffers | 24 | Encode straight onto the wire; decode incrementally into the frame |
| `history_stack` | 12 | 32 entries → 8; the on-disk history keeps everything anyway |

**The RAM disk is now a decision, not a constant.** `(mounted? "/sd0")` and `(board)` are new, so
`init.lisp` can say "if there is a writable card, do not spend heap on a RAM disk". On RP2350 that
matters more than the pages: the heap starts at `0x20058000`, the only 128 KB-aligned address
inside it is `0x20060000`, and a 64 KB RAM disk allocated first-fit lands *across* that boundary.
It would not merely consume pages, it would halve the largest user image the loader can ever place.
When there is no card it takes 64 KB rather than something smaller, because this FAT32 layout spends
48 sectors on reserved space and two FATs — a 32 KB volume is two-thirds metadata and a 16 KB one is
smaller than its own bookkeeping. There is no good small size, which is what makes "none" the right
answer when there is an alternative.

**The SLIP buffers were never buying anything.** Both receive paths accumulated *escaped* bytes in
an 8 KB `raw[]` and decoded the lot on `SLIP_END`; the state a SLIP decoder needs between bytes is
one boolean. The transmit path encoded into another 8 KB buffer and then wrote it out one byte at a
time. Three buffers, none of which held anything the code needed.

**Two pre-existing bugs surfaced, both worth more than the memory.**

*A 16 KB RAM disk mounted and then failed every write.* `fat32_format` spends 48 of its sectors on
metadata, so a 32-sector volume is smaller than its own bookkeeping — and nothing checked. Now
`FAT32_MIN_SECTORS`, clamped with a diagnostic at mount.

*The SD image was shared across targets.* It carries user program binaries, which are
target-specific; one image meant whichever target built last decided what every other target's
tests loaded. It survived a long time because a simple program mostly works on the wrong ISA —
`uargs`, which dereferences pointers, faults on a 32-bit address sign-extended to 64. Now built per
build directory, with the runner, the multinode tests and the run scripts each using their own. The
image also regenerates when the staging changes (`--if-stale`), which is what exposed it: `--if-missing`
had been freezing whichever target built first.

**Deliberately not done**: the `fs/` 9P buffer consolidation (~30 KB). It is the highest-risk item
in the phase — those buffers serve potentially concurrent 9P paths in the most heavily tested
subsystem — and it would not change what C4 can place, since 256 KB would need a 256 KB heap and
that needs C6. Available if a later item wants it.

Tests: 165 QEMU (unchanged), 11 hardware, with a new floor asserting the heap can still hold a
128 KB image so a future static array cannot silently halve C4's ceiling.

A flaky test was also fixed. "Cooperative Tasks Interleave" required strict A1 B1 A2 B2 A3 B3, which
stopped being a property of a correct system when B6 added preemption: a tick can switch back to A
after it yields but before B prints. Measured at about one run in eight on a loaded host, on the
commit before this work as well as after, so the assertion was wrong rather than anything it
watched. It now asks for B1 before A3 — still fails for the no-op-yield bug it exists to catch.


### C6 — `cc` becomes a process

Extract `user/chibicc` to `/flash0/system/bin/cc.elf`, pools trimmed per §2.2. Remove the `cc`
builtin in the same commit (C1). Expected: static RAM −108 KB, heap 60 KB → 168 KB.

### C7 — `e` becomes a process

Same treatment for the editor (`user/ed` 44 KB + the `line_editor.c` box-redraw path). Note the
ordering: the redraw bug is fixed in C0, *before* this, so C7 is a pure relocation with no
behaviour change to debug simultaneously.

### C8 — `init.lisp` as the component launcher

Once C2–C4 land, `init.lisp` stops being a configuration script and becomes the thing that decides
what runs: mount policy, search path, which servers start. This is where the phase's story lands —
the kernel boots a scheduler and a namespace, and a Lisp script assembles an operating system on
top of it.

---

## 4. What is deliberately *not* in this phase

- **The Lisp evaluator as a process.** 64.6 KB and tempting, but it is the shell — the hot path
  and the thing that launches everything else. Extracting it means designing what PID 1 is. Phase 7.
- **The VFS server as a process.** 57 KB, and a bootstrap loop: the loader reads executables
  *through* the VFS. Shrink its buffers (C5); do not move it.
- **Demand paging / swap.** No backing store worth paging to, and Sv39 exists on exactly one of
  three targets.
- **`fork`/`exec` semantics.** Spawn-with-argv is what an MCU needs; `fork` on a NOMMU target
  with NAPOT regions is not a good use of the phase.

---

## 5. Definition of done

| Metric | v0.8.0 | Phase 6 target |
|---|---|---|
| RP2350 static RAM | 452 KB | ≤ 300 KB |
| RP2350 heap | 60 KB | ≥ 210 KB |
| Largest placeable NAPOT region | 32 KB | ≥ 128 KB |
| Concurrent user processes | 1 | ≥ 2 |
| `cc`, `e` resident when idle | always | never |
| Commands resolved via search path | 0 | `cc`, `e`, `hello`, + |

Plus: `/proc/meminfo` continues to report the peak figures, and the hardware suite asserts the
margins rather than the sizes (as `test_memory_margins` already does).

---

## 6. Bug hunt — root causes

All three are the same story: **B4 separated the kernel log from the interactive console, and
three things were left on the wrong side of the split.**

### 6.1 `cat` output has no carriage returns

**Root cause.** `vprintk_to()` (`kernel/printk.c:78-82`) inserts `\r` before `\n` — but only for
newlines *literally present in the format string*. Bytes emitted through `%s` go to the `puts`
callback untranslated. `prim_cat()` (`user/lisp/lisp.c:302`) does `cprintf("%s\n", buf)`, so every
newline inside the file is bare LF and only the trailing one is CRLF. Exactly the reported
staircase.

**Fix.** Translation belongs on the *console stream*, and it is deleted from `vprintk_to()` in the
same change (doing either half alone gives `\r\r\n` or nothing). `/proc/kmsg` then holds raw LF,
which is what a remote node reading it over 9P wants, and every future program's output is fixed
at once rather than one `cprintf` call at a time.

> **Correction, 2026-08-10 (C0 implementation).** This section originally said to put the
> translation in `uart_putc()`/`usb_cdc_putc()` — "the terminal device", covering everything at
> once. **That is wrong and would have broken 9P.** `drivers/uart_net.c:73` sends SLIP-encoded
> frames through `uart_putc()`, so translating there inserts `0x0D` into binary protocol data and
> corrupts every frame containing a `0x0A` byte — silently, on the transport the distributed
> namespace runs over.
>
> The correct layer is `console_emit()` in `kernel/console.c`: the console *stream* is by
> definition attached to a terminal, whereas the UART is a byte pipe that sometimes carries one.
> The kernel log's console sink gets the same conversion via a shim in `kernel/main.c`, so
> `printk` and `cprintf` agree without the ring or the SLIP path being touched. Implemented this
> way; §3's C0 is complete.

### 6.2 Command output lands in `/proc/kmsg`

**Root cause.** `printk()` → the klog ring; `cprintf()` → the console. Call sites that produce
*user-requested output* but still call `printk()` put that output in the log. Confirmed:
`i2c_scan_bus()` at `drivers/i2c_rtc.c:269`. `ls` has the same class of problem via
`fs/vfs_server.c`.

**Fix.** Audit rather than patch: every `printk()` reachable from a shell verb is a candidate. The
regression test already exists in pattern form — the suite's "Log And Console Are Independent
Streams" test detaches the console sink and asserts shell output still appears. Generalise it to a
table of verbs (`ls`, `i2c`, `df`, …) and the class stays fixed.

### 6.3 Editor leaves stale lines from a previously larger file

**Two root causes, one of which is worse than reported.**

1. `redraw_box()` (`kernel/line_editor.c:97`) paints exactly as many lines as the current buffer
   has and never erases below them. Loading a 5-line file after a 10-line file leaves lines 6–10.
   **Fix:** emit `\033[J` (erase from cursor to end of screen) after the last painted line — two
   lines of code.
2. **`line_editor.c` writes via `uart_puts()` directly**, bypassing the console binding entirely
   (`kernel/console.c`'s `g_console_putc`). On RP2350 with the console bound to USB, the editor
   paints to the wrong device. This was not in the bug report and is the more serious defect.
   **Fix:** route through `console_putc()`.

### 6.4 A runaway Lisp recursion hangs RP2350 hard *(pre-existing; root-caused and fixed 2026-08-10)*

**Not one of the three reported bugs.** Found by the C0 hardware validation run, and serious
enough to have its own entry: it takes the board down hard, and only a physical replug recovers
it.

**Symptom.** `(define (loop n) (loop (+ n 1)))` then `(loop 0)` at the console. The board stops.
No `[Lisp Error]`, no fault banner, no prompt; USB CDC stops being serviced, the ports stay
enumerated but every open blocks, and the 1200-baud BOOTSEL touch no longer works. On QEMU the
identical input is caught by the depth guard and the shell carries on -- the suite's
"Lisp Recursion Depth Guard" test asserts exactly that, and passes.

Reproduced on firmware built from commit `f953876`, so it predates all Phase 6 work.

**Root cause**, in order:

1. `(loop 0)` recurses. The depth guard stops it at 100 levels -- but ~500 nodes were allocated
   getting there.
2. `NODE_POOL_SIZE` is **512 on RP2350 and 4096 on the QEMU targets** (`user/lisp/lisp.c`). So one
   recursion exhausts the pool on RP2350 and roughly one in eight does on QEMU. **This is the
   whole reason the suite never saw it**: the existing test runs the recursion once.
3. On exhaustion `alloc_node()` clamps `node_pool_idx` to the last slot. Its comment reasons
   carefully about not corrupting the environment and concludes that aliasing merely "produces
   wrong results".
4. It does more than that. Two allocations from the clamped slot are *the same node*, so
   `cell->cdr = alloc_node(...)` makes a cons cell point at itself.
5. Every list walker in the evaluator then runs forever. `prim_add()` accumulates into `sum`
   while it spins.
6. On QEMU that is a signed-overflow **UBSan trap** at `lisp.c:209`, which halts with a message.
   RP2350 builds without UBSan (it is enabled only for the QEMU targets), so the same overflow is
   plain undefined behaviour and the loop simply never ends -- a silent, unrecoverable spin. The
   pending `printk` never appears because `usb_cdc_task()` is never reached again to drain the TX
   ring, which is why the console goes quiet with no error.

**Fix.** `lisp_eval()` refuses to descend once the pool is exhausted, in the same place and the
same shape as the depth guard, so nothing is ever built out of the aliased node and evaluation
unwinds instead. The clamp stays -- allocation still has to return something writable -- but it is
no longer load-bearing. `prim_add()`'s walker additionally carries a `NODE_POOL_SIZE` iteration
bound, which is free and exactly correct: no list can have more elements than the pool has nodes.

The shell is left alive but degraded, returning nil until restart, which is what the existing
warning already promised. That is a large improvement on taking the machine down, and it is
honest about the state.

**Tests.** A QEMU regression test runs the recursion eight times to reach the state RP2350 reaches
in one, and asserts the REPL prints a result after the exhaustion warning -- getting a result at
all is the proof that evaluation unwound rather than spun. The RP2350 hardware test runs it once,
which is the stronger check, on the one target where the failure mode is silent.

**Still worth doing later**: the depth guard is calibrated such that a single runaway recursion
consumes RP2350's entire node pool. Making `LISP_MAX_EVAL_DEPTH` target-aware, or deriving it from
real stack headroom now that `_stack_bottom` and `stack_used_bytes()` exist, would stop a runaway
from costing the whole pool in the first place.

### 6.5 RP2350 was running on the wrong stack *(fixed, 2026-08-10)*

`linker/rp2350.ld` allocates a 16 KB `.stack` in main RAM and explains at length why the boot
stack must not live in SCRATCH — it gets 8 KB before running into the page allocator's heap at
`0x20080000`, and overflowing there corrupts the heap instead of faulting.

**Nothing ever loaded `sp` from it.** RP2350 enters at `_reset_handler`
(`arch/riscv/rp2350/boot_header.S`) via the bootrom and never executes `_start`, and that handler
sets `mtvec`, copies `.data`, clears `.bss` and calls `kernel_main` without touching `sp` — so the
kernel ran on the bootrom's initial SP at the top of SCRATCH_Y the entire time, with 16 KB of RAM
reserved and untouched and the linker script describing a layout that was not in effect.

Found because the boot-stack instrumentation read **100% used** on silicon against 6 KB of 64 KB
on QEMU. The paint lived in `entry.S`, which RP2350 does not run: two boot paths, one
instrumented. The paint is now a shared `PAINT_BOOT_STACK` macro in `kernel/meminfo.h` invoked
from both, and `_reset_handler` loads `sp` from `_stack_top`.

Verified on hardware: 4083 of 4096 words painted at `kernel_main` entry (13 words consumed by the
boot path), high-water **6036 bytes of 16 KB** — within 100 bytes of QEMU's 5936, which is the
cross-check that the number means something. The old arrangement was at 74% of its 8 KB before
anything unusual happened.

**On the proposal to make `e` an executable:** agreed, and it is C7 — but the fixes above are C0.
A two-line erase-to-end-of-screen fix should not wait on a multi-milestone loader refactor, and
landing them separately means C7 is a relocation with no concurrent behaviour change to debug.
"Always use the entire terminal" additionally needs a terminal size, which nothing currently
knows; either query it (`\033[999;999H\033[6n`) or make it a `/system/etc` setting — worth
deciding in C7, not assuming.

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| §2.2's arithmetic closes with ~16 KB spare — one unplanned static array and C6 stops fitting | `/proc/meminfo` already reports `_kernel_end` and the largest free run; make the hardware test assert a floor on both, so the day it stops fitting is a failing test rather than a mystery |
| Heap fragmentation makes the 128 KB region unplaceable even when 128 KB is free | Allocate large aligned regions top-down, or reserve at first `exec`; assert `Largest Free Run` in the hardware test |
| chibicc's trimmed pools silently fail on larger inputs | Pool exhaustion must be a diagnostic naming the pool, not a wrong answer; add a compile of a deliberately oversized source to the suite |
| QEMU-only validation misses a Hazard3 property, for the fifth time | Every item in §3 has a hardware assertion; C4's alignment-failure test is *unreachable* on QEMU by construction |
| The `/system/etc` migration breaks boot on an existing SD card | Fall back to the old paths for one release, with a `printk` deprecation notice |
