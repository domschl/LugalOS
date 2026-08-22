# Phase 15 — RAM reclamation on RP2350

**Status: ANALYSIS ONLY, 2026-08-22. Nothing implemented.** This document is
the review requested after phase 13's own closing note ("chess's peak now
uses 100% of the available heap on this board... the next time either
happens, the fix will have to come from actually reducing something's
footprint").

Every figure below is measured against the checked-in `build/rp2350`
(chess persona, commit `42e02e9`) with `riscv64-elf-nm -S -l` and
`readelf -S`, not estimated.

---

## 0. The baseline, measured

RP2350 has 520 KB of SRAM: 512 KB striped across SRAM0-7 at `0x20000000`,
plus two unstriped 4 KB banks (SRAM8/SRAM9) at `0x20080000`/`0x20081000`,
**contiguous with main RAM in the address map**. `linker/rp2350.ld`
declares only the 512 KB.

| region | bytes | note |
|---|---:|---|
| `.ustacks16384/2048/512/256` | 22,272 | driver U-mode PMP regions, already packed (M5) |
| `.data` | 624 | |
| `.bss` | 265,388 | of which **5,598 is intra-section alignment padding** |
| `.stack` (boot) | 16,388 | |
| padding to `_kernel_end` | 2,528 | `.data`→`.bss` 4096-alignment, and the final `ALIGN(4096)` |
| **`_kernel_end` = `0x2004b000`** | **307,200** | 300 KB static |
| heap (`_kernel_end` → `_heap_end`) | 217,088 | **53 pages / 212 KB** |
| SCRATCH_X + SCRATCH_Y | 8,192 | **allocated to nothing; both sections are size 0** |

Static RAM by source file (symbols in `[0x20000000, 0x20080000)`):

```
  95104  user/lisp/lisp.c          14025  user/chess/src/search.c
  32904  fs/p9_link.c              12316  fs/p9_chan.c
  26688  user/chess/src/chess_ui.c  9755  fs/vfs_server.c
  16740  drivers/usb_cdc.c          8422  kernel/shell.c
  14676  drivers/uart_net.c         8195  kernel/balloc.c
                                    6800  user/chess/src/zobrist.c
                                    6152  kernel/line_editor.c
                                    5440  fs/9p.c
  ... 282,668 total
```

### Where the 53 heap pages go at chess's peak

| consumer | pages | source |
|---|---:|---|
| balloc arena | 16 | `BALLOC_ARENA_PAGES=16`, `balloc_init()` |
| driver/server task stacks | 9 | usbcdc/uart/heartbeat/sdblk/i2c/st7735/tm1638 ×1, p9srv ×2 |
| chess transposition table | 8 | `init_tt()`, fixed 32 KB |
| chess move-list pools | 17 | `2 × 64 × sizeof(MoveList)` = 66,048 B |
| chess scratch `Position` | 3 | `sizeof(Position)` = 8,360 B |
| **total** | **53** | exactly the whole heap; `Pages Peak: 53` |

Two structural facts follow from this table and are worth stating plainly,
because most of the recommendations below are corollaries:

1. **`.bss` and the heap are the same budget.** `_heap_end` is a fixed
   physical address; `_kernel_end` is `.bss`'s end. Four KB of `.bss`
   removed is exactly one more heap page, permanently, for every
   subsystem. Phase 13's `STRING_POOL_SIZE` fix already proved this
   arithmetic on hardware (16 KB → 4 pages, predicted and observed).
2. **The largest single heap tenant has no users.** See §1.1.

---

## 1. Tier 0 — reclaim, no functional change

### 1.1 The buddy allocator's arena is 16 pages held for a demo command *(done, 2026-08-22)*

`balloc_init()` unconditionally reserves `BALLOC_ARENA_PAGES = 16` pages
(64 KB, **30% of the heap**) at boot. The only callers of
`balloc_alloc()`/`balloc_free()` in the entire tree are
`cmd_ballocdemo()` in `kernel/shell.c:1146` and the two assertions
`tests/runner.py:1382-1386` makes against it. Nothing else — no driver,
no chan endpoint, no task — has ever called it. `kernel/balloc.h`'s own
comment anticipated this ("Deliberately modest for M1... M4/M5 raise it
once real callers exist"); the callers never arrived, and the arena was
never lowered back.

The tree also pays a second time in `.bss`: `g_longest[NODE_COUNT]` is
`2 × (65536/32) − 1 = 4095` entries × `uint16_t` = **8,190 bytes**, sized
from `BALLOC_ARENA_PAGES`.

**Recommendation.** Both of:
- Board-scale `BALLOC_ARENA_PAGES` — 4 pages (16 KB) on RP2350, keeping 16
  on the QEMU targets where 128 MB makes it free. `g_longest` falls to
  1,023 entries = 2,046 B. `ballocdemo`'s churn loop allocates 8 slots
  from `{32, 96, 200, 500, 1500, 4000, 100, 60}`, which round to 7,264
  bytes of buddy blocks — 16 KB leaves room for the fragmentation the
  test deliberately provokes, 8 KB would not.
- Make the arena **lazy**: reserve it on the first `balloc_alloc()` rather
  than in `kernel_main()`. Then a boot that never runs `ballocdemo` pays
  nothing at all. This is a ~10-line change to `balloc.c` and leaves
  `balloc_init()`'s call site in `main.c` as a no-op registration.

**Recovers: 12 heap pages directly, plus ~6 KB of `.bss` (1.5 more
pages). With lazy init, 16 pages.**

#### What was done

Both, plus a build fix the work uncovered.

- **`CONFIG_BALLOC_ARENA_PAGES` is now a board fact**, mandatory in
  `cmake/gen_config.cmake`'s schema alongside `CONFIG_PALLOC_MAX_PAGES` and
  set in all four board files: **4** on both RP2350 personas, **16** on the
  two QEMU targets. `kernel/include/kernel/balloc.h` reads it from
  `lugalos_config.h` the way `palloc.h` already reads its own, rather than
  through an `#ifdef` in the header. The header now also says out loud that
  the constant is paid twice and that only one half is lazy — the arena is
  on-demand, but `g_longest` is compile-time `.bss` either way.
- **The arena is reserved on the first `balloc_alloc()`**, not in
  `balloc_init()`. `balloc_init()` stays (same call site, same ordering
  contract with `palloc_init()`) but only establishes the unreserved state
  and logs that the allocator exists. A new static `balloc_reserve()` does
  what `balloc_init()` used to. The size check moved above the reservation
  so an impossible request cannot drag an arena out of the heap on its way
  to returning NULL.
- **`balloc_stats()` reports `ARENA_BYTES`, not 0, while unreserved.** The
  figure is documented as "the largest single request `balloc_alloc()` could
  satisfy right now", and an unreserved allocator would reserve a whole
  empty arena to serve one; 0 describes a *full* allocator, the opposite
  state. Noted in place that this is a forecast in exactly one case — the
  reservation itself can still fail on a heap too fragmented to place an
  `ARENA_BYTES`-aligned run, and `balloc_alloc()` says so on the spot.

**Measured, RP2350 chess persona, clean rebuild:**

| | before | after |
|---|---:|---:|
| `.bss` | 304,048 | 299,952 |
| `g_longest` | 8,190 | 2,046 |
| `_kernel_end` | `0x2004b000` | `0x2004a000` |
| heap | 53 pages | **54 pages** |
| heap in use at boot | 25 (16 arena + 9 tasks) | **9** |
| free at boot | 28 | **45** |
| chess peak | 53 / 53 | **37 / 54** |

Chess goes from filling the heap exactly to leaving 17 pages spare.

#### The stale-config build bug this uncovered *(fixed)*

Validating the RP2350 value meant temporarily building rv32 with 4 pages.
It did not take: the boot log kept reporting the old 64 KB arena while
`build/rv32/lugalos_config.h` on disk already said 4. A second, otherwise
identical `cmake --build` fixed it.

`lugalos_config.h` was produced by a bare `add_custom_target(... COMMAND
...)`, which declares no producer/consumer edge on the file it writes, and
`add_dependencies(lugalos.elf lugalos_config)` orders that target before the
final **link** only, never before an individual compile. So an edit to any
`cmake/board-*.cmake` could yield objects compiled against the *previous*
config, silently.

This is the same failure `CMakeLists.txt` already documents and fixes for
`lugalos_build_id.h` ("a build-id mismatch on three consecutive real
flashes"), which got a real `add_custom_command(OUTPUT ...)` plus
`OBJECT_DEPENDS`; the board-config block never got the same treatment. It is
also the worse of the two: a build id is a cosmetic string, while this
header carries the board's pin map and its memory sizing — a stale object
means a flashed image driving the wrong GPIOs, or an allocator sized against
a heap it did not get.

Fixed the same way: `add_custom_command(OUTPUT ${LUGALOS_CONFIG_HEADER} ...
DEPENDS _always_rebuild ${LUGALOS_BOARD_FILE} gen_config.cmake)`, the target
reduced to `DEPENDS` on it, and `OBJECT_DEPENDS` wired to **every** source
rather than an enumerated subset — `lugalos_config.h` arrives transitively
(`palloc.h` and `balloc.h` both include it, and most of the tree includes one
of those), so a hand-maintained list would go stale silently, which is the
failure the edge exists to prevent. `fs/vfs_server.c`'s pre-existing
build-id `OBJECT_DEPENDS` is folded into one property value rather than
overwritten — `OBJECT_DEPENDS` is a single property, so a second
`set_source_files_properties()` on the same file would have silently
reintroduced the stale-build-id bug.

Costs nothing on an unchanged config: `gen_config.cmake` is write-if-changed,
so the header's mtime does not move and ninja recompiles nothing. Verified
both ways — a one-pass board-file edit now takes effect, and a no-op rebuild
recompiles zero objects.

#### Verification

- QEMU suite **245/245** on rv32 + rv64, including both existing balloc
  assertions (`Rounding/alignment: OK`, `largest=N arena=N`).
- The RP2350 4-page arena was exercised directly by temporarily building
  rv32 with `CONFIG_BALLOC_ARENA_PAGES=4`: `ballocdemo` twice in a row,
  `Rounding/alignment: OK` and `largest=16384 arena=16384` both times. The
  demo's peak is 7,264 bytes of buddy blocks across 8 live slots, so 16 KB
  keeps better than 2x headroom.
- **New hardware test `test_balloc_arena`** (`tests/hw/test_rp2350.py`),
  registered before `test_memory_margins`. It asserts what the QEMU suite
  structurally cannot, because both facts are about this board's own value:
  that the arena is actually big enough here, and that it is not held at
  boot (visible as `Pages Used` stepping up by exactly 4 on the first
  `ballocdemo` of a fresh boot). Until now nothing on hardware called
  `balloc_alloc()` at all, so a too-small arena would have surfaced as
  "[BAllocDemo] Allocation failed" the first time a human typed the command,
  months later.
- `test_heap_on_demand`'s docstring said the heap baseline was 60 pages. It
  was 53 — stale since phase 13, whose own regression and fix both moved it.
  Corrected, and updated to 54.

**Not done: releasing the arena when it becomes fully free.** It would make
the cost zero even after a run rather than only before one, and it is what
the tree's own acquire/release rule (`chess_session_end()`, `pools.c`) would
suggest. Rejected because it buys nothing real here — `ballocdemo` is absent
from the hardware suite, so on RP2350 the arena is never reserved in
practice — while introducing a genuine hazard for the callers this allocator
still exists for: a re-acquire can fail on a heap that has fragmented since
the first one succeeded, turning a working caller into an intermittently
failing one. Worth revisiting only if a real caller ever arrives.

### 1.2 8 KB of SRAM is declared and then never used *(done, 2026-08-22)*

`.scratch_x` and `.scratch_y` are both size 0 in the linked image; the
only references to `__scratch_*` anywhere are the six words in
`arch/riscv/rp2350/boot_header.S`'s copy table, which copy nothing.
`_heap_end` is `ORIGIN(RAM) + LENGTH(RAM)` = `0x20080000`, and SRAM8
begins at exactly that address.

**Recommendation.** `_heap_end = ORIGIN(SCRATCH_Y) + LENGTH(SCRATCH_Y)`,
guarded so it cannot rot:

```ld
ASSERT(SIZEOF(.scratch_x) == 0 && SIZEOF(.scratch_y) == 0,
       "something now places into a scratch bank -- the heap covers them; see plan/phase15")
```

The heap bitmap already handles a non-power-of-two page count. The banks
are unstriped, which matters only for multi-hart contention this build
does not have.

**Recovers: 2 pages (8 KB).**

#### What was done

`_heap_end` and `_ram_end` both now end at `ORIGIN(SCRATCH_Y) +
LENGTH(SCRATCH_Y)`. `_ram_end` had to move with it: `/proc/meminfo` reports
the heap as a fraction of RAM, and leaving RAM at 512 KB while the heap ran
to 520 would have made that figure wrong. The linker script's own comment
saying the RAM map "deliberately excludes SCRATCH_X/Y -- 8 KB that exist on
the die but that nothing on this board currently places into" was accurate,
and was precisely the argument for handing them to the allocator instead.

**Checked before changing it, because the scratch banks are not obviously
free:**

- **The bootrom's initial SP is this exact address.** `boot_header.S`'s
  IMAGE_DEF declares `.word SRAM_END` (0x20082000), so the bootrom does push
  into these banks before handing over. Harmless: `_reset_handler`'s first
  action on core 0 is `la sp, _stack_top`, with no call or push between
  entry and that instruction, so whatever the bootrom left there is dead
  long before `palloc_init()` runs.
- **Core 1 never takes a stack at all** — it branches straight to `wfi` and
  spins.
- **The boot copy table's scratch entries are no-ops.** `address_mapping_
  table` carries `__scratch_x_load_ptr/_start/_end` triples, but with the
  sections empty `data_cpy`'s `bltu a1, a2` sees `start == end` and returns
  immediately.
- **Nothing at runtime touches a scratch bank.** The only `__scratch_*`
  references in the tree are those six words. `rp2350_reboot_to_bootsel()`
  does call into the bootrom at runtime, but it runs on the caller's stack
  and does not return.

**The guard.** `ASSERT(SIZEOF(.scratch_x) == 0 && SIZEOF(.scratch_y) == 0,
...)` in the linker script, because an object placed there would now be
handed out as heap memory and silently overwritten. Negative-tested rather
than assumed: adding a throwaway `__attribute__((section(".scratch_x.probe")))`
variable to `kernel/palloc.c` made the link fail with the message naming the
cause, and removing it linked clean again.

**Measured:** heap 54 -> **56 pages**; `RAM: 520 KB total` on the board,
where it read 512 before.

### 1.3 Three permanent 8 KB `static Position` scratch objects *(done, 2026-08-22)*

`sizeof(Position)` is 8,360 bytes, almost all of it
`history[MAX_PLYS=256]` at 32 B/ply. Four instances are permanent `.bss`:

| symbol | site | lifetime |
|---|---|---|
| `g_chess_pos` | `chess_ui.c:54` | the game — genuinely permanent |
| `temp_pos.2` | `search.c:778` | printing one PV line |
| `temp_pos.5` | `chess_ui.c:745` | parsing a save file's FEN |
| `temp_pos.6` | `chess_ui.c:847` | parsing a typed FEN |

The last three are throwaway scratch, never live at the same time as each
other, and `chess_ui.c` already solved exactly this problem once:
`g_chess_scratch` is an on-demand `Position` shared between `tm_load()`
and `tm_probe_pv()`, with the reasoning written out at `chess_ui.c:56-65`
("A second permanent ~8 KB static `Position` for that... is exactly the
kind of..."). The three above were simply never brought into it.

**Recommendation.** Export `Position *chess_scratch_position(void)` from
`chess_ui.c` (returning `g_chess_scratch`, non-NULL for the duration of a
session) and route all three sites through it. `search.c:778`'s use is
inside `search_position()`, which by construction runs only after
`chess_ensure_init()` succeeded.

**Recovers: 25,080 bytes = 6 pages.**

#### What was done

`chess_ui.c` gained `Position *chess_scratch_position(void)`, returning the
`g_chess_scratch` it already owns; all three statics were deleted. The two
in `chess_ui.c` use the variable directly (`console_load()` now has exactly
the shape `tm_load()` already had); `search.c` declares the accessor
`extern` at its call site, matching how `search_progress_callback()` and
`search_poll_stop_callback()` — the only other two things it reaches into
that file for — are already wired, and the deviation is recorded in
`search.c`'s own list of changes-from-upstream.

`chess_platform.c` would have been the tidier home for a shared scratch, but
it sits *below* `position.h` in the include graph (`position.h` -> `defs.h`
-> `chess_platform.h`), and `Position` is a typedef'd anonymous struct so it
cannot be forward-declared. The existing seam avoids the circularity
entirely.

#### Why one scratch is safe for both the engine and the UI

This was the question worth answering before touching anything, since
`tm_probe_pv()` already mutates `g_chess_scratch` *during* a search:

- `tm_probe_pv()` is reached only from `search_poll_stop_callback()` ->
  `tm_search_ticker_tick()`.
- `search_poll_stop_callback()` is called only from `check_up_time()`, i.e.
  only while `pv_search()` or `quiescence()` is on the stack.
- `search.c`'s PV walk runs *between* iterative-deepening iterations, after
  `pv_search()` has returned for that depth, and calls only `printf`,
  `make_move` and `read_tt` — none of which reach the poll callback.

Same task, no interrupt path into either, so the two are strictly
sequential. The accessor can return NULL (before `chess_ensure_init()`,
after `chess_session_end()`); every `search_position()` call is gated behind
a successful `chess_ensure_init()` — there is exactly one call site, and all
four entry points into `chess_ui.c` gate first — so `search.c` can never see
NULL, and checks anyway, because being wrong about that on a board is a null
dereference mid-search.

**Measured:** `.bss` 299,952 -> **274,880** (25,072 bytes); heap 56 ->
**62 pages**.

---

## Verification of §1.1-§1.3 on real silicon *(2026-08-22)*

Flashed to the chess-persona board and run end to end.

**`/proc/meminfo`, fresh boot:**

```
  Pages Total: 62          (was 53)
  Pages Free:  53          (was 28)
  Pages Used:   9          (was 25 -- balloc's 16 are simply never taken)
  RAM: 520 KB total        (was 512 -- §1.2's scratch banks)
  Image (data+bss): 254 KB (was 283 KB)
  Heap: 248 KB managed of 248 KB   (was 212 KB)
```

**Chess, the number this phase exists for: `Pages Peak: 37` of 62.** It was
53 of 53. Twenty-five pages spare where there were none, and `Pages Free`
returns to 53 after `quit`, so the release path is intact.

**All three §1.3 call sites exercised through the console REPL**
(`(chess-console)`, since `(chess)` on a board with a display enters the
TM1638 game loop instead): `fen <position>` loaded an Italian Game position
and drew it correctly; `save`/`load` round-tripped through
`/sd0/system/chess.save`; and `e2e4` drew an engine reply (`bestmove d7d6`),
which is `search_position()` running with its PV printer on the shared
scratch.

**Hardware suite: 23/23** (`tests/hw/test_rp2350.py`), up from 22 tests --
the new one is §1.1's `test_balloc_arena`, which reported exactly what it
was written to prove: *"16 KB arena, coalesced clean, heap step on first use
= 4 pages"*. Two pre-existing tests independently re-measured the same gain:
`C2: two user programs resident at once ... heap peak 37/62 pages`, and
`C6/C7 ... heap 9/62 pages before and after a compile`.

**QEMU suite: 247/247** on rv32 + rv64 (245 before; the two new ones are the
`fen <position>` parse assertions added with §1.3, one per target).

### A tooling bug found on the way *(fixed)*

`tests/hw/flash.py`'s 1200-baud touch worked perfectly, and the script
reported *"The board did not enter BOOTSEL. Most likely its current firmware
predates the 1200-baud touch..."* and advised a manual BOOTSEL press. The
board had entered BOOTSEL: `lsusb` showed `2e8a:000f RP2350 Boot` and
`/dev/disk/by-label/RP2350` was present. Nothing had *mounted* it, and
`bootsel_volumes()` only ever looks at mount points -- fine on macOS and on
a desktop Linux running an automounter, wrong on a host without one.

The diagnosis was the bug, not the detection: it named the firmware as the
likely cause when the firmware was blameless, and sent the reader toward a
fix (manual BOOTSEL, reflash) that would not have changed anything.
`flash.py` now checks for an unmounted RP2350 block device before printing
that message, mounts it with `udisksctl` (no root needed for a removable
device, so it stays runnable as the same unprivileged user as the rest of
the suite), and if that is unavailable prints the exact manual command
instead.

---

## 2. Tier 1 — board-scoped re-sizing

### 2.1 `MAX_SEARCH_PLYS` and the quiescence pool — 17 pages for 66 KB *(done, 2026-08-22)*

`search_pools_init()` allocates `2 × MAX_SEARCH_PLYS × sizeof(MoveList)`
= `2 × 64 × 516` = 66,048 B, rounded to 17 pages (3,584 B of page-rounding
waste on top). Two independent overshoots:

- **`MAX_SEARCH_PLYS = 64`.** The pools are indexed `ply % MAX_SEARCH_PLYS`
  with a hard cutoff at `ply >= MAX_SEARCH_PLYS - 1` (`search.c:453`,
  `:498`), so the constant is a ceiling, not a requirement. A Hazard3 at
  150 MHz with a 32 KB TT does not reach ply 32 in a console-length think
  time — pv depth plus quiescence extension together. **32 halves this.**
- **The quiescence pool uses a full 256-entry `MoveList`.**
  `generate_captures()` cannot produce more than ~74 moves even in
  constructed positions; 256 is `MAX_MOVES`, the bound for
  `generate_moves()`. A separate 96-entry list type for the q pool costs
  196 B instead of 516 B per ply.

Together: pv `32 × 516` = 16,512 + q `32 × 196` = 6,272 → 22,784 B = **6
pages** instead of 17.

**Recommendation.** Do `MAX_SEARCH_PLYS = 32` first (one constant, board-
scoped) and measure the depth actually reached with `go movetime 5000`
before deciding whether the capture-list split is worth the type change.
The first change alone is 8 pages.

**Recovers: 8 pages (alone) or 11 pages (both), off chess's peak.**

#### What was done: `MAX_SEARCH_PLYS` 64 -> 32 on RP2350

Board-scoped with `#if defined(CONFIG_BOARD_RP2350)` in `search.c` itself,
not via a board file. That is a deliberate departure from §1.1's
`CONFIG_BALLOC_ARENA_PAGES`: an allocator's arena size is a board fact of the
kind `palloc.h`'s own convention already covers, whereas this is a subsystem
tuning constant, and the tree already spells that axis this way in
`user/lisp/lisp.c` (`NODE_POOL_SIZE`, `STRING_POOL_SIZE`). Kept out of
`defs.h`'s `LUGALCHESS_EMBEDDED` split too: that macro is true on the QEMU
targets as well (`version.h` -- it means "bare metal", not "small"), and this
split is about one board having 512 KB where the others have 128 MB.

Pools: `2 * 32 * 516` = 33,024 B = **9 pages**, from 66,048 B = 17.

#### Why 32, measured rather than argued

The risk is not a crash -- `pv_search()`/`quiescence()` already guard on
`ply >= MAX_SEARCH_PLYS - 1` and return a static evaluation -- it is silently
capping lookahead. So the question was whether that guard ever *fires* at 32,
and the way to answer it was to count.

A temporary counter on the guard itself (added, measured, fully removed) was
run against the 32-ply value **on QEMU**, deliberately: at ~19.8M nodes/s it
searches far deeper in a given wall-clock budget than RP2350's ~44K, making
it a strictly harder test than this board can face.

| position | depth reached | guard hits |
|---|---:|---:|
| Kiwipete (tactical, 48 legal moves) | 7 | **0** |
| promotion tactics | 9 | **0** |
| rook-and-pawn endgame | 11 | **0** |
| bare K+P endgame (deepest by far) | 15 | **0** |

Separate instrumentation put the peak ply at roughly `d+3..d+4` -- that is
quiescence's real extension beyond the iterative-deepening depth, and it is
much smaller than assumed when this section was first written. A 31-ply
ceiling therefore corresponds to an iterative-deepening depth near 27:
roughly twice the deepest figure above, on hardware ~450x slower than the
machine that produced it.

**Confirmed on the board after flashing:** a mate-in-one that the opening
book cannot answer (`6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1`) searched and
solved it -- `info depth 1 score mate 1 nodes 47 ... pv e1e8`, `bestmove
e1e8`. `Pages Peak: 29` of 62, down from 37.

**Suites after 2.1a:** QEMU **247/247**, hardware **23/23**. The hardware suite's own `heap peak 36/62` is not chess's number:
it runs no chess test at all, and that figure comes from its user-program
tests, whose images stay cached for the rest of the boot.

#### 2.1b: the capture-list split *(done, on review -- see below)*

Originally deferred here; the user overrode that, on the grounds that
LugalOS is LugalChess's successor rather than a passive consumer of it, so
improving the vendored engine is in scope, and that 3 pages is ~10% of a
62-page heap. Both points are right, and the change turned out cleaner and
cheaper than the deferral assumed.

**Shape.** `MAX_CAPTURES` (96) in `defs.h`; a `CaptureList` type in
`movegen.h`; `generate_captures()` takes one; `sort_moves()` now takes
`(Move *moves, int count, ...)` so it serves both list types without knowing
which struct the array came from. Inside `generate_captures()` the 37
`add_move()` pushes became `add_capture()` -- a mechanical retarget confined
to that one function's line range, with `generate_moves()`'s 50 pushes
untouched.

The pools are no longer one array of one type, so `search_pools_init()`
sizes the two halves separately and splits by byte offset instead of pointer
arithmetic, with a `_Static_assert` that `sizeof(MoveList)` is a multiple of
`_Alignof(CaptureList)` -- true today because both structs align to 4, and
exactly the kind of thing that stops being true silently if either grows a
wider member.

| | pool bytes | pages |
|---|---:|---:|
| RP2350 (`MAX_SEARCH_PLYS` 32) | 33,024 -> **22,784** | 9 -> **6** |
| QEMU (64) | 66,048 -> **45,568** | 17 -> **12** |

**`MAX_CAPTURES` is one value on every target**, deliberately unlike
`MAX_SEARCH_PLYS`. 96 against a measured maximum of 18 is not a capability
being traded for RAM, and keeping it uniform means the QEMU suites exercise
the same bound -- and the same overflow path -- that the board runs.

#### The bounds check, and why the first measurement of it was wrong

`add_move()` was unchecked upstream, which was survivable only because
`MAX_MOVES` (256) sits above the 218-move maximum for a legal position. A
list deliberately *narrower* than its theoretical worst case changes that:
overflow stops being hypothetical and becomes the failure mode to design
for. Both helpers now bounds-check and drop when full. Dropping is the right
behaviour here -- these lists sit in a contiguous per-ply pool, so an overrun
would land in the *next ply's list*: silent corruption of a live neighbour,
at a search depth that would be near-impossible to reproduce. A dropped move
costs one branch of one quiescence node instead, in a position no game
reaches.

The cost was measured, and the first measurement was misleading:

| | nodes | aggregate nps |
|---|---:|---:|
| QEMU rv32, unchecked | 14,753,293 | 225,000 |
| QEMU rv32, checked | 14,753,293 | 159,051 |

A 29% throughput loss would have been far too much to accept for robustness,
and on that evidence the check would have been dropped from `add_move()`.
Re-measured on the actual target, same binary shape, same perft suite:

| | nodes | nps | perft |
|---|---:|---:|---|
| RP2350, unchecked | 480,735 | 203,960 | 75 passed depths, 0 errors |
| RP2350, checked | 480,735 | **203,701** | 75 passed depths, 0 errors |

**0.13% -- nothing.** The QEMU figure was a TCG artifact: added branches
split translation blocks, which the emulator pays for and real in-order
Hazard3 silicon does not. Worth recording as a general caution, since perft
throughput on QEMU is otherwise a tempting proxy for engine speed: it is a
fine proxy for *node counts*, and a bad one for the cost of *branches*.

#### What the deferral got wrong

Kept as a record, since the reasoning was checked and still came out
mistaken. The deferral rested on the signature changes being invasive and on
`add_move()` having no bounds check. The first was overestimated -- the
mechanical part was one function's pushes, and `sort_moves()` losing its
struct parameter made it *simpler*, not more complex. The second was an
argument for making the change carefully, not for avoiding it: the missing
bounds check was itself worth fixing, and it turned out to be free on the
hardware that matters.

#### Verification of §2.1 as a whole

- **Hardware 23/23**, QEMU **247/247**, including `Chess Perft Move-Generation
  Correctness` on both -- move generation is what 2.1b touched, and perft is
  the test that would notice.
- Board perft, full run: 480,735 nodes, **75 passed depths, 0 errors** --
  identical node counts to the unchecked build, so neither the narrower
  capture list nor the bounds checks changed what is generated.
- Board search: `info depth 1 score mate 1 nodes 47 ... pv e1e8` on a
  mate-in-one the opening book cannot answer.
- **`Pages Peak: 26` of 62**, measured from a fresh boot with chess as the
  only workload. Note the figure must be read from a clean boot: the peak is
  cumulative since `palloc_init()`, so a session that ran `(perft 3)` first
  reports 31 -- perft's own pool, not chess's.

Chess's peak across this phase: **53/53 -> 37/62 -> 29/62 -> 26/62.** From
no spare pages at all to 36.

#### Not in scope: §2.2

Skipped at the user's direction, and correctly. `UndoState` carries
`Move move`, so `Position.history[]` *is* the move list a PGN exporter would
serialise -- truncating `MAX_PLYS` would not merely weaken threefold-
repetition and 50-move detection, it would remove what a planned feature
needs to read. After §1.3 only two `Position` instances remain, so the whole
of §2.2 was worth ~2.3 KB of `.bss` plus one heap page. 2.1a incidentally
helps here: `history_ply` carries game plies *plus* search depth, so halving
`MAX_SEARCH_PLYS` hands ~32 plies of that budget back to the game history.

### 2.2 `MAX_PLYS = 256` costs 8 KB per `Position`

`Position.history[]` is the game-plus-search undo stack. At 32 B/entry,
256 plies is 8,192 of `Position`'s 8,360 bytes — the struct is 98%
history.

This one deserves care rather than a blind cut. 128 plies is 64 full
moves, which real games routinely exceed, and the array serves threefold-
repetition and 50-move detection where truncating changes *results*, not
just limits. **192** is the defensible number: it saves 2 KB per instance
and still exceeds any game this UI is likely to host. The real fix is to
decouple search-ply undo depth from game-history depth, which is an
engine change and belongs upstream in LugalChess.

After §1.3 there are two instances (`g_chess_pos`, the heap scratch), so
at 192 this is ~4 KB of `.bss` plus keeping the heap scratch at 2 pages
instead of 3.

**Recovers: ~1 page of `.bss` + 1 heap page. Low priority.**

### 2.3 `P9_MAX_MSIZE = 4096` × ten static buffers ≈ 48 KB *(done, 2026-08-22)*

| buffer | file | bytes |
|---|---|---:|
| `g_req_buf`, `g_resp_buf`, `g_reply` | `p9_chan.c` | 12,288 |
| `resp_buf`, `rx_buf`, `tx`, `rx`, `g_remote_tx`, `g_remote_rx` | `p9_link.c` | 24,576 |
| `g_waiters[2].reply` | `p9_link.c` | 8,192 |
| `s_databuf` | `9p.c` | 4,096 |

msize is properly negotiated — `fs/9p.c:790` answers Tversion with
`min(req.msize, P9_MAX_MSIZE)` — so halving it on RP2350 is protocol-legal
and costs round trips, not correctness.

**Latent bug found while checking this.** `fs/9p.c:582` writes a
hardcoded `iounit = 4096` into every Ropen, with the comment "no
restriction beyond the negotiated msize". That is only true while
`P9_MAX_MSIZE` happens to be 4096. iounit must not exceed
`msize − IOHDRSZ(24)`; a conforming client that honours the advertised
iounit would issue a Tread the server cannot answer. **This should be
fixed regardless of whether msize is lowered** — it should be derived
from the connection's negotiated msize.

Second prerequisite: confirm `host/p9lib` honours the msize returned by
`client.py:301`'s `version()` rather than assuming its 4096 request was
granted, and likewise `host/fuse-p9`'s read/write chunking.

**Recovers: ~20 KB = 5 pages, once the iounit bug is fixed.**

#### What was done

`P9_MAX_MSIZE` is board-scoped -- **2048 on RP2350**, 4096 elsewhere -- with
`#if defined(CONFIG_BOARD_RP2350)` in `fs/include/fs/9p.h`, matching how
`lisp.c`'s pools and `MAX_SEARCH_PLYS` express the same axis. It costs round
trips, not correctness: msize is negotiated, so a peer asking for more is
answered with this and must respect it.

**Measured:** `.bss` 274,880 -> **247,056** (27,824 bytes); heap 62 ->
**69 pages**, better than the 5 predicted because the ten buffers were 48 KB
rather than the 41 KB first estimated.

#### The iounit bug *(fixed)*

Worse than first described. The hardcoded `4096` was not merely wrong once
msize dropped to 2048 -- **it was already unfulfillable at the old default of
4096**. Rread frames its payload behind `size[4] type[1] tag[2] count[4]`, so
a 4096-byte read against a 4096 msize needs 4107 bytes on the wire. Any
conforming client that believed the advertised iounit and issued a read that
size was asking for a reply the connection could not carry.

The root cause was structural: `p9_server_process()` computed the negotiated
msize for its Rversion, sent it, and **forgot it**, so the serializer had
nothing to derive an honest figure from and hardcoded one instead. Fixed by
storing it (`g_negotiated_msize`, alongside the equally-global fid table that
Tversion already resets) and adding `P9_IOHDRSZ` (24, Plan 9's own value --
set by Twrite's 23-byte header, so one number covers both directions).
`p9_negotiated_iounit()` derives the rest.

#### The other hardcoded sizes

Both LugalOS-side 9P *clients* sent `req.msize = P9_MAX_MSIZE` and then
ignored the Rversion reply, chunking reads at a hardcoded 1024. That happened
to be safe, but only by luck -- nothing bounded it by what the peer had
actually agreed to. `p9_link_cat()` now derives its chunk from the reply, and
`p9_remote_mount_t` carries the peer's msize so every Tread against a mount is
bounded by it. This matters concretely now that a mixed cluster is possible:
an RP2350 node answers 2048 where a QEMU node answers 4096.

Host side, `p9lib`'s `version()` returned the negotiated value and **both
call sites discarded it**, leaving `read_all()`/`write_all()` on a hardcoded
1024 that merely happened to fit under 2048. It now records `.msize`, exposes
`io_chunk()` (msize - IOHDRSZ), and both defaults derive from it. `open()`
also records the server's advertised `.last_iounit`, which it previously
parsed past and threw away.

#### Verification

- **On the board, over USB CDC:** a host client requesting 4096 is answered
  `msize=2048`, `io_chunk=2024`, and **`Ropen iounit = 2024`** -- where it
  used to say 4096 regardless. A real read through the link returns correct
  data.
- **New test `test_9p_iounit`** asserts the protocol invariant
  `0 < iounit <= msize - IOHDRSZ` rather than any fixed number, so it holds on
  every target. Negative-tested, unlike the first attempt at a guard in the
  input-eater work: with the hardcoded value restored it **fails on both QEMU
  targets** with `msize=4096 iounit=4096 limit=4072` -- which is also the
  cleanest demonstration that the bug predated the msize change.
- **Suites:** QEMU **249/249**, hardware **24/24**.

### 2.4 `string_pool` — 48 KB, the largest object in RAM *(done, 2026-08-22)*

`string_pool[384][128]` gives every interned symbol and string a fixed
128-byte slot. `define`, `car`, `lambda`, `cond` each occupy 128 bytes.
`make_str()`/`make_sym()` already truncate at `STRING_SLOT_LEN`
(`strncpy_local`), so slot length is a documented ceiling, not an
invariant.

Two options, in increasing order of payoff and effort:

- **`STRING_SLOT_LEN` 128 → 64 on RP2350.** One constant. Halves the pool
  to 24 KB. Shortens the longest storable string, which for a board whose
  console line editor caps at 512 and whose `Position` FEN strings are
  ~90 chars is worth checking against `test_rp2350.py` but is very likely
  fine. **24 KB.**
- **Two-tier pool.** 320 slots × 32 B + 64 slots × 128 B = 18,432 B.
  `alloc_string_slot()` picks the tier from `strlen`; `string_slot_index()`
  and the GC sweep each learn about two ranges. The free list is already
  threaded through slot bytes (`string_free_head`), so it generalises to
  one head per tier with no new storage. **~30 KB.**

**Recovers: 24 KB (6 pages) or ~30 KB (7 pages).**

#### What was done: the two-tier pool, sized from a measurement

The tier widths were not guessed. Instrumenting every `make_str()`/
`make_sym()` (added and fully removed) across the stdlib boot and an
evaluator stress run gave:

| length | allocations |
|---|---:|
| < 16 | 1187 |
| < 32 | 3 |
| < 64 | 2 |
| < 128 | 0 |
| >= 128 | 0 |

and peak *live* occupancy of slots needing more than 32 bytes: **zero**. So a
32-byte majority tier with a small full-width reserve, at `STRING_POOL_SIZE /
6` large slots -- 320 x 32 + 64 x 128 on RP2350.

**The slot count is unchanged**, so the pool still interns
`STRING_POOL_SIZE` strings; only the bytes per slot moved. A short string
*spills* into the large tier when the small one is full, so no workload can
intern fewer strings than before -- this is a layout change, not a capability
traded for RAM, which is why it is applied on every target rather than
RP2350 only. Uniformity also means the QEMU suites exercise the same flat
index mapping, the same two free lists and the same spill path the board
runs.

`intern_string()` now does the copy as well as the allocation. That is
load-bearing: `make_str()`/`make_sym()` used to pass `STRING_SLOT_LEN` to
`strncpy_local()` themselves, which would overrun a 32-byte slot the moment
one existed.

**Measured:** 49,152 -> **18,432 bytes**; heap 69 -> **76 pages**.

**Verified:** QEMU 249/249 including the node-pool-exhaustion test (which is
what drives the GC, both free lists and the clamp path). Boundary check on
the tiers -- 31, 32, 127 and 128-character strings -- returns exactly what
the single-pool build returned, including a pre-existing truncation at 111
characters that comes from the REPL's own line handling and is **not** a
tiering artifact: the old build was rebuilt and produces the identical
numbers.

### 2.5 Four static file buffers in `lisp.c` — 24 KB, mutually exclusive *(done, 2026-08-22)*

`buf[8192]` (`load`, :1419), `boot_buf[8192]` (:2190), `buf[4096]`
(`cat`, :1242), `buf[4096]` (`read-file`, :1453). No two can be live
simultaneously — each is a function-scoped static holding one `vfs_read()`
result for the duration of one call.

**Recommendation.** One shared 8 KB static, or better, `palloc_pages(2)`
acquired at entry and freed at exit, matching the on-demand pattern
`chibicc/pools.c` and `search_pools_init()` already establish. On-demand
is strictly better here: these calls are rare and never nested, and 8 KB
returned to the heap is 8 KB available to chess.

**Recovers: 16 KB shared, 24 KB on-demand = 4-6 pages.**

#### What was done, and why *not* the shared buffer

All four now take their buffer from `palloc_pages()` per call and give it
straight back -- **a buffer per call, not one shared on-demand buffer**,
which is where the plan's own first suggestion was wrong.

`load` evaluates the file it has just read, and that evaluation can reach
another `load` or a `cat`. One shared buffer would let the inner call
overwrite the text the outer one is still walking. The four separate statics
were, accidentally, protecting against exactly that for three of the four
combinations; collapsing them to one would have turned a latent bug into a
live one. Per-call allocation makes nesting correct by construction, and
costs a bitmap scan on a path that already does filesystem I/O.

The boot loader's buffer goes the same way -- it existed in `.bss` for the
life of the board to serve two reads during init.

**Measured:** `.bss` 216,336 -> **191,760** (24,576 bytes, exactly the four
buffers); heap 76 -> **82 pages**.

**Verified:** QEMU 249/249. Nesting tested directly -- an outer `load` whose
file loads an inner one: both definitions take effect (`inner-ran` 1,
`outer-ran` 2) and the pages return afterwards, which the shared-buffer
design would have failed.

---

## 3. Tier 2 — structural

### 3.1 The persistent-vs-session audit (the general form, and the big lever) *(done, 2026-08-22)*

Of ~283 KB of static RAM, roughly **110 KB serves subsystems that are
idle almost all of the time**:

| subsystem | permanent `.bss` | actually live when |
|---|---:|---|
| chess (positions, zobrist, bitboards, history/killers/sort_scores, movelists) | ~48 KB | a game is running |
| 9P transport (`g_demux` 4.4K, `g_uart_slip_ctx` 4.1K, slip_tx/rx 4K, tx/rx 2K, remote_tx/rx 8K, `g_waiters` 8.2K) | ~31 KB | a remote mount or `p9serve` is active |
| lisp file buffers (§2.5) | 24 KB | during one `load`/`cat`/`read-file` |
| shell U-mode probe stacks (`g_user_stack`, `g_echo_ustack`) | 8 KB | `usertest` / `chanecho` |
| line editor (`history_stack` 4K, `ins_buf` 2K) | 6 KB | interactive editing |

The precedent exists three times over — `chibicc/pools.c`,
`search_pools_init()`, `g_chess_scratch` — and `pools.c`'s header already
argues the case in full. What is missing is that it was applied
case-by-case, each time after a failure, rather than as a rule.

**Recommendation.** A tiny shared helper — `subsys_arena_acquire(bytes)` /
`subsys_arena_release()` over `palloc_pages()`, essentially `pools.c`'s
bump allocator lifted out of chibicc — and a stated rule: *a static buffer
larger than 1 KB that is not live at boot belongs in an on-demand arena.*
Because chess, cc, ed, networking and Lisp-heavy work are mutually
exclusive in practice, the effective gain approaches the full 110 KB /
27 pages: it becomes memory that goes to whichever subsystem is active
rather than memory permanently unavailable to all of them.

#### What was done

Taken on for consistency rather than for the bytes -- most of the table above
had already been dealt with piecemeal by §1.3, §2.1 and §2.5, and what was
left was the *pattern* not existing anywhere as a named thing.

**`kernel/scratch.h` / `kernel/scratch.c`.** A `scratch_t` holding base and
page count, with `scratch_acquire()`/`scratch_release()`. The page count is
stored rather than recomputed at release, which is the one thing it exists to
make impossible to get wrong -- `tt.c` and `search.c` each carry a comment
explaining they keep their own count for exactly that reason, having arrived
at it independently.

**The rule needed a second clause.** The version proposed above -- "not live
at boot" -- is necessary but not sufficient, and following it literally would
have been a mistake. The header states it as *not live at boot* **and** *not
on a hot path*. `fs/p9_link.c` is where that bites: its client-side one-shot
buffers moved to the heap and its server-side ones (`resp_buf`, `rx_buf`,
`g_waiters`) deliberately did not, because those are touched once per inbound
frame by a task whose whole job is serving frames.

Converted: `lisp.c`'s four file buffers (replacing §2.5's local copy of the
pattern with the shared one), `kernel/shell.c`'s two U-mode probe stacks,
`kernel/line_editor.c`'s multiline insert buffer, and `p9_link_cat()`'s client
buffers.

**Measured:** `.bss` 191,760 -> **170,072** (21,688 bytes -- more than the
~14 KB of buffers, because the two `aligned(4096)` probe stacks were also
forcing section padding); heap 82 -> **88 pages**.

#### Two bugs the conversion produced, both caught before landing

1. **`sizeof(tx)` silently became `sizeof(uint8_t *)`.** `p9_link_cat()` passed
   `sizeof(tx)` as its receive-buffer bound at six call sites; once `tx` became
   a pointer that is 4, not 2048. Exactly the trap an array-to-pointer
   conversion invites, and it compiles clean.
2. **`chanechotest` leaked a page per re-run.** Found by measuring `Pages Used`
   across three rounds of every probe command rather than trusting the suites,
   which run each once. `kernel/chan.h` has no unregister, so the endpoint
   outlives the probe and a second run has *always* failed at
   `chan_register_task()` -- but it now failed *after* spawning a task and
   taking a page for its stack, and that page cannot be freed with a live task
   standing on it. Fixed by detecting the repeat up front, before anything is
   claimed, with a message that says what is actually true. Verified stable at
   11 pages across three full rounds afterwards.

Both are the same lesson: moving a buffer from `.bss` to the heap changes its
*lifetime*, and lifetime bugs do not announce themselves at compile time. The
release paths are where the care goes -- `run_user_task()` and
`cmd_chan_echo_test()` both refuse to reclaim unless the task is genuinely
`TASK_DEAD`, since the wait loops are capped and "we stopped waiting" is not
the same as "it finished".

**Verified:** QEMU **249/249**, hardware **24/24**, including B3 and B6, which
drive the probe stacks that moved.

### 3.2 Boot-computed constant tables belong in flash *(done, 2026-08-22)*

RP2350 uses ~748 KB of its 4 MB flash (`__flash_binary_end` ≈
`0x100b6970`), including the 512 KB embedded FAT32 payload. **3.3 MB is
free**, and XIP reads are cached.

- `zobrist_pieces[6][2][64]` + `zobrist_castling[16]` +
  `zobrist_en_passant[64]` + `zobrist_side` = **6,792 B**, produced by
  `init_zobrist()` from a fixed-seed xorshift with no inputs. Fully
  determined at build time.
- `bitboard.c`'s pawn/knight/king attack tables and file/rank masks =
  **~2.2 KB**, same property (the magic tables are already `#if
  USE_MAGIC_BITBOARDS`-ed out on this board).

**Recommendation.** Generate both as `static const` arrays at build time
(a host-run generator emitting a `.h`, the same shape as
`cmake/gen_config.cmake` and `gen_build_id.cmake` already use) and delete
the init functions. `init_zobrist()` currently reruns on every session
start; this also removes that.

**Recovers: ~9 KB = 2 pages, and removes work from every chess start.**

#### The performance question, asked first and answered with numbers

The obvious objection: these are read on *every* `make_move()` -- 17 references
in `position.c`'s hot path -- with random indexing, and RP2350 executes from
XIP flash through a small cache that is already holding the engine's own code.
Moving 6.8 KB of hot random-access data in there could plausibly thrash it.

Measurable, as it happens, because **perft updates `hash_key` but never reads
it**: the node counts are unaffected by what the keys contain, so the same
benchmark times the access cost directly. Two runs each on real silicon, same
code, only the tables' residence differing:

| tables in | perft nps |
|---|---:|
| flash (`const`) | 188,671 / 188,597 |
| `.bss` (as before) | 185,683 / 185,468 |

**No regression -- about 1.7% the other way.** The concern is real in principle
and does not bite here: the XIP cache holds a table this size across the
repeated reads a search makes of it, and removing 6.8 KB from `.bss` also
shifts the genuinely hot structures around RP2350's striped SRAM banks, which
is worth a couple of percent on its own. The honest summary is "within a couple
of percent, favouring flash", not "flash is faster".

#### What was done

`tools/gen_zobrist.py` emits `user/chess/include/zobrist_tables.h`, replicating
`init_zobrist()`'s PRNG and draw order exactly. Verified bit-exact against the
running board before switching: `zobrist_pieces[0][0][0]` and `zobrist_side`
printed from the old build match the generator's output digit for digit.

The generator is deliberately **not** wired into the build. These 849 numbers
are a mathematical constant; regenerating them on every compile would only
create a way for them to change by accident. Rerun it and `git diff` should be
empty.

`init_zobrist()` is gone rather than left as an empty stub -- there is nothing
to initialise -- along with its call in `chess_ensure_init()`. The original
names are preserved via macros so every call site in `position.c` is untouched;
macros rather than pointer aliases because the array *types* must survive the
substitution (the engine indexes `zobrist_pieces` three-dimensionally).

**Measured:** `.bss` 170,072 -> **163,264** (6,808 bytes, exactly the tables);
heap 88 -> **89 pages**. Flash grew by the same 6.6 KB, to 731 KB of 4096.

#### Not done: `bitboard.c`'s tables

The remaining ~2.2 KB. Skipped deliberately: it is half a page, and unlike the
Zobrist keys -- a PRNG sequence trivially replicable in twelve lines -- these
come from the mask and attack-generation loops, which would have to be
transcribed into Python and kept in step. The risk of a silent transcription
error in move generation is a bad trade for half a page, especially now that
there are 60 free.

#### Verification

- QEMU **249/249**, hardware **24/24**.
- The mate-in-one probe returns `bestmove e1e8` in **47 nodes** -- the same
  node count as before the change, which is the strongest available evidence
  that the keys are bit-identical, since transposition-table behaviour depends
  on them.
- Worth recording for whoever tests this next: the first attempt returned
  `f2f4` with no `info` line, which looks like a regression and is not. After a
  `fen`, the game history is empty, so `get_book_move()` matches an opening
  line -- Bird's Opening here -- and returns without searching at all. It has a
  time-based entropy source, so it is nondeterministic. Repeat the probe, or
  use a position the book cannot claim.

### 3.3 `vfs_handle_t` is 976 bytes × 8, dominated by a buffer few handles use

`proc_buf[768]` + `rel_path[128]` account for 896 of 976 bytes, and
`proc_buf` is used only by `/proc/*` handles — of which one, occasionally
two, are open at a time. `g_handles` is **7,808 bytes**.

**Recommendation.** A side pool of 2 `proc_buf`s claimed on open of a PROC
handle, with the handle carrying an index. `is_kmsg` handles already read
straight from the klog ring rather than `proc_buf`, so the pool only has
to cover generated files.

**Recovers: ~4.5 KB = 1 page.**

### 3.4 5,598 bytes of intra-`.bss` alignment padding

`kernel/shell.c`'s `g_user_stack[4096]` and `g_echo_ustack[4096]` carry
`aligned(4096)`, which forces the whole `.bss` output section to 4096
alignment (`readelf`: `Al 4096`) and costs a further 1,680 B between
`.data`'s end and `.bss`'s start.

`linker/rp2350.ld`'s `.ustacksN` scheme exists precisely to eliminate this
class of padding, and its comment explains it was "deliberately NOT
applied to `kernel/shell.c`'s `g_user_stack`/`g_echo_ustack`" — but that
reasoning is about *shrinking* the arrays below a page (wrong under Sv39),
not about *placing* them. Placement and grant size are separable.

**Recommendation.** An RP2350-only `section(".ustacks4096")` attribute on
those two arrays, with a matching tier in the linker script above
`.ustacks2048`. Sizes and grants unchanged; only their address moves.
Better still, per §3.1 these two are `usertest`/`chanecho`-only and could
be on-demand entirely.

**Recovers: ~7 KB = 1.7 pages (or 8 KB more if made on-demand).**

### 3.5 `usb_cdc`'s 16 KB NAPOT region — available, not recommended

`g_usb_region` pads `usb_umode_fields_t` (~14.9 KB: EP2 TX 4096, EP2 RX
512, EP4 TX 4096, EP4 RX 6144, plus scalars) to 16,384 so it is a single
legal PMP grant. Bringing the total under 8,192 would halve it.

**Recorded as available but explicitly not recommended for EP2.** This
tree's own commit `42e02e9` and the Full-Speed-behind-a-hub latency it
documents mean console output can sit queued for seconds; the EP2 TX ring
is exactly what absorbs that, and shrinking it converts a latency problem
into a data-loss problem. EP4 (the network ACM) is the trimmable half if
the chess persona does not need the 9P-over-USB link — but that is a
persona question, not a sizing one.

---

## 4. The two architectural questions asked

### 4.1 "Would breaking subsystems into separate processes reclaim heap?"

**No — on this platform it makes peak RAM strictly worse**, and this tree
already contains the measured argument. `user/chibicc/pools.c`'s header,
section "Why this rather than making cc a process", recorded it when C6
proposed exactly this for the compiler:

- **In-kernel code costs zero RAM.** It executes XIP from flash. A user
  ELF's text is *copied into RAM* by `elf.c` (`memcpy` into `p->image`).
  Chess's engine is tens of KB of text that is currently free.
- **NAPOT rounding.** `image_span_pages()` rounds an image to a power-of-
  two page count, and `palloc_pages_aligned()` needs a run aligned to its
  own size. chibicc's 108 KB of pools became a 128 KB region: 20 KB of
  padding at a 128 KB-aligned address — an address a 53-page heap cannot
  offer at all.
- **Per-process overhead** is a 1-page user stack plus a 2-page kernel
  stack on top of the image.
- **`MEM_DOMAIN_MAX_REGIONS` is 5** on RP2350 (8 PMP entries less the 3
  shadowing Hazard3's hardwired U-mode grants), so an image whose segment
  page counts are not powers of two spends regions as well as pages.

### 4.2 "Is running processes in a shared address space viable?"

**It is already what LugalOS does**, and it is the right choice here.
chess, lisp, chibicc and ed are all in `SOURCES` in `CMakeLists.txt`,
linked into the kernel image, running in M-mode in one address space. The
separately-linked U-mode programs (`user/progs/u*.c`) are the isolation
demonstrators, not the heavy subsystems.

What those subsystems lack is not an address space, it is a *task*: they
run on the shell's stack rather than their own. Giving chess its own task
would **cost** 2 pages, not save any. There is no memory argument for it —
only a responsiveness one (a long search currently blocks the shell), and
that is what `search_poll_stop_callback()` already exists to mitigate.

### 4.3 The variant that would actually pay: XIP-resident user programs

If out-of-kernel execution is wanted for **isolation** — and a compiler
consuming arbitrary input is the honest case for it — the way to make it
cheaper than in-kernel rather than more expensive is to stop copying text
into RAM.

PMP regions are physical-address ranges; the XIP window at `0x10000000` is
as grantable as SRAM. A program whose `.text`/`.rodata` sit at a NAPOT-
aligned flash address can be granted `R|X` **in place**, with RAM pages
allocated only for `.data`, `.bss` and stack. That inverts the economics:
such a program would cost *less* permanent RAM than the same code
in-kernel, because its `.bss` becomes on-demand.

Three obstacles, all real, none unsolvable:

1. **Position independence breaks.** `linker/user.ld` links at 0 with
   `-mcmodel=medany` and `-mno-relax`; text and data currently move
   together, so PC-relative data references resolve. With text pinned in
   flash and data at an unrelated RAM address they do not. The fix is
   gp-relative data addressing — set `gp` to the program's data base at
   entry — which means deliberately reversing the current no-gp choice
   for this program class. `linker/user.ld`'s own comment names `gp` as
   the reason `-mno-relax` is passed, so this is a known seam, not a
   surprise.
2. **The exit stub is written into the text page.** `install_exit_stub()`
   patches 16 bytes into the last bytes of page 0, and `user.ld`'s
   `ASSERT(_utext_end <= 4080)` encodes that. Flash is not writable at
   runtime. The stub must move to the shared `.utext` page, or become a
   return address planted on the user stack at `argv` setup time.
3. **A flash region for program images** is needed. The build already
   embeds a 512 KB FAT32 payload at `g_flash_fs_start` in `.rodata`; a
   NAPOT-aligned program area alongside it is the same mechanism, and
   `flash_fs.c` is already generated per build.

**Scope this as its own phase, and only if isolation is the goal.** It
does not belong in a memory-reclamation effort — §1 and §2 alone are
worth more RAM, sooner, at a fraction of the risk.

---

## 5. What this adds up to

Two separate effects, worth not conflating: freeing `.bss` **grows the
heap** for everyone, and re-sizing chess's pools **lowers its demand**.

**Heap grows (static RAM returned):**

| item | § | pages |
|---|---|---:|
| `g_longest` (balloc tree shrink) | 1.1 | 1.5 |
| scratch banks SRAM8/9 | 1.2 | 2 |
| chess `temp_pos` ×3 | 1.3 | 6 |
| `P9_MAX_MSIZE` 2048 | 2.3 | 5 |
| `string_pool` two-tier | 2.4 | 7 |
| lisp file buffers on-demand | 2.5 | 6 |
| zobrist + bitboards → flash | 3.2 | 2 |
| `vfs_handle_t.proc_buf` pool | 3.3 | 1 |
| `.bss` alignment padding | 3.4 | 1.7 |
| **subtotal** | | **~32 pages (+128 KB)** |

**Chess's peak demand falls:**

| item | § | pages |
|---|---|---:|
| balloc arena 16 → 4 (or 0, lazy) | 1.1 | −12 to −16 |
| move-list pools 17 → 6 | 2.1 | −11 |
| scratch `Position` 3 → 2 | 2.2 | −1 |
| **subtotal** | | **−24 to −28 pages** |

Net: a heap of roughly **85 pages** with chess peaking near **26** — from
53/53 with zero margin to about 26/85, ~59 pages (236 KB) of headroom.
§3.1's broader audit is on top of that again.

**Suggested order.** §1.1 and §1.2 first — they are the cheapest and
together already restore ~18 pages, which is enough margin to work in.
Then §1.3 and §2.1, which are chess-local and independently testable on
QEMU. §2.3's iounit bug should be fixed on its own merits whether or not
msize is lowered. §2.4/§2.5 and §3 after that.

---

## 6. Preventing the recurrence *(done, 2026-08-22)*

Every regression in this file's history — S4's `NODE_POOL_SIZE` growth,
and the condition that prompted this review — was discovered by a board
failing to start chess, weeks after the commit that caused it. The heap
size is known at **link** time.

1. **A link-time floor.** In `linker/rp2350.ld`:
   ```ld
   ASSERT(_heap_end - _kernel_end >= 128K,
          "RP2350 heap below its floor -- .bss grew; see plan/phase15_memory_reclamation.md")
   ```
   A `.bss` growth then fails the build, naming the cause, instead of
   surfacing as "chess: out of memory (search move-list pools)" later.
2. **A `sizereport` build target** — the `nm -S -l` grouping used at the
   top of this document, plus a checked-in baseline — so a commit's RAM
   delta is visible in review rather than reconstructed by hand each time
   it bites. This analysis has now been redone from scratch three times
   (phase 9 H4, phase 13's follow-up, and here).
3. **`/proc/meminfo` should report the static side**, not only heap pages
   and peak. `meminfo_ram_map()` already has `image_bytes`; breaking it
   into `.data` / `.bss` / ustacks / boot stack makes "what moved" an
   on-board question rather than a host-toolchain one.
4. **Measure task stacks.** `stack_used_bytes()` already implements the
   poison-scan high-water mark for the boot stack. Applying the same
   priming and scan to `palloc`'d task stacks would answer whether
   `elf.c:750`'s `task_create("uprog", ...)` really needs the 2-page
   default (its body sets up a domain and traps to U-mode; the deep call
   chains that justified `TASK_STACK_PAGES = 2` are the Lisp evaluator
   and the re-entrant 9P server, neither of which is on this path), and
   would let several driver tasks shrink with evidence rather than
   guesswork.

---

## Follow-up: the input-eater, and one session across both input devices *(done, 2026-08-22)*

Two things, requested together after the §2.1 measurement work kept losing
typed input to the board.

### The input-eater

**Not architectural residue from lsh and chess having been one program**, which
was the initial guess. `kernel/console.c`'s `console_interrupt_requested()`
drained the whole input device hunting for Ctrl-C and *discarded every byte
that was not one*. `search.c`'s `check_up_time()` polls it every 2048 nodes, so
during any engine think every keystroke was actively consumed and destroyed by
the interrupt check. The driver rings (512 B on USB CDC) would have held them
perfectly well.

The header had even documented the discard as "a real, explicit tradeoff:
there is no push-back/ungetc() on the underlying read". The fix was to add the
push-back it named as missing: a bounded ring in the console layer, and one
`console_pump()` that every reader funnels through.

Two mistakes on the way there, both worth recording because both were
self-inflicted and both were caught by the suites rather than by reasoning:

1. **The first pump drained the device dry into a 128-byte ring**, dropping the
   overflow -- reintroducing the exact input-eating being fixed, one layer
   down. The test runner submits multi-line command blocks well over that, and
   their tails vanished. The pump now stops when the ring is full and leaves
   the surplus in the device, where the UART's own buffering holds it.
2. **The first policy was "Ctrl-C is a signal, never data"**, which read
   cleanly and broke `edit_multiline_box()`, whose documented exit is Ctrl-X
   Ctrl-C -- it consumes the 0x03 as an ordinary character. Swallowing it made
   the editor impossible to leave; it redrew forever. The policy is now *latch
   and deliver*: latching where bytes enter, rather than where they are
   consumed, is what makes the latch reliable without the byte being stolen.

Also found: three readers bypassed the new ring entirely (`user/lisp/lisp.c`,
`user/ed/ed.c`, `fs/vfs_server.c` all called `uart_getc()` directly), which
stranded input mid-session. All now go through `console_getc()`.

### `chess_next_event()`

`readline_poll()` (kernel/line_editor.c) is a non-blocking line reader sharing
`readline_interactive()`'s editor *exactly* -- one body, two drivers -- rather
than a cut-down reader that would have quietly lost history, cursor movement
and the multiline escape. `chess_next_event()` polls abort, then a console
line, then the keypad, and reports whichever speaks first.

`tm_wait_key()` was the single chokepoint for keypad input in the whole board
UI, so routing it through the event source gives terminal commands *everywhere*
the board loop waits -- between moves, mid square-selection, inside a menu.
`console_dispatch_line()` was lifted out of `chess_console_run()` so a command
means the same thing from either device.

**Verified on the board:** `(chess)` shows `chess[board]> `; `level 4` answers
"Search level set to 4 (10s per move)"; `fen` prints the live position; Ctrl-C
returns to `lsh`. Type-ahead during a 5-second search survives and executes
after it.

**A regression guard that actually guards.** The first version of the
type-ahead test went into `tests/runner.py` and **passed with the bug
deliberately reintroduced** -- worthless. QEMU's 16550 has a 16-byte FIFO and
its chardev does not hand a host-side write to the guest until the guest
reads, so the typed-ahead bytes are never present during the search for the
drain to eat. That is precisely why this bug was only ever observable on
hardware. The test moved to `tests/hw/test_rp2350.py`, where it was
negative-tested properly: it **fails** with the eating behaviour flashed and
passes with the fix.

**Suites:** QEMU 247/247, hardware 24/24.

---

## Where §2 leaves the board *(2026-08-22)*

Measured on the chess persona after §2.3-§2.5, fresh boot:

```
  Pages Total: 82          (53 at the start of this phase)
  Pages Used:  11 at idle
  Pages Peak:  28          (53 of 53 at the start -- no spare pages at all)
  RAM: 520 KB total
  Image (data+bss): 173 KB (283 KB at the start)
  Heap: 328 KB managed     (212 KB at the start)
```

**A small observer effect worth knowing about:** the idle "Pages Used" figure
rose from 9 to 11 across §2.5, and none of that is a leak. `cat /proc/meminfo`
*is* the Lisp `cat` primitive, which now takes its 4 KB buffer from the heap
and holds it while `vfs_read()` generates the very report being printed. The
number therefore includes the page the reporting command is standing on.

| | heap | chess peak |
|---|---:|---:|
| phase start | 53 | 53 / 53 |
| after §1.1-§1.3 | 62 | 37 -> 26 |
| after §2.1 | 62 | 26 |
| after §2.3 | 69 | -- |
| after §2.4 | 76 | -- |
| after §2.5 | **82** | **28 / 82** |

Suites throughout: QEMU **249/249**, hardware **24/24**.

The remaining plan items (§3's structural audit, §6's link-time floor) are now
optional rather than urgent. §6 in particular is worth doing *before* the
margin gets spent: the whole point of a floor is that it exists while there is
still room above it.

---

## §6 as built *(2026-08-22)*

All four items, and each one negative-tested where it could be -- a safeguard
that cannot fail is decoration.

### The link-time floor

`linker/rp2350.ld` now carries
`ASSERT(_heap_end - _kernel_end >= 256K, ...)`.

256 KB is set against measurements, not chosen as a round number:

  - the whole system's observed peak, two U-mode programs resident at once, is
    38 pages / 152 KB (`test_rp2350.py`'s C2);
  - the largest user image this board can place needs a 32-page (128 KB)
    NAPOT-aligned run, which is a *contiguous* requirement on top of that, not
    a share of it.

64 pages leaves both satisfiable together. The heap is 89 pages today, so there
are 25 pages of slack: enough that ordinary work never trips it, little enough
that a subsystem quietly claiming 100 KB of `.bss` does. The comment says
explicitly that the response to it firing is not to lower it reflexively.

**Negative-tested:** 110 KB of `.bss` added to `kernel/palloc.c` makes the link
fail with the message naming the cause; removing it links clean.

RP2350 only. The QEMU targets link into 128 MB where a floor would never fire,
and the clock persona shares this script.

### `sizereport` / `sizecheck`

`tools/sizereport.py` plus two cmake targets. The floor catches a regression;
this attributes it -- the per-source-file breakdown that had been redone by
hand three times (phase 9's H4, phase 13's follow-up, and this phase).

`sizecheck` compares against `tools/sizereport-rp2350.json` and **exits
non-zero if static RAM grew at all**. Deliberately blunt: growth here is not a
neutral fact about the image, it is heap nobody else gets. Intended growth
means running `--update` and having the new numbers land in a reviewable diff.

**Negative-tested:** a 9,000-byte array produces
`kernel/palloc.c  +9000` and `FAIL: static RAM grew by 9000 bytes (2.2 heap
pages)`, exit 1; removing it returns `+0`, exit 0.

### `/proc/meminfo` reports the static side

`Image (data+bss)` was one number: enough to see *that* the static side moved,
useless for seeing *what* moved. It now breaks down:

```
  Image (data+bss): 144 KB
    .data 624 B, .bss 121 KB, ustacks 21 KB
```

`ustacks` is RP2350-only -- the driver U-mode PMP regions, 21 KB of RAM that is
neither `.data` nor `.bss` and was invisible in the image figure until now.

**A bug found by reading the output rather than the script:** the first version
bracketed the tiers with `_ustacks_start = .;` before `.ustacks16384`. Every
section above that point is assigned `> FLASH`, so `.` was a *flash* address
and the board reported `ustacks 261435 KB`. Computed from
`ADDR()`/`SIZEOF()` now, which does not depend on where the location counter
happens to be.

### Per-task stack high-water

`task_create_sized()` paints each stack with `STACK_POISON`, and
`sched_stack_used()`/`sched_stack_size()` scan it the same way `meminfo.c` has
always scanned the boot stack. `/proc/ps` gained a Stack column. The poison
word moved into `kernel/meminfo.h` so the paint and every reader of it cannot
disagree.

This existed only for the boot stack before, where reading it is what found
that the RP2350 boot path had been running on the wrong stack entirely. Every
per-task size in the tree -- `TASK_STACK_PAGES`, and the 1-page choice each
driver task makes -- was a judgement with no way to check it. Measured on the
board after a `chess-selftest`:

```
  1  usbcdc     492/4096 B      5  i2c        544/4096 B
  2  uart       556/4096 B      6  st7735     544/4096 B
  3  heartbeat  492/4096 B      7  tm1638     544/4096 B
  4  sdblk      544/4096 B      8  p9srv     2788/8192 B
```

**Deliberately not acted on here.** Driver tasks touching ~550 bytes of a 4096
page says their stacks could be an eighth the size, which would be another
7 pages -- but PAGE_SIZE is the allocator's granularity, so realising it means
sub-page task stacks, which is a change to `palloc`/`sched`, not a constant.
Recorded as evidence for whoever wants it; the instrument is the deliverable.

**Verified:** QEMU **249/249**, hardware **24/24**.
