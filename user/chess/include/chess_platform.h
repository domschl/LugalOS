/*
 * LugalOS chess engine platform shim (H4, plan/phase9_chess_computer.md).
 *
 * The vendored engine (~/gith/domschl/LugalChess, under engine/src) is hosted C11
 * written against a real libc (printf-family, atoi, rand/srand, malloc). This
 * header is what "defs.h" includes instead of <stdio.h>/<stdlib.h> on
 * LugalOS's freestanding build, which has neither.
 *
 * `printf` -> `cprintf` (kernel/console.h) covers the overwhelming majority
 * of call sites unmodified: cprintf's %d/%i already reads its argument as a
 * `long` regardless of an 'l' length modifier (kernel/printk.c), so %ld call
 * sites need no cast. The two real incompatibilities -- %.0f (no float
 * support at all) and %016llx (a single 'l' is skipped, not two, so it would
 * mis-consume a 64-bit argument as 32-bit and desync every argument after
 * it) -- are hand-edited at their call sites in tt.c/search.c/position.c/
 * bitboard.c instead of patched here.
 */

#ifndef LUGALOS_CHESS_PLATFORM_H
#define LUGALOS_CHESS_PLATFORM_H

#include "kernel/printk.h"
#include "kernel/console.h"
#include <string.h>

#define printf cprintf
#define fflush(x) ((void)0)

#define abs(x) ((x) < 0 ? -(x) : (x))

/* Only used by position.c's FEN parser (halfmove/fullmove clocks) --
 * decimal digits only, no sign/whitespace handling, matching what a FEN
 * string actually contains at that point. */
int atoi(const char *s);

/* Seeds a small xorshift PRNG. Only `srand` is ever called by the vendored
 * engine (search.c, opening-book move selection) -- `rand()` itself is
 * provided for completeness/forward-safety but nothing currently calls it. */
void srand(unsigned int seed);
long rand(void);

#endif /* LUGALOS_CHESS_PLATFORM_H */
