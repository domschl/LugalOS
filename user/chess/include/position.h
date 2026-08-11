/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/position.h),
 * H4, plan/phase9_chess_computer.md.
 */

#ifndef POSITION_H
#define POSITION_H

#include "defs.h"
#include "move.h"

typedef struct {
    Move move;
    int en_passant;
    int castling_rights;
    int halfmove;
    int fullmove;
    int captured_piece;
    uint64_t hash_key;
} UndoState;

typedef struct {
    uint64_t piece_bbs[6];   // PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING
    uint64_t color_bbs[2];   // WHITE, BLACK

    int side;                // WHITE or BLACK
    int en_passant;          // NO_SQ or square index
    int castling_rights;     // WK_CASTLE | WQ_CASTLE | BK_CASTLE | BQ_CASTLE
    int halfmove;            // 50-move rule
    int fullmove;            // Fullmove number

    uint64_t hash_key;       // Zobrist hash key

    uint8_t board[64];       // O(1) piece lookup (piece type PAWN..KING or NO_PIECE)

    UndoState history[MAX_PLYS];
    int history_ply;
} Position;

// Parse/Display
void parse_fen(Position *pos, const char *fen);
bool is_position_valid(const Position *pos);
void generate_fen(const Position *pos, char *buf);
void print_board(const Position *pos);
void print_position_info(const Position *pos);

// Execution
bool make_move(Position *pos, Move move);
void unmake_move(Position *pos);
bool make_null_move(Position *pos);
void unmake_null_move(Position *pos);

// Hashing
uint64_t generate_hash_key(const Position *pos);

// Attack detection
bool is_square_attacked(const Position *pos, int square, int attacker_side);

// Helper helper
static inline uint64_t get_occupied(const Position *pos) {
    return pos->color_bbs[WHITE] | pos->color_bbs[BLACK];
}

#endif // POSITION_H
