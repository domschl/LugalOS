# Phase 9 — Chess computer (ST7735 canvas + TM1638 I/O + engine port)

**Status:** H1-H4 complete and hardware-verified, 2026-08-11. Phase closed
except for the deliberately-deferred item below (UCI/GUI bridge).
First entry from `plan/raw_ideas.md`'s "Application scenarios" with a concrete,
already-built consumer (`~/gith/domschl/LugalChess`) rather than being purely
speculative, and it also closes two "New hardware" backlog items (Displays,
Keyboard support) as a side effect, by giving them a real driving use case
instead of building them speculatively.

**Note on H0's research basis:** H0 below was researched against a since-updated
snapshot of `~/gith/domschl/LugalChess` that predated the project's own RISC-V
port (commits `a9dc5f5`..`3208936`, "Add RISC-V option" through "Support perft
for embedded"). The re-check found the core findings held (pin layout, malloc
footprint, stdio footprint all unchanged) and two things got *better* than H0
assumed: the engine now uses a dedicated `LUGALCHESS_EMBEDDED` macro (defined
correctly for LugalOS's bare-metal build for free, since its guard condition —
`defined(__riscv) && !defined(__linux__) && ...` — fires on any freestanding
RISC-V cross-compile, no extra define needed) rather than requiring the
`CONFIG_BOARD_RP2350` patch H0 originally planned; and `search.c`'s `MoveList`
stack-overflow risk H0 flagged was already fixed upstream (hoisted into static
per-ply arrays) by the time this phase started implementing. H0's text below is
left as originally researched since its conclusions still hold; this note is
the correction record.

**Theme.** The user's own three-step shape is right and this plan keeps it, refined
into four milestones with the platform gaps this research actually found made
concrete. `~/gith/domschl/LugalChess` targets the Pico SDK on RP2350's **ARM**
Cortex-M33 core; LugalOS's RP2350 target is the **RISC-V Hazard3** core, bare-metal,
no pico-sdk. So this is not a binary port — it's recompiling clean, portable C11
against LugalOS's own bare-metal register access and IPC/shell primitives, the same
shape every other RP2350 driver in this tree already takes (`drivers/uart_rp2350.c`,
`drivers/spisd_rp2350.c`). The engine (`engine/*.c`, ~4.9K lines) turned out to be
substantially more portable than the firmware glue (`firmware/*.c`, ~1K lines) —
most of the real work is in a handful of specific gaps, not a wholesale rewrite.

---

## H0 — Feasibility: pins, peripherals, memory, stack (research done, 2026-08-11)

**Pin/peripheral conflicts: none.** LugalChess's board wiring already avoids every
pin LugalOS's own `cmake/board-rp2350.cmake` claims today:

| Claimed today | Pins | LugalChess wants |
|---|---|---|
| UART0 | GP0/GP1 | — |
| SPI1 (SD card) | GP10-13 | — |
| LEDs | GP16, GP25 | — |
| *(free)* | GP6, GP7, GP8 | TM1638 (STB/CLK/DIO) |
| *(free)* | GP17-21 | ST7735 (CS/SCK/MOSI/DC/RST) |

Better than the user's "shared bus with the SD card" guess: RP2350 has **two**
independent hardware SSP/SPI peripherals, `SPI0` (`0x40080000`) and `SPI1`
(`0x40088000`, already `CONFIG_SPI1_BASE`, the SD card) — confirmed against
`~/gith/pico/pico-sdk/src/rp2350/hardware_regs/include/hardware/regs/addressmap.h`.
The display gets its own real hardware SPI0, not arbitrated time-sharing with the
SD card's SPI1 — no C8 one-wire-one-owner contention to design around here, because
there is no contention. GPIO function-select values: `GPIO_FUNC_SPI = 1`,
`GPIO_FUNC_SIO = 5` (from `io_bank0.h` in the same SDK checkout), matching what
`spisd_rp2350.c` already writes for SPI1 — the SPI0 pin setup is the same pattern,
different base address and pin numbers. (The SD-card slot physically present on
that board's TFT module is unused by LugalChess's own firmware — nothing to port
or conflict with there.)

**Memory allocator gap.** LugalOS has **no general `malloc`/`free`** anywhere in
the tree — only page-granular `palloc_pages()` plus each subsystem's own arena
(the heap-on-demand pattern the 2026-08-11 `e`/`cc`/`ed` fixes established). Good
news: the engine only calls `malloc`/`free` in **one place**, `tt.c`'s transposition
table (2 call sites total — everything else in the ~4.9K-line engine is
static/stack allocated). This doesn't need a general allocator port, just one
`palloc_pages()`-backed buffer sized for the table.

**The `__arm__`/`PICO_BOARD` ifdef gate is a real trap, not cosmetic.**
`engine/src/tt.c` and `engine/src/console.c` gate embedded-vs-desktop behavior on
`#if defined(__arm__) || defined(PICO_BOARD)`. LugalOS's RP2350 build defines
**neither** (RISC-V target, no pico-sdk) — so ported as-is, `init_tt()` would fall
through to the *desktop* branch and try to allocate a **16 MB** transposition table
against a board whose entire managed heap is `CONFIG_PALLOC_MAX_PAGES = 128` pages
(512 KB, `cmake/board-rp2350.cmake`). This must be caught before first boot, not
found by a crash: add `|| defined(CONFIG_BOARD_RP2350)` (already generated,
K0/phase7) to that gate rather than spoofing `PICO_BOARD`. `console.c` has ~19
more sites on the same gate, each needs individually auditing during H4 rather
than assumed safe by association — several are likely raw `hardware/flash.h`
sector writes for settings persistence, which don't port at all (LugalOS has its
own FAT32 volumes for that) and want rewriting against `vfs_*`, not re-gating.

**stdio gap, sized precisely rather than assumed.** Across `engine/*.c`:
`printf`/`fprintf`: ~150 call sites; `scanf`/`sscanf`: **1** site
(`console.c`); `fgets`/`getchar`/`stdin`: **4** sites. LugalOS has no libc stdio
at all (no `FILE*`, no `stdin`, no scanf family) but does have `cprintf()`
supporting `%c %s %d %i %u %x %X %p %%` — no float, no length modifiers. Most
`printf` call sites can become `cprintf` directly; the exception found is
`search.c`'s UCI `info depth %d ... nps %.0f ... nodes %ld pv` line, which needs
either extending `cprintf` with `%f`/`%ld` or hand-editing that one call site (it's
UCI-GUI-only output, not needed for on-device play — see H4/stretch below). The 5
combined `scanf`/`fgets` sites are few enough to hand-port against a small
line-input helper.

**Stack usage is a real, specific risk — not hand-waved.** `pv_search()` and
`quiescence()` in `search.c` both recurse and both keep a `MoveList list;`
(`uint16_t moves[256]` + count, ~516 bytes) as a **stack-local**, not a pooled
per-ply buffer. Default `max_search_depth = 64`, plus quiescence extension beyond
that. Worst case that's tens of KB of recursion stack; LugalOS task stacks are
`TASK_STACK_PAGES = 2` (`kernel/include/kernel/sched.h:42`) = **8 KB**. Three ways
to close this, cheapest-and-most-robust listed first:
1. Hoist `MoveList` out of the recursive frame into a small array indexed by
   `ply` (bounded by `max_search_depth`), allocated once — a ~15-line patch to
   `search.c`, removes the risk regardless of stack size chosen.
2. Run the search in kernel context on a purpose-sized `palloc_pages()` stack (see
   H4's placement decision below) rather than a fixed 8 KB task stack.
3. Lower `max_search_depth` for the embedded build. Weakest option — caps engine
   strength for a reason unrelated to chess.

**RAM/flash budget: comfortable.** ST7735 driver (`firmware/st7735.c`) streams
pixels directly over SPI with small local buffers — **no framebuffer**, so no
32 KB RGB565 buffer tax. Bitboard position/zobrist/move-gen state is a few hundred
bytes total. Current RP2350 image is ~654 KB of a 4 MB flash budget (K2/K3,
phase7) — plenty of headroom for ~6K more lines of engine + driver code.
Floating point (`movegen.c`: 8, `search.c`: 4, `perft.c`: 3 sites) compiles fine
without a hardware FPU via libgcc's soft-float routines (already linked,
`target_link_libraries(lugalos.elf PRIVATE gcc)`) — slower, not blocking; worth a
one-time check that it isn't in evaluation's hot path once ported, not before.

**Placement decision this phase should make explicitly.** `cc`, `ed`, and `lisp`
itself all run synchronously in kernel/M-mode context today (phase5/6/8) — not as
PMP-isolated U-mode processes. The chess engine should follow the same shape: a
new shell command / Lisp primitive, not a new U-mode ELF. That sidesteps the
syscall-boundary and PMP-isolation questions entirely for v1 and keeps this
phase's scope to "drivers + engine port," matching how `cc`/`ed` were scoped in
phase8 rather than reopening the process model.

**Remaining unknowns, deliberately left for H1/H2 rather than guessed here:**
physically confirming the user's actual board wiring matches
`firmware/tm1638.h`/`st7735.h`'s documented pins (a schematic reading, not a
software question); and there is **no QEMU model** for SPI0, ST7735, or TM1638 (same
category as SPI1/SD, phase7's own K2 note) — every milestone below is hardware-only,
verified through `tests/hw/`, the same tooling phase7/8 already use for
board-only code.

---

## H1 — Canvas primitive: ST7735 over real hardware SPI0 *(done, 2026-08-11 — hardware-verified)*

`drivers/st7735_rp2350.c` + `drivers/include/drivers/st7735.h`: bare-metal PL022
driver at `SPI0_BASE`, same raw-register shape as `spisd_rp2350.c` (own
SSPCR0/SSPDR/SSPSR poking, no pico-sdk), vendored from `firmware/st7735.c`'s init
command sequence and generic drawing primitives (chess-board rendering and piece
bitmaps stayed behind, per this section's original scope). New pin fields
(`CONFIG_SPI0_BASE`, `CONFIG_ST7735_{SCK,MOSI,CS,DC,RST}_GPIO`) added to
`cmake/board-rp2350.cmake` through the existing K0 generator, and reported via
`/proc/config` (K3's pattern) alongside the existing SPI1/LED fields — `tests/hw/
test_rp2350.py`'s `test_board_config` extended to assert all 6 new fields.

The drawing API (`draw_pixel`/`draw_rect`/`draw_bitmap_mono`/`draw_char`/
`draw_string`/`fill_screen`) is exposed to Lisp as `canvas-fill`, `canvas-pixel`,
`canvas-rect`, `canvas-text` (`user/lisp/lisp.c`, using the existing `arg_int()`
helper), all gated `#if defined(CONFIG_BOARD_RP2350)`. `st7735_init()` runs eagerly
at boot (`kernel/main.c`, after `dev_probe_all()`) rather than lazily on first
canvas call — this hardware is a fixed, always-present board persona, not
optional/hot-pluggable like the SD card, so it doesn't fit `spisd_get_device()`'s
lazy pattern.

**A real, hardware-only finding this milestone produced:** the vendored MADCTL
init value (`0x36` command, `0xC8`) swapped red and blue on the user's physical
panel. Diagnosed from the *symmetry* of the failure — a green fill (RGB565 bits
confined to the middle field) and a white fill (all bits set) both rendered
correctly, while a pure-red fill (top bits only) rendered blue — which is the
signature of the panel's RGB/BGR sub-pixel-order bit (MADCTL bit 3) being wrong
for this unit, not a systematic byte-order bug (which would have broken green
too). Fixed by clearing that bit (`0xC8` -> `0xC0`); documented in the driver as
a per-unit hardware property, since nominally identical ST7735 breakout boards
from different batches wire this either way and it isn't derivable from the
datasheet. A direct instance of [[falsify_on_hardware_not_qemu]]'s point — there
is no QEMU model for this peripheral at all, so this could only have been found
by a human looking at the actual board.

**Verified:** disassembly-checked constants (SPI0 base `0x40080000`, `IO_BANK0_CTRL`
offsets for GP17/GP18 spot-checked against `CONFIG_ST7735_{CS,SCK}_GPIO`) before
hardware access; all three targets build clean, 181/181 QEMU tests unchanged
(this driver isn't compiled for QEMU targets at all). Hardware: flashed via
`tests/hw/flash.py --verify`, then `tests/hw/test_rp2350.py` — **15/15 passed**,
including the extended `K3: /proc/config` (17/17 fields, up from 11). Live canvas
exercise over the console (`(canvas-fill ...)`, `(canvas-rect ...)`,
`(canvas-text ...)`) confirmed visually on the physical panel: correct red
background, green rectangle, white text, after the MADCTL fix above.

## H2 — TM1638: 7-segment + 4x4 keypad *(done, 2026-08-11 — hardware-verified)*

`drivers/tm1638_rp2350.c` + `drivers/include/drivers/tm1638.h`: near-direct port
of `firmware/tm1638.c`/`.h` — it's a simple bit-banged 3-wire protocol on raw
SIO GPIO (matching `drivers/uart_rp2350.c`'s LED handling: `IO_BANK0_CTRL`
func-select 5, `SIO_GPIO_OUT_SET/CLR/OE_SET/OE_CLR`), no hardware peripheral
dependency. Lisp primitives: `tm-display`, `tm-set-leds`, `tm-get-key`. New
`CONFIG_TM1638_{STB,CLK,DIO}_GPIO` fields, same K0 generator pattern as H1,
reported via `/proc/config`, asserted in `tests/hw/test_rp2350.py`'s
`test_board_config`.

**A real regression this milestone produced, found by the hardware suite
itself:** RP2350's Lisp node pool (`NODE_POOL_SIZE = 512`, a fixed static array,
much smaller than QEMU's 4096 since it sits outside `palloc`'s managed pages)
was already close to the ceiling a full `tests/hw/test_rp2350.py` run reaches
within one boot session — there's no GC, so allocations across the whole suite
accumulate. H1+H2 together added 7 new global primitive bindings
(`canvas-fill/-pixel/-rect/-text`, `tm-display/-set-leds/-get-key`), and that
was enough to tip a full suite run into `[Lisp Error] Node pool exhausted!`
partway through — reproduced twice from a clean flash, not a fluke. Fixed by
raising `NODE_POOL_SIZE` to 768 for RP2350 (`user/lisp/lisp.c`); the pool is a
few KB of static RAM against 520 KB of physical SRAM, so there was plenty of
room, it just hadn't been grown since phase7/8. Re-ran the full suite twice
after the fix with no recurrence.

**Verified:** all three targets build clean, 181/181 QEMU unchanged. Hardware:
`tests/hw/flash.py --verify`, then `tests/hw/test_rp2350.py` — **15/15 passed**,
twice in a row (`K3: /proc/config` at 20/20 fields). Live exercise over the
console: `(tm-display "LUgAL CH")` and `(tm-set-leds 170)` visually confirmed on
the physical 7-segment/LED module by the user ("Lugal Ch on display"); `(tm-get-key)`
polled for 8 s while the user pressed keys returned real, varying non-idle
values (7, 6, 5, 4, 5) rather than sticking at -1, confirming the bit-bang read,
phantom-key rejection, and matrix decode all work end-to-end on real silicon.

## H3 — `/proc/config` + feature-flag wiring *(done, 2026-08-11 — hardware-verified)*

Three independent flags, not two — revised from this section's original plan at
the user's explicit request: the TM1638 keypad/7-segment/LEDs are useful on
their own in a board persona with no display attached, so bundling it with the
canvas driver would have been wrong. `LUGALOS_ENABLE_DISPLAY`,
`LUGALOS_ENABLE_TM1638`, `LUGALOS_ENABLE_CHESS` — CMake options following
phase8's F0-F3 mechanism exactly: option -> generator input ->
`CONFIG_ENABLE_*` define -> gated `SOURCES` + registration sites. Each of
`CMakeLists.txt`, `cmake/gen_config.cmake`, `kernel/main.c`, `user/lisp/lisp.c`,
and `fs/vfs_server.c`'s `/proc/config` block now has independent `#if
CONFIG_ENABLE_DISPLAY` / `#if CONFIG_ENABLE_TM1638` guards rather than one
shared `#if defined(CONFIG_BOARD_RP2350)`. `CONFIG_ENABLE_CHESS` is defined now
with no consumer yet — same shape F0 defined `ENABLE_CC`/`ENABLE_ED` before
F1/F2 had anything to gate; H4 is what makes it real.

**Verified:** default build (all three ON) unaffected — all three targets build
clean, 181/181 QEMU tests unchanged, 15/15 hardware tests pass. Independence
proved on real hardware, not just by inspection: flashed a `-DLUGALOS_ENABLE_TM1638=OFF`
build and confirmed live — `/proc/config` reports `ENABLE_TM1638=0` and omits
the TM1638 pin fields, `(canvas-fill ...)` still works, `(tm-display ...)` comes
back `Unbound symbol: tm-display`; then the inverse
(`-DLUGALOS_ENABLE_DISPLAY=OFF`) — `ENABLE_DISPLAY=0`, SPI0/ST7735 pins omitted,
`(tm-display ...)` still works, `(canvas-fill ...)` unbound. Board reflashed
back to the full default build afterward.

## H4 — Engine port, feature-gated behind `CONFIG_ENABLE_CHESS` *(done, 2026-08-11 — hardware-verified)*

Landed differently from the original plan in one deliberate, load-bearing way:
`engine/src/console.c` (1637 lines, stdio/UCI/menu-system deeply intertwined)
was **not vendored at all**. It was going to cost ~150 hand-edited `printf`
call sites and ~1600 lines for behavior (UCI protocol, flash save/load, level
menus) already out of scope for this phase. Instead, `user/chess/src/chess_ui.c`
is a fresh, small (~330-line) implementation against the exact extension point
`search.c` already calls through — `search_progress_callback()` /
`search_poll_stop_callback()`, declared `extern` there and never defined there,
upstream's console.c-only definitions. Everything else (`bitboard`, `position`,
`movegen`, `zobrist`, `evaluation`, `tt`, `search`) is vendored under
`user/chess/{include,src}/`, mirroring `user/chibicc/`'s layout.

Also landed with less required adaptation than H0 estimated, since two of its
concerns had already been resolved upstream by the time implementation started
(see this file's top-of-document correction note) or turned out to need a
different fix than expected:

1. **`tt.c`'s `malloc`/`free`** -> `palloc_pages(8)`/`palloc_free(...)`, exactly
   as planned (`LUGALCHESS_EMBEDDED` always requests a fixed 32 KB table, i.e.
   8 pages at `PAGE_SIZE=4096`).
2. **The `__arm__`/`PICO_BOARD` gate** needed no patch at all —
   `LUGALCHESS_EMBEDDED`'s real guard (`engine/include/version.h`) already
   fires correctly on any freestanding RISC-V cross-compile, and since
   `console.c` wasn't vendored, none of its ~19 gated blocks (flash
   save/load, etc.) were ever in scope to audit.
3. **The `MoveList`-on-recursion-stack risk** was already fixed upstream
   (hoisted into static per-ply arrays) before this phase reached
   implementation — confirmed present, not re-fixed.
4. **`printf`/`fflush`/`atoi`/`srand`/`abs`** -> a small compatibility header
   (`user/chess/include/chess_platform.h`): `#define printf cprintf` covers
   the overwhelming majority of call sites unmodified (`cprintf`'s `%d`
   already reads a `long` regardless of an `l` length modifier, so `%ld`
   sites needed no cast); the real incompatibilities (`%.0f`, entirely
   unsupported, and `%016llx`, which desyncs the whole argument stream since
   only a single `l` is skipped) were hand-fixed at their ~5 call sites in
   `search.c`/`tt.c`/`position.c`/`bitboard.c`. `atoi`/`srand`/`rand` are
   ~20-line implementations in `chess_platform.c`; `abs` is a macro.
5. **A new problem H0 didn't anticipate**, found by the RV64_MMU build, not
   RP2350: on any LugalOS RISC-V target (no Zbb extension, confirmed even on
   RP2350's `rv32imac_zicsr_zbs`), `__builtin_ctz`/`__builtin_popcount` lower
   to libgcc calls (`__ctzsi2`, which itself promotes to 64-bit `__ctzdi2`
   on this backend) whose reference to libgcc's `__clz_tab` failed to
   relocate in the full kernel link (`relocation truncated to fit:
   R_RISCV_HI20`) — a genuine toolchain/multilib defect, confirmed with a
   two-line reproduction completely outside this codebase. Fixed by forcing
   `bitboard.h`'s existing (previously unreachable) pure-software bit-scan
   fallback on `__riscv` ahead of the GCC-builtin branch — zero library
   calls, confirmed via the same repro, uniform across every LugalOS target
   rather than special-casing one board.
6. Wired as `chess-selftest` (all targets — no hardware dependency, searches
   a fixed midgame position and reports the move via `cprintf`; this is what
   made the engine testable on fast QEMU iteration before ever touching real
   silicon) and `chess-run` (RP2350 + `CONFIG_ENABLE_DISPLAY` +
   `CONFIG_ENABLE_TM1638` only — the actual interactive game, TM1638 for move
   entry, ST7735 for the board; does not return, reset to exit, same shape
   as `p9serve`).

**Two more real, hardware-only findings, both from live play testing, neither
visible on QEMU:**

- **RP2350's Lisp node pool needed no further change, but the static heap
  budget did.** `search.c`'s per-ply move-list pools
  (`search_pv_movelists`/`search_q_movelists`, ~65 KB total) are plain
  `static` arrays, not heap-on-demand like `cc`/`ed` — they cost real,
  permanent `.bss` on every RP2350 build with chess enabled (the default),
  dropping the board's managed heap from 78 pages to 48. `tests/hw/
  test_rp2350.py`'s `C6/C7` test had a hardcoded `>= 70` pages assertion
  from phase6/7's own measurement; updated to `>= 45` with the reasoning
  recorded in the test itself. Converting these pools to `palloc`-on-demand
  (matching `cc`/`ed`'s own precedent) is a real candidate for a follow-up,
  not done here.
- **The TM1638 key protocol H0/H1 assumed from `tm1638_get_key()`'s own code
  comment — keys 0-7 for file, 8-15 for rank — was wrong**, confirmed
  directly by the project's original author (also this repo's user):
  keys 0-7 double up for *both* file and rank, in sequence (first press =
  file, second = rank); 8-15 are reserved for menu functions (level/board/
  info/options) this phase doesn't implement. The bug this produced was
  genuinely confusing to debug live: with the wrong assumption,
  `tm_read_square()` silently waited forever (well, up to its own 15 s
  timeout, recurring) for a key range the physical pad never sends during
  normal move entry — indistinguishable from a real hang without adding
  live `raw key=N` diagnostics to `tm_wait_key()` and watching the console
  while the user pressed keys in real time. Kept those diagnostics in the
  shipped code (low-frequency, genuinely useful for any future TM1638
  troubleshooting) rather than stripping them back out once the real bug
  was found.

**Verified:** all three targets build clean, 181/181 QEMU tests unchanged.
Hardware: 15/15 `tests/hw/test_rp2350.py` (with the updated C6/C7 threshold).
`chess-selftest` run on both QEMU rv32 and real RP2350 silicon from the exact
same non-book midgame FEN: identical move choices and PV lines on both
(`b1c3` at every completed depth), QEMU reaching depth 9 (~19.8 M nodes/s,
host-speed emulation) and real Hazard3 silicon reaching depth 6 in the same
2 s budget (~44 K nodes/s — the real, honest hardware number, not QEMU's). A
full interactive game played live via TM1638 + ST7735 on the physical board:
correct board rendering (with a live-diagnosed and fixed light-square/white-piece
contrast issue), correct move entry, correct engine replies (e2e4 -> d7d6
confirmed on both the TFT board and its text status line), correct handoff
back to waiting for the next human move.

---

## Deliberately out of scope for this phase

**UCI-over-USB-CDC / desktop GUI bridge** (LugalChess's `gui/lugalgui` PySide6
app talking to the board): depends on more of the stdio-format gap than the core
hardware scenario needs (the `%.0f`/`%ld` UCI `info` line specifically) and on
LugalOS's own USB CDC / port-binding (C8) semantics for a second console-like
role. Real candidate for a follow-up phase once H1-H4 prove the on-device game
works standalone — not a blocker for "play chess on the physical board."
