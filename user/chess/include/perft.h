/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/perft.h),
 * J4, plan/phase10_chess_completion.md. No platform dependency at all -- see
 * perft.c for what actually needed adapting.
 */

#ifndef PERFT_H
#define PERFT_H

#include "defs.h"
#include "position.h"

/* Lazily allocates a heap-backed per-ply MoveList pool on first call
 * (perft.c); the pool is freed only by run_perft_tests_depth() at the end
 * of its own loop. Calling run_perft() directly, outside that path, leaves
 * the pool allocated with nothing to free it -- fine for one-off use, but
 * the caller owns that consequence. */
uint64_t run_perft(Position *pos, int depth);
int run_perft_tests(void);
int run_perft_tests_depth(int max_depth);

#endif // PERFT_H
