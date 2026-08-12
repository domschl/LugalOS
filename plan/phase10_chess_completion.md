# Phase 10 — Chess completion (console REPL, game-outcome logic, TM1638 menus, perft, UCI)

**Status:** planned, 2026-08-12. Not started. Written from the user's own raw
proposal plus a re-read of `~/gith/domschl/LugalChess/engine/src/console.c`
(1637 lines) and `uci.c` (174 lines), against the current state of
`user/chess/` after phase9 H1-H4.

**What phase9 actually shipped vs. what "play a game of chess" needs.**
H4 deliberately did not vendor `console.c` at all (its own text: "~150
hand-edited printf call sites and ~1600 lines for behavior... explicitly out
of scope"). What it built instead, `chess_ui.c`'s `chess_run()`, is scenario
3 from the user's own framing minus the parts `console.c` handles that aren't
just I/O plumbing: it plays a full TM1638+ST7735 game, but a checkmate ends
it in an infinite `for(;;) time_delay_us(...)` loop printing "gAME OuEr" with
no distinction from a draw or a resignation, promotions always default to
Queen, and there is no way to undo a move, change search depth, inspect the
board, or save a game. **Scenario 1 (serial-terminal-only play, no hardware
at all) does not exist in LugalOS yet** — `chess-selftest` runs one fixed
search and exits; there is no interactive text REPL. This is a bigger gap
than "check/mate announcement and a promotion selector," which is what the
user's own notes named — those two items plus the full usability-command set
(`board`, `level`, `fen`, `save`/`load`, `go`, `stop`, `undo`, `redo`, `eval`,
`moves`, `perft`) are all missing for **every** front end, not just the
TM1638 one, because none of `console.c`'s command logic was ported at all.

**This phase folds in all three of [[phase9_chess_followups]]'s deferred
items** rather than scheduling them separately — they turn out to be
sub-parts of the same integration work, not independent cleanup:
1. Heap-on-demand for `search.c`'s static move-list pools is J0's opening
   move, because it's what makes "`chess` becomes a by-default available
   command" (the user's own framing) actually free on every board.
2. TM1638 function keys 8-15 (J3) are meaningless without the game-outcome
   and level-select state J1/J2 build first — the keys navigate menus that
   call into exactly that logic.
3. `perft` in the test suite (J4) is the same command the user asks to add
   as `perft [n]`, just wired into `tests/runner.py` at a bounded depth.

**A finding that shrinks this phase's riskiest item.** Phase9 H0 flagged the
UCI `info depth ... nps %.0f ... nodes %ld` line as the one real stdio-format
gap standing between the engine and a GUI bridge, and phase9's "out of
scope" section deferred the whole UCI bridge on that basis. Re-reading
`search.c` as vendored (not the upstream file) shows H4 already fixed this
as a side effect of its own `printf -> cprintf` compatibility pass: the `nps`
figure is computed as an integer and printed with `%d`/`%ld`, not `%.0f`
(`user/chess/src/search.c:692-707`, with the rationale recorded in a comment
at the top of that file). **The stdio blocker phase9 deferred UCI over no
longer exists** — J5 (UCI-over-UART) is now a device-wiring task, not an
engine-surgery task. Confirmed by reading the file, not assumed from the old
finding.

---

## J0 — `chess`/`perft` as always-on commands + heap-on-demand search pools *(partly done, 2026-08-12)*

**Landed:** the heap-on-demand pool conversion (item 1) and a `chess` Lisp
primitive (item 2, scoped down — see below). **Not landed:** `perft`, which
stays J4's job since `run_perft_tests_depth()` doesn't exist in
`user/chess/` yet — registering a primitive with nothing real behind it
would just be dead code.

`chess_ensure_init()` (`chess_ui.c`) now returns `bool` and calls the new
`search_pools_init()` (`search.c`), failing loudly via `cprintf` rather than
crashing later if the allocation is ever refused; `chess_selftest()` and
`chess_run()` both check the return value. The `chess` primitive
(`user/lisp/lisp.c`) dispatches to `chess_run()` where the hardware for it
exists, and otherwise — deliberately, since J1's console REPL doesn't exist
yet — prints that text play isn't available yet rather than silently
substituting `chess-selftest`'s fixed-position benchmark, which is not
"playing chess" and would have been a misleading stand-in; `chess-run`/
`chess-selftest` keep working under their existing names either way. Swap
the fallback branch for a real `chess_console_run()` call once J1 lands.

**Two things found in the course of verifying this, neither part of J0's
own diff:**

1. **Fixed, pre-existing, unrelated to heap-on-demand:** `search.c`'s
   iterative-deepening loop computes `nps` as `(nodes_searched * 1000) /
   time_spent` in a plain `long` (`search.c`, the UCI `info` line). On a
   32-bit `long` target (rv32 QEMU, RP2350 — not rv64, whose `long` is 64
   bits) this overflows a signed 32-bit int once `nodes_searched` passes
   ~2.1M, which a `chess-selftest` run reaches by search depth 10 on QEMU's
   host-speed emulation (H4's own ~19.8M nodes/s figure) — confirmed live,
   `UBSAN_PANIC` halting the system mid-search. Latent since H4 landed;
   never triggered before because nobody had run a search deep enough in
   the same session. Fixed with a 64-bit intermediate
   (`(int64_t)nodes_searched * 1000`), narrowed back to `long` for the
   unchanged `%ld` format spec.
2. **Found and fixed, root-caused rather than left as a guess:** past
   depth 11 on the same QEMU smoke test, the search appeared to stall
   indefinitely (100% host CPU, no further `info depth` line after several
   real minutes) on the fixed midgame FEN `chess_selftest()` uses. The
   user's own instinct — that neither "engine bug" nor "genuine branching
   blowup" was a probable explanation, given the engine's otherwise
   well-tested behavior through depth 11 — was correct on investigation:
   there was no search bug at all. The actual cause was
   `kernel/time.c`'s `read_hardware_counter_us()`, which had no real timer
   backing on non-RP2350 targets — its `#else` branch was a bare call
   counter (`return ++g_soft_us * 100;`), entirely decoupled from real
   wall-clock time. `search.c`'s time-based cutoff (`check_up_time()`,
   `max_search_time_ms`) is driven entirely by this counter, so
   `chess_selftest()`'s nominal "2000 ms" budget on QEMU actually meant
   "however many `search_poll_stop_callback()` polls it takes to
   accumulate 2000 fake-ms" — roughly 41M real nodes at the observed
   conversion rate (2048-node poll interval × 100 fake-us/poll), not 2 real
   seconds. Once the fake budget was that loose, the search simply kept
   iterative-deepening productively (still making real progress, hence
   100% CPU with no actual hang) into territory where per-depth node counts
   grow the way exponential game-tree search always does at that many
   plies — nothing pathological about this specific FEN, just an
   effectively unbounded budget meeting normal exponential growth. This is
   also why H4's own testing only ever reported reaching depth 9 on QEMU
   (plan/phase9_chess_computer.md) — it never pushed further, so this was
   never exposed; real RP2350 hardware has a genuine timer here
   (`TIMER0_TIMEHR`/`TIMELR`) and was never affected.

   **Fixed** by giving `read_hardware_counter_us()` a real counter on QEMU
   too, reusing hardware `kernel/ticker.c` already reads for its own
   preemption deadline rather than inventing a new source: QEMU RV32
   (M-mode) reads the CLINT's `MTIME` register directly (`CLINT_BASE
   0x02000000` + `0xBFF8`, the same address ticker.c's own `now()` uses);
   QEMU RV64 (S-mode, where the CLINT's raw MMIO is M-mode-only) uses the
   Sstc `rdtime` pseudo-instruction instead, again mirroring ticker.c's own
   S-mode branch. Both run at the virt machine's documented, fixed 10 MHz
   (`ticker.c`'s own `TICK_HZ`), so `/ 10` converts ticks to microseconds.
   K210 (S-mode, no board file yet, per `CMakeLists.txt`'s own comment)
   lands in the same `rdtime` bucket ticker.c already puts it in — an
   existing simplification this fix didn't introduce.

   **Confirmed fixed, not just plausible:** re-running the identical
   `chess-selftest` smoke test after this change, twice, now stops
   deterministically at depth 7 completed / depth 8 cut short partway
   through (real elapsed ~2.0s both times, `nps` in the honest low-100
   thousands rather than the fake 20M/s QEMU host-speed figure), instead of
   running past depth 11 indefinitely. No further engine investigation
   needed — the search algorithm was never the problem.

**Verify:** all three targets (`rv32`, `rv64`, `rp2350`) build clean with no
new warnings, both before and after the `kernel/time.c` fix; 181/181 QEMU
regression tests pass unchanged both times (nothing in the suite depended on
the old fake-counter's specific values, only relative ordering/delays, which
a real counter satisfies too). Functional correctness of the on-demand pools
confirmed by the same QEMU run that surfaced both findings above: an
11-ply-deep real search (4.8M+ nodes, `pv_search`/`quiescence` recursing
through every ply 0-20+ and dereferencing `search_pv_movelists`/
`search_q_movelists` at each one) produced a clean, sane, consistently
deepening PV with no crash or corruption — the allocation and pointer
indexing are correct. The timer fix itself verified by re-running
`chess-selftest` twice post-fix and observing deterministic ~2-real-second
completion (see above) where it previously ran unbounded.
`tests/hw/test_rp2350.py`'s C6/C7 comment updated to describe the pool
conversion and explain why its numeric threshold (`>= 45`) is left as a
safe but no-longer-tight lower bound rather than guessed at — this session
had no RP2350 hardware attached to re-measure the actual new idle-heap
figure (unaffected by the timer fix, which only touches non-RP2350
targets); tightening it is a follow-up for whoever next runs the hardware
suite.

### J0's original plan text (kept for the reasoning; superseded by the "partly done" note above for current status)

Two changes, landed together because the second is what makes the first free:

1. `search.c`'s `search_pv_movelists`/`search_q_movelists` (~65 KB static
   `.bss`, phase9 H4's own finding, dropped RP2350's managed heap from 78 to
   48 pages) move to `palloc_pages()`-on-demand, allocated once on first
   `chess_ensure_init()` call and never freed for the process lifetime —
   matching `cc`/`ed`'s own precedent (phase8) rather than reinventing a
   different lifetime policy. This is the concrete mechanism behind the
   user's "default-cost is just additional flash" framing: with this change,
   `LUGALOS_ENABLE_CHESS=ON` by default costs image size only, not permanent
   RAM, exactly like `LUGALOS_ENABLE_CC`/`LUGALOS_ENABLE_ED` already do.
2. New Lisp primitives `chess` (dispatches to the console REPL of J1, or to
   `chess_run()` if `CONFIG_ENABLE_DISPLAY && CONFIG_ENABLE_TM1638`, per the
   user's own "initializing the available optional hardware" framing) and
   `perft` (J4). No `shell.c` dispatch needed — `chess-selftest`/`chess-run`
   already work today as bare-word shell commands through the existing
   POSIX-to-S-expression transform (`kernel/shell.c`'s command line ->
   `(chess-selftest)` translation), the same path `cc`/`ed` use; `ed` is the
   one command with an explicit `strcmp` branch in `shell.c:601-602`, and
   only because `ed_main()` needs the raw line-editor mode `kernel/
   line_editor.c` provides — `chess`/`perft` don't need that, they're plain
   blocking function calls like `chess-selftest` already is.

**Verify:** default build (`LUGALOS_ENABLE_CHESS=ON`, the default) reports
the same managed-heap page count as an `OFF` build until `chess`/`perft` is
actually invoked once; `tests/hw/test_rp2350.py`'s C6/C7 threshold (already
lowered from `>=70` to `>=45` in H4 for the static-pool cost) reverts back
towards its pre-H4 value once this pool is on-demand — an explicit test
change to make, not just a side effect to notice.

## J1 — Console REPL: the missing baseline scenario *(done, 2026-08-12)*

Landed close to the design below, with `/sd0`-vs-`/ram0` save-path
auto-detection (found live to be necessary, see findings) as the one real
addition. `chess_console_run()` is wired into the existing `chess` Lisp
primitive's fallback branch (J0) — `(chess)` on any target without
display+TM1638 hardware now runs it for real, in place of J0's placeholder
message.

**Three bugs found while verifying this, none part of J1's own design,
all fixed:**

1. **`cprintf` has no `%+d` (no flag characters at all)**, confirmed live:
   `Score: %+d` printed the literal four characters `%+d`, not a signed
   number. `chess_platform.h`'s own header comment already documented this
   exact gap for `%.0f`; `+` is the same category of unsupported
   `printf`-flag, just never hit before because nothing printed a
   deliberately-signed integer through `cprintf` until this console did.
   Fixed with a two-line `sign_prefix()` helper (`%s%d`) rather than
   extending `cprintf` itself, which is out of scope for a chess milestone.
2. **`vfs_write()` doesn't create missing parent directories**, confirmed
   live: `/ram0/system/chess.save` failed outright on a freshly-formatted
   `/ram0` with no `system/` subdirectory yet (`ls /ram0/` showed `(empty
   directory)`). Fixed two ways at once: `console_save()` now calls
   `vfs_mkdir()` on the target directory first (ignoring the result — it
   either already existed or now does), and the save path itself became
   runtime-selected (`vfs_volume_writable("/sd0")`) rather than hardcoded to
   `/ram0` — `init.lisp` only ever mounts a RAM disk *when `/sd0` isn't
   already writable*, so a board with a working SD card never mounts
   `/ram0` at all, and the original hardcoded path would have failed with
   "no such volume" on exactly the boards most likely to have persistent
   storage worth saving to.
3. **A real, pre-existing, cross-target crash — the most serious of the
   three.** QEMU RV64 (`-march=rv64gc -mabi=lp64d`, real hardware F/D
   register ABI, unlike RV32's soft-float `ilp32`) trapped Illegal
   Instruction on the very first `fsd` (floating-point register spill) in
   `search_position()`'s own prologue, the instant this milestone's new
   QEMU test became the first thing ever to call chess search on RV64 QEMU
   — phase9 H4's own verification only ever ran `chess-selftest` on RV32
   QEMU and real RP2350 hardware (both soft-float), and `tests/runner.py`
   had zero chess coverage before this milestone (J0's own finding). Root
   cause: `search_position()`'s dynamic time-cutoff heuristic used `double`
   arithmetic (`search.c`, branching-factor estimate `b`), and LugalOS's
   boot code never sets `mstatus.FS` to enable the FPU at all — so any
   target whose ABI makes GCC reach for real FP registers (RV64's `lp64d`;
   RV32's `ilp32` never does, hence never crashed) faults immediately.
   Fixed by removing the float dependency instead of enabling the FPU:
   `b` is now fixed-point (tenths) integer arithmetic, sufficient for a
   `>=` comparison and consistent with this same function's own `nps`
   calculation a few lines up (already integer, same reasoning, from H4).
   Enabling the FPU kernel-wide was deliberately not attempted as the fix —
   it would have opened a second, unrelated question (nothing currently
   saves/restores FP register state across a preemptive task switch, B6)
   for a heuristic that doesn't need hardware float at all.

A new entry point, `chess_console_run()` in `chess_ui.c`, ported from
`console.c`'s `console_loop()` (`~/gith/domschl/LugalChess/engine/src/
console.c:1176-1636`) minus everything gated on `LUGALCHESS_EMBEDDED` or
`is_uci_client_mode` — this is the plain-terminal REPL only, scenario 1.1.
No hardware dependency: builds and is fully testable on every QEMU target,
same category as `chess-selftest` today.

Command set, each a near-direct port of its `console.c` handler (line
references into the upstream file): `help` (:388), `new`/`board`/`d` (:1304,
:1323), `level <1-8>` (:1327, using the same `level_times_ms[8]` table),
`fen [FEN]` (:1347), `save`/`load` (:1371, :1388 — **rewritten** against
`vfs_write()`/`vfs_read()`, e.g. `/ram0/system/chess.save` or `/sd0/...` if
mounted, storing `FEN\n<level>\n` as plain text; this replaces the original's
raw `hardware/flash.h` sector write, which doesn't exist on LugalOS and
wouldn't be the right primitive even if it did — H0 flagged this exact
`console.c` incompatibility and it's why `console.c` wasn't vendored
wholesale), `go [depth N | movetime N]` (:1441), `undo`/`redo` (:1523,
:1538), `eval` (:1557), `moves` (:1561), `quit`, and bare `<move>` UCI-format
input (`execute_player_move()`, :415-477, ported as-is — it's pure
board/movegen logic, no stdio). `stop` (:1603) is a no-op placeholder in
`console.c` too outside the embedded build (search runs synchronously on the
calling stack, same as `chess_ui.c`'s current `search_poll_stop_callback()`
— nothing to interrupt until a real async search exists, out of scope here).

Line input goes through `readline_interactive()` (`kernel/line_editor.c`,
the same primitive `lsh` itself uses at `kernel/shell.c:881`), not a
hand-rolled reader — `console.c`'s own `get_line_custom()` (:609-978) is
~370 lines specifically because it interleaves keypad and stdio input in one
function; the console-only path doesn't need that split.

**Verify:** all three targets build clean. A new `tests/runner.py` case
(`#10b`) drives `(chess)` through `new`/`level 1`/a move (which triggers a
real engine reply, the actual thing that exposed finding 3 above)/`board`/
`eval`/`moves`/`undo`/`redo`/`fen`/`save`/`load`/`quit` — the first time any
automated test exercises chess beyond `chess-selftest`'s single fixed
search, closing the coverage gap J0's own memory note flagged. 183/183 QEMU
tests pass (181 + the new case on both RV32 and RV64). Manually verified
live over a raw QEMU RV32 session first (every command, including a real
save/load round-trip that landed on `/sd0` — this build's QEMU persona has
one mounted and writable, exercising the `/sd0`-preferred branch of finding
2's fix, not just its `/ram0` fallback) before the automated test was
written from that same working session. RP2350 hardware was reachable
earlier the same session (J0's own hardware verification) but had gone
unresponsive at the USB level by the time J1 was ready to verify there —
not blocking, since none of J1's diff touches RP2350-specific code paths
(`chess_run()`/TM1638/ST7735 are untouched) and RP2350 is `ilp32`
soft-float like RV32, which the float-removal fix (finding 3) already
confirmed working; worth a hardware re-run next time the board's reachable,
but not a gap in what J1 itself needed proven.

## J2 — Game-outcome detection, check/mate/draw announcement, promotion selector, Ctrl-C interrupt

The actual "unfinished integration" the user's notes name, and the piece
both J1 and the existing `chess_run()` are missing today. Lives in
`chess_ui.c` as shared helpers (not duplicated per front end), ported from
`console.c`'s equivalents — all of them pure position-logic, no stdio, built
entirely from primitives H4 already vendored (`generate_moves`, `make_move`/
`unmake_move`, `is_square_attacked`):

- `chess_game_outcome()`, from `check_and_display_game_over()`
  (`console.c:1031-1098`): generates moves, confirms at least one is legal
  via a `make_move`/`unmake_move` probe, and on zero legal moves distinguishes
  checkmate (`is_square_attacked` on own king) from stalemate; separately
  checks `is_threefold_repetition()` (:1011-1029, a linear scan of
  `pos->history[]` for a matching `hash_key`, bounded by `pos->halfmove`) and
  the 50-move rule (`pos->halfmove >= 100`). Returns an enum (`ONGOING`,
  `CHECKMATE_WHITE`, `CHECKMATE_BLACK`, `STALEMATE`, `DRAW_REPETITION`,
  `DRAW_50MOVE`) rather than printing directly, so J1's text output and J3's
  TM1638 8-character status strings (`"nAtE bL "`, `"drAU    "`, etc. —
  `console.c:1049-1094`'s literal display strings) can both consume it.
- `chess_in_check()`, from `check_and_update_check_status()` (:993-1009): one
  `is_square_attacked` call, same shape.
- Promotion piece selection: J1's console path reuses `execute_player_move()`
  (:415-477)'s existing "only-promo defaults to Queen unless the move string
  names one" logic verbatim (`e7e8q` etc., already the UCI move-string
  convention). J3's TM1638 path ports the keypad selector at `console.c:
  651-677` — after a 4th square-entry key completes a move that
  `is_player_move_promotion()` (:589-605, itself a ~10-line port: pawn from a
  back-rank-adjacent square moving to rank 1 or 8) recognizes as a promotion,
  display `"1n2b3r4q"` and read one more key (0-3) mapping to
  knight/bishop/rook/queen — `chess_ui.c`'s `tm_read_move()`
  (`user/chess/src/chess_ui.c:287-308`) currently hard-codes Queen and skips
  this step entirely; this milestone is what removes that hard-code.

`chess_run()` and the new `chess_console_run()` both call
`chess_game_outcome()` after every half-move (player's and engine's) and stop
play with a real status message instead of `chess_run()`'s current
undifferentiated `"gAME OuEr"` infinite loop (`chess_ui.c:341-345`).

**Verify:** QEMU tests driving known checkmate/stalemate/threefold FENs
through `chess_console_run()` (J1's scripted-input harness) and asserting the
reported outcome matches (e.g. a standard fool's-mate or scholar's-mate
sequence, cheap and deterministic — no search needed for the *detection*
side, only for whether the engine still finds a legal reply in ongoing
positions).

**Stopping an unbounded search — polling, not preemption (revised
2026-08-12; an earlier draft of this plan wrongly filed this under "out of
scope").** Level 8 (`level_times_ms[8-1] == -1`, J3) and a bare `go` with no
`movetime`/`depth` both leave `search_position()`'s own time cutoff disabled
— `search.c:636`'s `max_search_time_ms` is `-1`, so `check_up_time()`
(`search.c:310-320`) never sets `stop_search` from its time check
(:314-317). The **only** thing that can end such a search is `stop_search`
becoming true some other way, and `search.c` already calls exactly the hook
for that: `check_up_time()` invokes `extern search_poll_stop_callback(void)`
every 2048 nodes (`nodes_searched & 2047`, :310-313) — a **cooperative poll
already wired into the engine's inner loop**, not something this phase needs
to add. `chess_ui.c`'s current body of that callback is an empty stub
(`chess_ui.c:77-83`, "Nothing to poll yet in this first version"); this
milestone is what fills it in.

**A general Ctrl-C primitive, not a chess-specific one (added 2026-08-12,
user's own suggestion).** Rather than have `chess_ui.c` invent its own
"accumulate bytes into a line, compare against `stop`" text parser (what an
earlier draft of this section proposed, mirroring `console.c`'s
`poll_cmd_buf`, :294-318), the right home for a non-blocking interrupt check
is one level down, in `kernel/console.c` — the file that already owns the
console *output* stream (`console_bind`/`cprintf`) and is the natural place
for its input-side counterpart. Two small additions:

```c
/* kernel/include/kernel/console.h */
bool console_interrupt_requested(void);  // non-blocking; latches until cleared
void console_interrupt_clear(void);
```

Implementation drains whatever's waiting via the existing non-blocking
`uart_has_char()`/`uart_getc()` pair (`drivers/uart_rp2350.c`, already
unifies the physical UART0 wire and USB CDC ACM0 behind one check — the
same primitive J1 was going to reach for anyway), latching a static flag the
instant it sees a raw `0x03` (Ctrl-C) byte and discarding everything else it
drains along the way. That discard is a real, explicit tradeoff worth
stating rather than hiding: `uart_getc()` has no push-back/`ungetc()`, so
there is no way to non-destructively peek at a byte and decide whether to
consume it — anything typed *during* a synchronous foreground command that
isn't a Ctrl-C is silently dropped, not queued for the next prompt. This is
not a new limitation this milestone introduces — `console.c`'s own upstream
`poll_cmd_buf` design already accepted exactly this (type-ahead during `go`
was never preserved there either) — but a bare single-byte Ctrl-C is a
strict improvement over the line-based `stop` design it replaces: one atomic
byte can't be garbled by interleaving the way a 4-5 character word typed
across two non-blocking polls could be.

Two consumers, one primitive:

- **Chess's `search_poll_stop_callback()`** calls
  `console_interrupt_requested()` in place of the bespoke line parser, and
  separately checks `tm1638_get_key() == 11` (the TM1638 STOP key — a
  physical-keypad gesture with no terminal equivalent, so it stays a direct
  check alongside the Ctrl-C one rather than being folded into it) exactly
  as `console.c:320-327` does, wait-for-release debounce included. Either
  sets `stop_search = true`. The typed `stop`/`s`/`q`/`quit` REPL command
  from J1 (ported from `console.c:1603-1606`) stays too, but honestly as a
  UCI-protocol-compatibility no-op rather than a working mid-search
  interrupt: a GUI can send literal `stop` between searches and get
  `console.c`'s own "Engine is not currently thinking" reply, but it cannot
  reach a synchronous search already in progress the way Ctrl-C can, because
  `readline_interactive()` (where a typed line would be read) isn't running
  while `search_position()` owns the call stack — only the raw
  `console_interrupt_requested()` byte-level poll inside
  `search_poll_stop_callback()` runs concurrently with the search, in the
  sense that both share the same synchronous execution, one nested inside
  the other every 2048 nodes.
- **`lisp_eval()`** (`user/lisp/lisp.c:1793-1821`) is the other named use
  case ("run-away Lisp scripts") and the reason this belongs at the kernel
  layer instead of living in `user/chess/`: it's called recursively for
  every sub-expression already, right next to the existing
  `eval_depth`/`LISP_MAX_EVAL_DEPTH` guard (:1809-1817) that already
  protects against unbounded *recursion*, but not against a legitimately
  bounded-depth call that's simply expensive (an exponential-blowup
  recursive definition well under the depth-100 ceiling can still run for a
  long time). Add a static call counter alongside `eval_depth`, check
  `console_interrupt_requested()` every ~1024 calls (the same
  cost-vs-responsiveness throttle `search.c` already applies at a 2048-node
  cadence, for the same reason: `uart_has_char()` isn't free enough to call
  on literally every recursive step, but is cheap enough every thousand or
  so), and on a hit, print `"[Lisp] interrupted by Ctrl-C\n"` and unwind to
  `&nil_val` — the exact same shape the node-pool-exhaustion and
  depth-exceeded checks immediately above it in that function already use,
  not a new error-handling pattern.
- Any future long-running foreground command gets this for the cost of one
  polled call, by the same convention — `console_interrupt_requested()`
  isn't chess- or Lisp-specific, it's general kernel infrastructure this
  phase happens to be what motivates writing.

**Why this doesn't need a process-model change, correcting the earlier
out-of-scope note:** that note conflated "no preemptive interrupt of a
synchronous call stack" (true, and irrelevant here) with "no way to poll for
a stop request" (false — `search.c` already calls a poll hook on a fixed
node cadence regardless of search depth or position, and `lisp_eval()` can
cheaply grow the same habit). 2048 nodes is a few milliseconds even at
RP2350's measured ~44K nodes/s hardware number (H4's own figure), so the
response latency of "press Ctrl-C or STOP, search ends" is well within what
a human perceives as immediate, with zero scheduler or task-model
involvement — the search still runs synchronously to completion of its
current `check_up_time()` interval, it just checks a flag on the way back
out of `pv_search`/`quiescence`'s recursion
(`search.c:392,420,437,492,542`, all `if (stop_search) return 0;`).

**Verify:** QEMU — a scripted test starts `go` (or `level 8` then a move)
against a slow-to-resolve position and sends a raw `0x03` byte on a short
delay, asserting the command returns with a `bestmove` rather than hanging
for the test's timeout; a second test drives a deliberately expensive (but
depth-bounded) recursive Lisp definition through `lisp_repl()` and confirms
the same `0x03` unwinds it with the interrupted-message rather than running
to completion. Hardware — the TM1638 STOP-key path, same category as J3's
other interactive checks (no QEMU model for the keypad).

## J3 — TM1638 function keys 8-15

Closes [[phase9_chess_followups]] item 2. Wires the four-mode state machine
`console.c` already designs (`BoardMode` enum, :87-96) around J1/J2's shared
logic instead of `console.c`'s own globals:

- **Board view** (key 10 to enter, 8/9 to scroll ranks, 11 to exit): ports
  `show_board_rank()` (:197-225) directly — pure formatting over
  `pos->board[]`, no hardware-specific state.
- **Level select** (key 12 to enter, 0-7 to pick, 11 cancel / 15 confirm):
  ports the `time_names[8]` display table and `search_level` assignment at
  :769-785 — `chess_ui.c` doesn't have a `search_level` concept at all today
  (`chess_run()` hard-codes 5000 ms, `chess_ui.c:340`); this milestone adds
  it, reusing `console.c`'s same `level_times_ms[8]` table from J1.
  `2s`/`5s`/`10s`/... a per-move budget, not depth — no depth-based level
  needed.
- **Options menu** (key 14 to enter, 8/9 to cycle, 11 cancel / 15 confirm):
  of `console.c`'s 10 options (:106-117), five port directly with no new
  dependency (`new`, `score`, `side`, `halfmove`, `movecount` — :805-859, all
  either a state transition already in scope or a one-line status readout);
  `save`/`load` (idx 8/9, :860-903) redirect to J1's `vfs_write()`/
  `vfs_read()` mechanism instead of the original's flash-sector code, so the
  TM1638 and console front ends share one save format and one save file.
  "Play Black"/"Play White" (idx 1/2) are folded into a single "swap sides"
  toggle rather than ported as two separate menu entries — `console.c`'s
  own idx-2 handler is just `go` (:815-820), i.e. "make the engine move
  now," which J1's `go` command already covers without a dedicated menu slot.
- Search-time keys already work and don't need new wiring: `search_poll_stop_
  callback()`'s key-11-stops / key-10-toggles-board-view behavior
  (`console.c:293-352`) is a mid-search poll, not a `MODE_NORMAL` dispatch —
  `chess_ui.c`'s existing empty `search_poll_stop_callback()`
  (`chess_ui.c:77-83`) is exactly where this plugs in; J0's heap-on-demand
  change doesn't touch this function.

`tm_read_move()`'s existing keys-8/9 handling (undo/redo shortcuts,
`console.c:699-711`) already has a natural home now that J1/J2 give `chess_
ui.c` real undo/redo and outcome state to call into — currently `chess_ui.c`
has no undo/redo at all, matching J1's gap on the console side.

**Verify:** hardware-only, per [[falsify_on_hardware_not_qemu]] — no QEMU
model for TM1638. `tests/hw/test_rp2350.py` gets a new interactive checklist
entry (menu navigation, level change, save/load round-trip) alongside the
existing move-entry/board-render checks from H1/H2.

## J4 — `perft` command + bounded perft in the automated test suite

Closes [[phase9_chess_followups]] item 3. `perft.c`/`.h`
(`~/gith/domschl/LugalChess/engine/src/perft.c`, 131 lines) get vendored
into `user/chess/{src,include}/` alongside the rest of the engine core. Two
small platform edits, both isolated to `run_perft_tests_depth()`
(`run_perft()` itself is pure `generate_moves`/`make_move`/`unmake_move`
recursion, no stdio, no timing — ports unchanged):

1. `printf` -> `cprintf` (covered by `chess_platform.h`'s existing `#define`
   for every call site here — no `%f`/`%ld` mismatches in this file, checked
   against the actual source above).
2. `clock()`/`CLOCKS_PER_SEC`/`<time.h>` (host-only, not available in
   LugalOS's freestanding build) -> `kernel/time.h`'s existing millisecond
   timer, the same one `chess_ui.c` already uses for `chess_selftest()`'s own
   timing.

`perft [n]` becomes a Lisp primitive / shell command (J0) calling
`run_perft_tests_depth(n)` (default depth 5, matching upstream's own
default when `n` is omitted).

**Test suite integration, bounded for QEMU/CI runtime as the user's own note
anticipates** ("perft is time-consuming, so making non-trivial levels part
of the automatic test-suite would slow down the hw-testing significantly"):
add one `tests/runner.py` case running `perft 3` (or the shallowest depth
that still exercises castling/en-passant/promotion — `perft.c`'s own
`pej-*` positions at :25-46 are specifically chosen for that, several are
depth-1-sufficient already) against the `"start pos"` and 2-3 of the
`pej-*` tactical positions, asserting `errors == 0` in the printed summary.
Depth 3 on the full 24-case table is sub-second even at RP2350's measured
~44K nodes/s (H4's own hardware number) — this is a correctness check on
move generation, not a performance benchmark, so it doesn't need to run on
hardware at all; QEMU-only is the right (and per [[falsify_on_hardware_not_qemu]],
sufficient) place for it, since perft correctness doesn't depend on any
board-specific code path.

## J5 — UCI-over-UART bridge (stretch; standard UCI only)

The user's own simplification — drop `console.c`'s hacked-in bidirectional
control (keypad-to-GUI move sync) and expose only what `uci.c`
(`~/gith/domschl/LugalChess/engine/src/uci.c`, 174 lines) already
implements: a clean `uci`/`isready`/`ucinewgame`/`position`/`go`/`quit` loop
with no menu system, no TM1638/ST7735 coupling. `uci.c` is small enough to
port close to verbatim (`chess_uci_run()` in `chess_ui.c`), reusing J1's
`vfs`-free save/load-less state (a UCI session doesn't persist across
`ucinewgame`) and the same `execute_player_move()`/promotion logic J1/J2
already share.

**Binding decision this milestone should make explicitly, following
`p9serve`'s own precedent rather than `console-bind`'s:** `console_bind()`
(`kernel/console.c:12`) rebinds the *system console* — the shell's own
stdio — to a device; that's the wrong shape here, since a UCI session and an
interactive shell are different consumers that might both want to exist
(one on USB, one on a UART, say). `p9serve` instead takes ownership of a
named device directly and blocks servicing it (`kernel/shell.c`, phase5 B-series) without touching the system console at all — `chess-uci <device-name>`
should follow that same shape: open the named UART device directly (same
device-registry lookup `console_bind_device()` already uses internally,
`kernel/console.c:64-80`), and loop reading/writing raw bytes against it
until `quit`, never calling `console_bind()`. This is what lets `chess-uci`
run on a second UART while `lsh` keeps the primary console, rather than
forcing a choice between "shell" and "chess GUI" on one board.

**Explicitly out of scope, and belongs to a different repository:** teaching
`~/gith/domschl/LugalChess`'s own `gui/lugalgui` PySide6 app to speak plain
UCI instead of its current bidirectional protocol. This phase only needs
LugalOS to emit correct, standard UCI on a port — that already makes it
usable from any standard UCI-speaking GUI that exists today (cutechess-cli,
Arena, a lichess bridge), which is the actual value of the simplification
the user proposed: no GUI-side changes required to get *some* GUI working,
even before LugalGUI itself is ever touched.

**Verify:** QEMU-testable in principle (no hardware-specific code — the UART
device abstraction already has a QEMU-side virtio/16550 backing per earlier
phases), via `tests/runner.py` scripting a handful of UCI commands over the
device and asserting `bestmove` output. Real GUI interop (cutechess-cli
against a live board) is hardware-only, same category as J3.

## J6 — `init.lisp` single-purpose persona

Small, and last because it only makes sense once J1-J3 exist: document (and
optionally provide as a commented-out example in `tools/sd_root/system/etc/
init.lisp`, following that file's existing style of commented alternatives
rather than hard-coding a board-specific choice) replacing `init.lisp`'s
final `(lsh)` call with `(chess)` for a board persona that boots straight
into the chess computer, matching old LugalChess's dedicated-hardware
behavior. Not a new mechanism — `(chess)` is J0's own primitive, already
callable from Lisp; this milestone is purely about `init.lisp` documentation/
example, not new C code.

---

## Deliberately out of scope for this phase

- **LugalGUI-side protocol changes** (see J5) — different repository, not
  blocked on anything here.
- **Preemptive/async search interruption** — not needed and not attempted;
  J2 now covers the actual requirement (stopping an unbounded Level-8/`go`
  search via a terminal Ctrl-C or the TM1638 STOP key) with the polling hook
  `search.c` already calls every 2048 nodes, backed by a general
  `console_interrupt_requested()` primitive (J2) that also reaches
  `lisp_eval()` for the "run-away script" case. What's still genuinely out
  of scope is something reaching into the search from *outside* that
  cadence — e.g. a second shell session on another console signaling a
  search running in a different task — which would need the search running
  as a schedulable, signalable task rather than a synchronous call on the
  invoking context's own stack. Nothing in this phase's usage pattern (one
  console or one keypad, one search, one place to check `stop_search`) needs
  that.
- **Flash-based persistent save** — deliberately replaced by VFS-based save
  (J1/J3) rather than ported, since LugalOS has its own filesystem for
  exactly this (H0's own note from phase9).
