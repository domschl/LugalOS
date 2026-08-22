/*
 * Vendored from ~/gith/domschl/LugalChess (engine/include/movegen.h), H4,
 * plan/phase9_chess_computer.md. Changes from upstream (§2.1,
 * plan/phase15_memory_reclamation.md):
 *   - CaptureList, a narrower list type for generate_captures(), which cannot
 *     produce anything near MAX_MOVES entries; quiescence keeps one per ply,
 *     so the width was being multiplied by MAX_SEARCH_PLYS for nothing. See
 *     MAX_CAPTURES in defs.h for the measurement behind its width.
 *   - generate_captures() takes a CaptureList * accordingly.
 *   - add_move()/add_capture() bounds-check. Upstream's add_move() did not,
 *     which was survivable only because MAX_MOVES sits above the 218-move
 *     maximum for a legal position; with a list deliberately narrower than
 *     its worst case, an unchecked push stops being a theoretical concern
 *     and becomes the failure mode to design for.
 */

#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "defs.h"
#include "position.h"

typedef struct {
    Move moves[MAX_MOVES];
    int count;
} MoveList;

/* Quiescence's per-ply list. Same shape, narrower: see MAX_CAPTURES. */
typedef struct {
    Move moves[MAX_CAPTURES];
    int count;
} CaptureList;

void generate_moves(const Position *pos, MoveList *list);
void generate_captures(const Position *pos, CaptureList *list);

/* Helpers to push a move.
 *
 * Both silently drop the move when full rather than writing past the end.
 * Dropping is the right failure here: these lists live inside a contiguous
 * per-ply pool, so an overrun would land in the *next* ply's list -- silent
 * corruption of a live neighbour, at a search depth that is hard to
 * reproduce. A dropped move costs one branch of one node instead, and only
 * in a position that exceeds a bound no real game approaches. */
static inline void add_move(MoveList *list, Move move) {
    if (list->count < MAX_MOVES) list->moves[list->count++] = move;
}

static inline void add_capture(CaptureList *list, Move move) {
    if (list->count < MAX_CAPTURES) list->moves[list->count++] = move;
}

#endif // MOVEGEN_H
