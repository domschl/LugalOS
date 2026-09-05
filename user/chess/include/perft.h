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

/* Same answer as run_perft(), computed across up to `workers` cores by
 * splitting the root move list (X8, plan/phase23_multicore_scheduling.md).
 * The identity with run_perft() is the point: the published node counts in
 * perft.c's own table verify the parallel path exactly, with no argument
 * about whether a concurrency change was correct.
 *
 * Clamps to the harts actually online and falls back to one worker whenever
 * a second cannot be had. `*used` (optional) reports how many really ran, so
 * a caller never has to infer that from timing. */
uint64_t run_perft_cores(Position *pos, int depth, int workers, int *used);
int run_perft_tests(void);
int run_perft_tests_depth(int max_depth);

/* As above, with the per-position perft split across `workers` cores. */
int run_perft_tests_cores(int max_depth, int workers);

#endif // PERFT_H
