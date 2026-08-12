/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/perft.h),
 * J4, plan/phase10_chess_completion.md. No platform dependency at all -- see
 * perft.c for what actually needed adapting.
 */

#ifndef PERFT_H
#define PERFT_H

#include "defs.h"
#include "position.h"

uint64_t run_perft(Position *pos, int depth);
int run_perft_tests(void);
int run_perft_tests_depth(int max_depth);

#endif // PERFT_H
