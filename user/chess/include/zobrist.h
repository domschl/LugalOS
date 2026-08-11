/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/zobrist.h),
 * H4, plan/phase9_chess_computer.md.
 */

#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "defs.h"

// Zobrist keys declaration
extern uint64_t zobrist_pieces[6][2][64]; // [piece_type][color][square]
extern uint64_t zobrist_castling[16];
extern uint64_t zobrist_en_passant[64];
extern uint64_t zobrist_side;

void init_zobrist(void);

#endif // ZOBRIST_H
