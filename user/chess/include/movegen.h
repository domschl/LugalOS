/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/movegen.h),
 * H4, plan/phase9_chess_computer.md.
 */

#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "defs.h"
#include "position.h"

typedef struct {
    Move moves[MAX_MOVES];
    int count;
} MoveList;

void generate_moves(const Position *pos, MoveList *list);
void generate_captures(const Position *pos, MoveList *list);

// Helper to push a move
static inline void add_move(MoveList *list, Move move) {
    list->moves[list->count++] = move;
}

#endif // MOVEGEN_H
