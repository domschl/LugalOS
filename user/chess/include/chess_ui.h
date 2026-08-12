/*
 * Public entry points for the LugalOS chess UI (H4,
 * plan/phase9_chess_computer.md). See chess_ui.c for the rationale.
 */

#ifndef LUGALOS_CHESS_UI_H
#define LUGALOS_CHESS_UI_H

#include "lugalos_config.h"

/* Engine-only smoke test: searches from the start position and reports the
 * chosen move via cprintf. No hardware dependency -- builds and runs on
 * every target, including QEMU. */
void chess_selftest(void);

/* J4 (plan/phase10_chess_completion.md): move-generation correctness
 * suite (perft.c's own test-case table). `max_depth <= 0` uses
 * run_perft_tests_depth()'s own documented default (5). No hardware
 * dependency, same as chess_selftest() above. */
void chess_perft(int max_depth);

/* The plain-terminal console REPL (J1, plan/phase10_chess_completion.md) --
 * scenario 1.1, no hardware dependency. Builds and runs on every target.
 * Returns on 'quit', same as chess_run() below on Ctrl-C/STOP. */
void chess_console_run(void);

/* J5 (plan/phase10_chess_completion.md): the standard UCI protocol loop, no
 * TM1638/ST7735 coupling. Builds and runs on every target -- RP2350 talks
 * over its dedicated ACM2/EP6 CDC interface (drivers/usb_cdc.c), QEMU
 * borrows the single virtio-console link from background 9P for the
 * session's duration. Returns only on 'quit', deliberately not on Ctrl-C --
 * unlike chess_console_run() above, this front end runs on a wire separate
 * from the operator's own console, and checking for Ctrl-C there would mean
 * silently discarding whatever the operator is typing on that unrelated
 * console for as long as a session is open (chess_ui.c's uci_read_line()
 * has the full story). Same "no software escape" shape as p9serve. */
void chess_uci_run(void);

/* The interactive game: TM1638 for move entry, ST7735 for the board.
 * RP2350 hardware only (CONFIG_ENABLE_DISPLAY && CONFIG_ENABLE_TM1638).
 * Returns to the shell on Ctrl-C or the TM1638 STOP key (J2) -- prior to
 * that this had no software exit at all, only a physical board reset. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DISPLAY && CONFIG_ENABLE_TM1638
void chess_run(void);
#endif

#endif /* LUGALOS_CHESS_UI_H */
