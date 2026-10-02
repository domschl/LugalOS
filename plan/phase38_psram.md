# Phase 38 — PSRAM on the RP2350: a second memory, and what moves into it

**Status: planned 2026-10-02; in progress.** Written from the owner's proposal of the same
day, the review of it, and the measurements in
[`plan/phase38_preliminaries.md`](phase38_preliminaries.md), which this plan
cites throughout as **[P§n]**. Decisions marked *(owner, 2026-10-02)* are
settled. The **[sign-off]** items of §6 were decided the same day; §6 records
the answers.

**Where this comes from.** SRAM has been the limit on every persona this
project has built, and phase 37's GUI made it obvious: the LCD-7 terminal
idles at 27 of 74 heap pages, peaks at 57 with two user programs resident,
and its static image is 191 KB, of which Lisp alone is 62 KB. The board has a
PSRAM that nothing uses. It turned out to be **8 MB, not the 2 MB** the board
file assumed [P§1], as fast as flash for sequential reads, and ~14x SRAM for
a random miss [P§2].

**Scope.** The RP2350 (priority 1), and everything that is not specific to
one chip: the memory classes, the allocator zone, the static section, libc,
and the consumers. The ESP32-P4 is **phase 39** (§9): its PSRAM bring-up is a
project the size of phase 34, and the new P4 board with a 7" screen may be on
the bench by then.

**Milestone scheme: `38.0`, `38.1`, …**, as phases 36 and 37.

---

## 0. The principles

1. **RAM before speed** *(owner, 2026-10-02)*. Where moving something to
   PSRAM frees SRAM at a cost that is not extreme, it moves. "Extreme" is
   judged per consumer against a measurement, not in advance. The Lisp heap
   at ~1.7x is accepted [P§3]; an SRAM nursery for it waits for a later,
   dedicated Lisp phase.
2. **The caller states a latency class; the board decides where it lands.**
   Two classes: **fast** (SRAM, always) and **bulk** (PSRAM where the board
   has it, SRAM otherwise). Nobody wraps `malloc` so that hot data lands in
   PSRAM by accident -- the Waveshare demo's design, which this rejects.
3. **PSRAM is optional per board and mandatory per persona.** A board file
   that declares PSRAM (`CONFIG_PSRAM_BYTES`) makes it a requirement of that
   build. A persona built for PSRAM that does not find it at boot halts with
   a message saying so, rather than limping on SRAM it was never sized for
   **[sign-off S1]**. Personas without PSRAM build exactly as today: bulk
   falls back to SRAM, and the `.bulk_bss` section folds into `.bss` (§3).
4. **What reports a failure stays in SRAM.** Stacks, trap frames, the
   scheduler, PMP state, the klog ring, the console path and the fatal
   handler never live in PSRAM. When PSRAM is the thing that failed, the
   board must still be able to say so.
5. **Measured before and after, on the board.** Every consumer moved in this
   phase reports its SRAM saving and its speed cost with the same instrument
   it is judged by. A move that costs more than it should is reverted, not
   argued for.

---

## 1. What the preliminaries settled

* **No swapping.** PSRAM is memory-mapped; copying between two memories the
  CPU can address directly buys nothing *(owner, 2026-10-02)*.
* **No FAT cache in PSRAM.** The slow card was algorithmic and is fixed
  without PSRAM (`38.0`, below) *(owner, 2026-10-02)*.
* **Scan-out from PSRAM is ruled out above 4 bpp** [P§5]. The panel needs
  ~21.5 Mpixel/s; PSRAM DMA gives 25-28 MB/s and shares the QMI with every
  instruction fetched from flash. On the RP2350, PSRAM is storage for the
  GUI, not the frame the panel reads. Colour remains a later phase, which
  this one makes affordable by freeing SRAM for a 2 bpp (96 KB) frame.
* **The flash path loses PSRAM data** unless the XIP cache is cleaned first,
  and leaves PSRAM unreadable unless QMI window 1 is restored after [P§4].
* **Every identity-store write already slows flash** until reboot: the ROM
  leaves M0 in serial 03h mode at CLKDIV 12 [P§4]. Fixed in this phase on
  every RP2350 board *(owner, 2026-10-02)*.
* **libc's `memcpy` is a byte loop**, 2.3x (PSRAM) to 10x (SRAM) slower than
  a word loop [P§2]. Everything that moves data into and out of PSRAM goes
  through it, so it is fixed here (it was an open issue).

---

## 2. The hardware rules

These are the rules every milestone below is checked against. They go into
`drivers/psram_rp2350.c`'s header comment, where the next reader looks.

**H1. Two aliases.** PSRAM is at `0x11000000` through the 16 KB XIP cache
and at `0x15000000` uncached. The bulk zone hands out **cached** addresses.
Bulk writers that stream (the RAM disk, DMA targets) may use the uncached
alias for speed -- 31 against 9 MB/s [P§2] -- and must then invalidate the
cached range they wrote around (`xip_cache_invalidate_range()`, by address,
op 2). Reading through the uncached alias after writing through the cached
one needs `xip_cache_clean_range()` first. Two helpers, in one place.

**H2. The flash critical section.** While the ROM has XIP turned off, PSRAM
is unreachable and the cache is flushed. Core 0 runs it with interrupts off;
core 1's PIO-USB engine runs from RAM and never touches PSRAM (a rule, now
stated in `piousb_rp2350.c`, since `check_core1_ram.py` checks code
placement, not data). DMA channels in use must not target PSRAM across it --
today only the LCD's, whose frame is SRAM. **The flash write's source
buffer must be in SRAM**: the ROM reads it with XIP off. Checked by address
in `flash_rp2350_write_sector()`, refused with a message.

**H3. Cache maintenance cost.** A full clean is 2 048 maintenance writes
(16 KB / 8-byte lines). Cheap at flash-write frequency; not something to do
per sector.

**H4. Executing from PSRAM** is XIP like flash, through the same cache.
Whether U-mode images may run from there is measured in 38.7, not assumed.

**H5. Coherence.** The RP2350 has no data cache besides the XIP cache, and
Hazard3 no instruction cache, so code written to PSRAM through the cached
alias and executed through it needs nothing beyond the `fence.i` the ELF
loader already issues.

---

## 3. The software design

### 3.1 The bulk zone

`kernel/palloc.c` gains a second zone over PSRAM:

    void *palloc_pages_bulk(uint32_t n);                  /* bulk class */
    void *palloc_pages_bulk_aligned(uint32_t n, uint32_t align_pages);
    void  palloc_free(void *p, uint32_t n);               /* either zone, by address */

* One bitmap per zone. 8 MB is 2 048 pages, a 256-byte bitmap (SRAM).
  `PALLOC_MAX_PAGES` stays the fast zone's cap; the bulk cap is its own
  board value.
* **Fallback:** a board without PSRAM has no bulk zone, and the bulk calls
  are the fast ones. A board *with* one falls back to SRAM when the bulk zone
  is exhausted, and `/proc/meminfo` counts the fallbacks -- a number that
  should stay 0, and says so when it does not.
* `palloc_free()` finds the zone from the address. Freeing a bulk page into
  the fast bitmap (or the other way round) is the bug this makes impossible.
* NAPOT alignment works in the bulk zone exactly as in SRAM: PMP checks
  addresses, not memory types.
* `/proc/meminfo` gains a line per zone: total, free, peak, largest run.
* **QEMU** gets a bulk zone too, carved from its 128 MB as a separate range,
  so the suite runs the zone code, the by-address free and the fallback.

### 3.2 The `.bulk_bss` section

Large static arrays (Lisp's pools today) are not allocated at run time and
should not have to be. A section attribute marks them:

    #define BULK_BSS __attribute__((section(".bulk_bss")))
    static lisp_val_t node_pool[NODE_POOL_SIZE] BULK_BSS;

* On a board with PSRAM, `linker/rp2350.ld` places `.bulk_bss` (NOLOAD) in a
  `PSRAM` memory region at `0x11000000`; the bulk zone starts after it. GNU
  ld cannot choose a region conditionally, so CMake generates a one-line
  include naming the region (`PSRAM` or `RAM`) per board. On every other
  target `.bulk_bss` lands in RAM next to `.bss` and nothing changes.
* It is zeroed by the PSRAM bring-up (38.2), after the chip is in QPI and
  before anything reads it -- the reset handler zeroes `.bss` long before.
* `tools/sizereport.py` reports `.bulk_bss` separately, so SRAM growth and
  PSRAM growth are two baselines and neither hides the other.
* The ASSERTs that keep the heap floor honest keep working: `.bulk_bss`
  leaving `.bss` is what raises the SRAM heap.

### 3.3 Which class, by consumer

| Consumer | Today | Class after 38 | Why |
|---|---|---|---|
| Lisp node pool, string pools, GC mark stack | 62 KB static SRAM | bulk (`.bulk_bss`), larger (38.5) | measured 1.7x, accepted [P§3] |
| Lisp mark bits | SRAM bitmap | fast | random bit access; small |
| `/ram0` | SRAM pages, ≤ 512 KB | bulk, MBs (38.6) | sequential: PSRAM's best case |
| Editor buffer, undo | SRAM pages | bulk (38.7) | human-speed access |
| chibicc arena | SRAM pages | bulk (38.7) | compile speed measured |
| U-mode program images | SRAM, NAPOT | bulk if H4 holds (38.7) | removes the 128 KB contiguous limit |
| Chess TT | SRAM pages | measured (38.7) | random access; "extreme" is possible |
| Chess search pools, perft lists | SRAM pages | fast | the inner loop |
| Canvas backing store | none | bulk (38.8) | new, only possible with PSRAM |
| LCD frame, line table | SRAM | fast | DMA scan-out [P§5] |
| Stacks, klog, scheduler, PMP, trap state | SRAM | fast | §0 principle 4 |
| VFS handle `proc_buf`s, 9P/SLIP buffers | SRAM static | fast, for now | candidates for a later pass; not worth the risk here |

---

## 4. Resources

| Item | SRAM | PSRAM |
|---|---|---|
| Bulk zone bitmap (2 048 pages) | +256 B | — |
| Saved XIP setup function (38.1) | +256 B | — |
| PSRAM driver state | < 64 B | — |
| Lisp pools, mark stack (38.5) | **−~60 KB** static | +~1.6 MB at the sizes of [sign-off S2] |
| `/ram0` (38.6) | −its size, when mounted | +2 MB [sign-off S3] |
| Editor, chibicc, U-mode images (38.7) | −their peak, on demand | +the same |
| Canvas backing store (38.8) | 0 | +48 KB per stored screen |

The static saving alone is ~15 heap pages on the LCD-7: from 74 to ~89. The
on-demand consumers move the *peak* (57 pages with two user programs) much
further than that.

---

## 5. Milestones

The order: the flash fix first, because it is a live bug on every RP2350
board and PSRAM bring-up depends on it. Then the chip, the shared plumbing
(libc, zone, section), then the consumers from safest to most
latency-sensitive, and last the spending of what was freed.

### 38.0 — Preliminaries: FAT32 fixed, PSRAM measured *(done 2026-10-02)*

* FAT32 without the quadratic walks (commit `f36aa25`): FSInfo free count,
  allocator hint, per-handle cluster cursors, a cached FAT sector for /sd0,
  a volume lock, `df -r`. `df` 10.3 s → 0.1 s; `sdbench w 1024` 6/11 →
  62/349 KB/s. Fixed on the way: multi-sector-cluster corruption in
  `fat32_write_file()`, clustered empty files, statfs' off-by-two.
* The PSRAM measured on branch `phase38-psram-spike` and written up in
  `plan/phase38_preliminaries.md`.

### 38.0b — `open_issues.md` sorted *(owner, 2026-10-02; done 2026-10-02)*

The owner's observation: the file holds two different classes of entry.
Sort it into **actionable** (a known fix, deferred for priority -- each
gets a line naming the phase or milestone that will take it, or "unscheduled"),
**unexplained intermittents** (each gets "what evidence would settle it"),
and delete the FIXED/CLOSED entries per the file's own rule, moving the two
or three kept as lessons into a short closing section. No code.

**Done when:** every entry is in one of the two classes and the actionable
ones each name a destination.

**Done.** 44 headings became 16 actionable entries (one new: the runner's
blindness to a stuck guest, gathered from the harness halves of three
intermittents), 12 unexplained intermittents, 3
"deliberate limits that look like faults" (a third class the sort turned up:
by design, listed because each once cost an investigation) and 3 lessons;
the rest were FIXED/CLOSED and deleted. Destinations: the USB serial number
→ 38.1, libc → 38.3, `sizereport`'s blind spot for `.data` → 38.4, the chess
spread → 38.9, eleven items → the new
[`plan/phase40_backlog.md`](phase40_backlog.md) (harness first), the
DS3231's cell → the owner's bench.

### 38.1 — Flash writes keep XIP fast, and keep PSRAM intact (every RP2350 board)

`drivers/flash_rp2350.c` does what pico-sdk's `flash.c` does, in order:

1. At boot, copy the XIP setup function the bootrom leaves in the first
   256 bytes of boot RAM (`0x400e0000`) into SRAM (`.ramfunc`-adjacent, so
   it runs with XIP off). **First, verify on the board** that boot RAM holds
   RISC-V code at that point and that calling it restores the M0 registers
   observed at boot (RFMT 0x000492a8, RCMD 0xeb, TIMING 0x60007203 on the
   LCD-7) -- pico-sdk's comment says the copy works on RISC-V; this tree
   checks.
2. Before the ROM sequence: clean the XIP cache (H3).
3. Save QMI M1 TIMING/RFMT/RCMD; after `flash_enter_cmd_xip`, call the
   saved setup function (M0 back to quad continuous read), then restore M1.
4. Refuse a source buffer outside SRAM (H2).
5. *(from `open_issues.md`, 38.0b)* The USB serial number from the chip's
   unique ID instead of `LUGALOS-0001` on every board -- this milestone's
   tests are the first to attach two RP2350 boards at once, which is when
   the shared `/dev/serial/by-id` name sends a flash to the wrong board.

`clocks` grows a QMI line (M0 and M1 TIMING/RFMT/RCMD) -- the instrument.

**Tests:**
* `test_rp2350.py`: read the QMI line, do an `identity name` write, read it
  again: M0 unchanged. Run on the LCD-7 and (owner to attach) the Pico 2
  chess board, since every persona writes the identity store.
* Before/after on the LCD-7: `psram xip` from the spike shows M0 at 03h
  today; after 38.1 the same sequence leaves it fast.
* The 46 % slowdown measured after a flash write [P§4] is gone: a flash-
  bound benchmark (`perft 3` on the chess persona) is the same before and
  after an identity write, within its noise.

### 38.2 — PSRAM bring-up at boot

`drivers/psram_rp2350.c` becomes the driver, starting from the spike's
direct-mode code [P§7] and losing its bench commands to 38.2's `psram`:

* Board file: `CONFIG_PSRAM_CS_GPIO 0`, `CONFIG_PSRAM_BYTES 8388608`; the
  board file's header comment ("2 MB PSRAM") corrected.
* Early in `kernel_main()`, before `palloc_init()`: GP0 → XIP_CS1, ID read,
  reset, QPI, M1 timing derived from `CONFIG_CLK_SYS_HZ` (a `_Static_assert`
  that SCK stays ≤ 133 MHz), `WRITABLE_M1`, size by aliasing checked against
  the board file, `.bulk_bss` zeroed.
* A persona that requires PSRAM and does not find it: halt with the reason
  on the console and on the panel **[sign-off S1]**.
* `boardprobe`'s GP0 check now expects XIP_CS1 where the board has PSRAM.
* `psram` shell command: status (ID, size, clock, zone use), `psram test`
  (pattern test over the *free* bulk pages only), `psram bench` (kept, as
  `sdbench` is: the instrument for every later milestone).
* `/proc/meminfo` and `/proc/config` report it.

**Tests:** `test_rp2350.py`: PSRAM present, 8 MB, `psram test` clean,
`psram bench` within 10 % of [P§2]. Cold boot and warm reset both (a warm
reset finds the chip already in QPI -- the spike's exit-QPI first step).

### 38.3 — libc moves words, not bytes

`libc/string.c`: `memcpy`, `memmove` and `memset` word-wide when both
pointers allow it, with byte heads and tails; `memmove` correct in both
overlap directions. Built `-fno-builtin` as today, so the compiler does not
turn the loops back into calls to themselves.

**Tests:** a QEMU selftest over every alignment pair 0-3 × length 0-67 plus
a large copy, both overlap directions, against a byte reference. On the
LCD-7, `psram bench`'s memcpy lines: SRAM → SRAM from 20 MB/s towards the
word loop's ~200, PSRAM → SRAM from 9.6 towards ~22 [P§2].

### 38.4 — The bulk zone and `.bulk_bss`

§3.1 and §3.2, with no consumer moved yet:

* `palloc_pages_bulk()`, `_aligned()`, by-address `palloc_free()`, zone
  stats, fallback counter.
* The `PSRAM` region in `linker/rp2350.ld` via the generated include;
  `.bulk_bss` in RAM on every other target.
* `sizereport` splits `.bulk_bss` out; the baselines gain that column. It
  counts by section (`readelf -sW`) rather than by `nm` type, which also
  closes `open_issues.md`'s blind spot: RP2350's `.data` symbols type as `t`
  today and are not counted at all.
* `xip_cache_clean_range()` / `xip_cache_invalidate_range()` (H1).

**Tests:** QEMU: allocate and free across both zones, NAPOT alignment in
the bulk zone, exhaustion falling back to SRAM and being counted, a bulk
page freed by address. On the LCD-7: the same through a `psram` subcommand,
and `/proc/meminfo` showing both zones.

### 38.5 — The Lisp heap moves to PSRAM

* `node_pool`, `string_small`, `string_large` and `gc_work_stack` become
  `BULK_BSS`; the mark bits stay in SRAM.
* Sizes on the LCD-7 **[sign-off S2]**: recommended 65 536 nodes (1 MB),
  string pools ×8, the mark stack sized to the pool (it is PSRAM now, so
  "bounded" stops being urgent; it remains a note for the later Lisp phase).
  The mark bits are then 8 KB of SRAM -- or move them too, after measuring.
* Other personas: unchanged sizes, unchanged placement (`.bulk_bss` is
  `.bss` there).

**Measured, on the LCD-7, against [P§3]:** `(fib 18)` and the allocation
loop (expect ~1.7x today's), the **longest GC pause** at the full pool
(a sweep over 65 536 nodes in PSRAM -- interactivity, Ctrl-C latency), and
the SRAM regained (`sizereport`, `meminfo`).

**Tests:** the QEMU Lisp suite unchanged; `test_rp2350.py`'s node-pool
exhaustion test (it must still exhaust, now later, and still degrade
cleanly -- its timeout may need to grow with the pool).

### 38.6 — `/ram0` in PSRAM

* `ramdisk.c`'s storage from the bulk zone; `RAMDISK_MAX_BLOCKS` a board
  value. On the LCD-7, 2 MB mounted at boot **[sign-off S3]** -- `cc`'s
  multi-file builds already use `/ram0`.
* Reads through the cached alias; writes through the uncached alias plus an
  invalidate of the written range (H1) -- measured against plain cached
  writes, and kept only if faster.

**Measured:** `sdbench w` generalised to take a path (`fsbench /ram0 1024`),
before (SRAM, at the size SRAM allows) and after.

### 38.7 — On-demand consumers: editor, `cc`, U-mode images, chess TT

Each is a one-line class change (`palloc_pages` → `palloc_pages_bulk`) with
its own measurement; each is kept or reverted on its own number:

| Consumer | Measure | Keep if |
|---|---|---|
| Editor buffer, undo (`kernel/editor.c`) | load/save/search a 200 KB document | no visible lag (typing, scrolling) |
| chibicc arena (`user/chibicc/pools.c`) | `cc` of the suite's multi-file program | compile ≤ 2x today |
| U-mode images (`arch/riscv/common/elf.c`) | a compute-bound user program, SRAM vs PSRAM | H4 holds and ≤ 2x |
| Chess TT (`user/chess/src/tt.c`) | `perft`/search node rate with TT | the owner's call on the number **[sign-off S4]** |

U-mode images are the largest win (the 57-page peak is mostly two
programs) and the largest risk: the first test is that code runs from the
M1 window at all under PMP (H4), on the spike branch, before the class
changes. If it does not, images stay in SRAM and the bulk zone still gives
them their *data* pages.

**Tests:** `test_rp2350.py`'s C2 (two programs resident) and C4 (multi-page
image, W^X) on bulk-zone images; `meminfo` peak with two programs, before
and after.

### 38.8 — A canvas backing store

Phase 37 §6 item 5 left the door open: "with PSRAM, `lcdterm` can keep a
copy and send fewer redraws". The 1 bpp frame is 48 KB; a stored copy of a
screen is 48 KB of PSRAM.

* `lcdterm` keeps one stored screen per layout (48 KB each): the pixels of
  a layout when another replaces it (the editor over a Lisp drawing, a help
  screen over the chess board), restored on return without a redraw
  message. Applications still handle redraw -- phase 37's contract is
  unchanged, this only makes it rarer.
* `lcdterm` is a U-mode domain; it gets one more PMP region (the bulk page
  run holding the stores). The PMP budget is checked in the milestone.

**Scope [sign-off S5, decided]:** one stored screen per layout, so that
switching between layouts restores each without a redraw. The window system
itself grows in a later phase; colour is its own later phase.

### 38.9 — Spending what was freed: the chess hot path

`plan/open_issues.md`'s "chess speed depends on where the linker puts it
(2.3x spread)": option (a), the hot search files in `.ramfunc` and their
tables in `.data`, ~31 KB, which the issue parked because "the fix spends
heap". After 38.5 there is heap to spend.

**Measured:** `(perft 3 1)` on three builds that differ only in unrelated
code: the spread must collapse (today 7.0-17.0 s) and the speed match or
beat the best of today's builds. On the chess persona (no PSRAM) the same
change costs 8 pages it may not have -- applied to the LCD-7 persona only,
unless the owner wants the chess board too.

### 38.10 — Documents

README (the LCD-7 section: 8 MB PSRAM, the memory classes), the per-board
memory table, `plan/hardware_seams.md` (the QMI's second window as a seam),
open issues closed by this phase deleted, and this file's status line.

---

## 6. Sign-off items

Decided by the owner, 2026-10-02:

* **S1** — A persona built for PSRAM that does not find it at boot: **halts
  with a message.**
* **S2** — Lisp sizes on the LCD-7: **65 536 nodes (1 MB), string pools ×8.**
* **S3** — `/ram0` on the LCD-7: **2 MB, mounted at boot.**
* **S4** — Chess TT in PSRAM: deferred to 38.7's node-rate number.
* **S5** — 38.8's scope: **one stored screen per layout.** The window system
  grows in a later phase; colour is a dedicated later phase of its own.

---

## 7. Risks

* **The bootrom's XIP setup function is not where pico-sdk expects it** on
  this boot path (we boot a RISC-V image without boot2). 38.1 checks before
  relying on it; the fallback is reconstructing the M0 settings observed at
  boot and re-entering continuous-read mode by hand, which is what the boot
  RAM function does internally.
* **Something touches PSRAM inside the flash critical section** -- an
  interrupt that should have been masked, a future DMA user. The symptom is a
  hang during an identity write. H2 states the rule; 38.1's test exercises
  the write with PSRAM in active use (a Lisp program running).
* **Executing from PSRAM under PMP fails or is slow** (H4). 38.7 tests it
  first; the fallback is clear.
* **GC pauses become noticeable** with a 1 MB pool in PSRAM. Measured in
  38.5; the answer, if needed, is a smaller default pool, not a new GC in
  this phase.
* **`.bulk_bss` hides SRAM growth** if the sizereport split is wrong.
  The split is the first thing 38.4 tests.
* **Warm reset finds the chip in QPI**; the spike handles it (exit QPI
  first) and 38.2 tests both reset kinds.
* **Two QMI windows share one 16 KB cache.** PSRAM traffic evicts kernel
  code. [P§3]'s Lisp numbers include that effect; 38.9's perft numbers are
  taken with and without a PSRAM-heavy task running.

---

## 8. Not in this phase

* **Colour.** Freed SRAM makes a 2 bpp frame affordable; the palette, the
  PIO program and `lcdterm`'s colour model are their own phase.
* **An SRAM nursery / generational GC for Lisp** -- a later Lisp phase
  *(owner, 2026-10-02)*.
* **Network and VFS buffers to PSRAM** (§3.3, last row): a later pass, once
  the zone has a track record.
* **PSRAM on other RP2350 boards.** None in this project has one fitted;
  the board-file key makes adding one a board-file change.

---

## 9. Phase 39 — PSRAM on the ESP32-P4 (outline only)

Not elaborated until it starts. What is known now:

* **Why it is needed.** The ordered ESP32-P4-WIFI6-Touch-LCD-7B has a
  1024 × 600 MIPI-DSI panel (EK79007); one RGB565 frame is 1.2 MB, more
  than the P4's whole 768 KB L2MEM. Its display cannot exist without PSRAM.
  If that board is on the bench, phase 39 is done on it; the P4-NANO has the
  same in-package PSRAM and serves otherwise.
* **The bring-up** (ESP-IDF's, to be re-derived, ~1 500 lines):
  MPLL (never started in this tree; IDF runs PSRAM from it at
  320-500 MHz), the MSPI2/3 PSRAM controller (`psram_ctrlr_ll.h`), the AP
  hex-PSRAM mode registers (`esp_psram_impl_ap_hex.c`), timing tuning
  (`mspi_timing_tuning`), ECC (optional), and the cache MMU mapping of the
  PSRAM window. The samples run PSRAM at 200 MHz.
* **The L2 cache trade.** PSRAM performance depends on the L2 cache size,
  which phase 27 found is carved out of L2MEM: a bigger cache is a smaller
  heap. Measured, not guessed.
* **What it reuses from phase 38:** the bulk zone, `.bulk_bss`, the latency
  classes, the libc fixes, every consumer's class decision -- the P4 adds a
  backend and re-measures.
* **First milestone:** the same bench as [P§2] on the P4, before any
  consumer moves. The display (MIPI-DSI) is the phase after.
