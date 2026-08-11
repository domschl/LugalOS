/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/move.h),
 * H4, plan/phase9_chess_computer.md. No platform dependency at all.
 */

#ifndef MOVE_H
#define MOVE_H

#include "defs.h"

/*
 * Move Encoding (16-bit):
 * bits 0-5   : Source square (0-63)
 * bits 6-11  : Destination square (0-63)
 * bits 12-15 : Flag (4 bits)
 *
 * Flags:
 * 0000 (0)  : Quiet move
 * 0001 (1)  : Double pawn push
 * 0010 (2)  : Kingside castle
 * 0011 (3)  : Queenside castle
 * 0100 (4)  : Regular capture
 * 0101 (5)  : En-passant capture
 * 1000 (8)  : Knight promotion
 * 1001 (9)  : Bishop promotion
 * 1010 (10) : Rook promotion
 * 1011 (11) : Queen promotion
 * 1100 (12) : Knight promotion-capture
 * 1101 (13) : Bishop promotion-capture
 * 1110 (14) : Rook promotion-capture
 * 1111 (15) : Queen promotion-capture
 */

#define MOVE_FROM(move)       ((move) & 0x3F)
#define MOVE_TO(move)         (((move) >> 6) & 0x3F)
#define MOVE_FLAGS(move)      (((move) >> 12) & 0xF)

#define MOVE_FLAG_DOUBLE_PUSH 1
#define MOVE_FLAG_K_CASTLE   2
#define MOVE_FLAG_Q_CASTLE   3
#define MOVE_FLAG_CAPTURE    4
#define MOVE_FLAG_EN_PASSANT 5

#define MOVE_FLAG_PROMO_N    8
#define MOVE_FLAG_PROMO_B    9
#define MOVE_FLAG_PROMO_R    10
#define MOVE_FLAG_PROMO_Q    11

#define MOVE_FLAG_PROMO_CAP_N 12
#define MOVE_FLAG_PROMO_CAP_B 13
#define MOVE_FLAG_PROMO_CAP_R 14
#define MOVE_FLAG_PROMO_CAP_Q 15

// Checkers
static inline bool move_is_capture(Move move) {
    return (MOVE_FLAGS(move) & 4) != 0;
}

static inline bool move_is_promo(Move move) {
    return (MOVE_FLAGS(move) & 8) != 0;
}

static inline int move_promo_piece(Move move) {
    // Mapping: flag & 3: 0->KNIGHT(1), 1->BISHOP(2), 2->ROOK(3), 3->QUEEN(4)
    return (MOVE_FLAGS(move) & 3) + KNIGHT;
}

// Builder
static inline Move build_move(int from, int to, int flag) {
    return (Move)(from | (to << 6) | (flag << 12));
}

#endif // MOVE_H
