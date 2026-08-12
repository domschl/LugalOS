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

/* The plain-terminal console REPL (J1, plan/phase10_chess_completion.md) --
 * scenario 1.1, no hardware dependency. Builds and runs on every target.
 * Returns on 'quit', same as chess_run() below on Ctrl-C/STOP. */
void chess_console_run(void);

/* The interactive game: TM1638 for move entry, ST7735 for the board.
 * RP2350 hardware only (CONFIG_ENABLE_DISPLAY && CONFIG_ENABLE_TM1638).
 * Returns to the shell on Ctrl-C or the TM1638 STOP key (J2) -- prior to
 * that this had no software exit at all, only a physical board reset. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DISPLAY && CONFIG_ENABLE_TM1638
void chess_run(void);
#endif

#endif /* LUGALOS_CHESS_UI_H */
