/*
 * Public entry points for the LugalOS chess UI (H4,
 * plan/phase9_chess_computer.md). See chess_ui.c for the rationale.
 */

#ifndef LUGALOS_CHESS_UI_H
#define LUGALOS_CHESS_UI_H

#include "lugalos_config.h"
#include "position.h"

/* Engine-only smoke test: searches from the start position and reports the
 * chosen move via cprintf. No hardware dependency -- builds and runs on
 * every target, including QEMU. */
void chess_selftest(void);

/* J4 (plan/phase10_chess_completion.md): move-generation correctness
 * suite (perft.c's own test-case table). `max_depth <= 0` uses
 * run_perft_tests_depth()'s own documented default (5). No hardware
 * dependency, same as chess_selftest() above. */
void chess_perft(int max_depth);

/* As above, with each position's perft split across `cores` (X8,
 * plan/phase23_multicore_scheduling.md). One core is the pre-X8 behaviour. */
void chess_perft_cores(int max_depth, int cores);

/* The plain-terminal console REPL (J1, plan/phase10_chess_completion.md) --
 * scenario 1.1, no hardware dependency. Builds and runs on every target.
 * Returns on 'quit', same as chess_run() below on Ctrl-C/STOP. */
void chess_console_run(void);

/* The session's shared scratch Position (phase 15 §1.3). Non-NULL only
 * between chess_ensure_init() and chess_session_end(). Declared here now that
 * pgn.c wants it too -- search.c still reaches it with its own `extern` at the
 * call site, matching how it reaches the other two engine/UI seam functions. */
Position *chess_scratch_position(void);

/* The interactive game: TM1638 for move entry, ST7735 for the board.
 * RP2350 hardware only (CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638).
 * Returns to the shell on Ctrl-C or the TM1638 STOP key (J2) -- prior to
 * that this had no software exit at all, only a physical board reset. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735 && CONFIG_ENABLE_TM1638
void chess_run(void);
#endif

#endif /* LUGALOS_CHESS_UI_H */
