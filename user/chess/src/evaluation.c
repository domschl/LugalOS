/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/src/evaluation.c),
 * H4, plan/phase9_chess_computer.md. All-integer arithmetic throughout; the
 * only libc dependency is abs() (2 call sites), which chess_platform.h
 * covers with a macro.
 */

#include "evaluation.h"
#include "bitboard.h"

// PeSTO's Piece-Square Tables (PSQT)
// Reference: https://www.chessprogramming.org/PeSTO%27s_Evaluation_Function
// All values are indexed from White's perspective (A1=0..H8=63).
// Black squares are mirrored vertically using sq ^ 56.

// Middlegame values for pieces
static const int mg_value[6] = { 82, 337, 365, 477, 1025, 0 };
// Endgame values for pieces
static const int eg_value[6] = { 94, 281, 297, 512,  936, 0 };

// Middlegame PSQTs
static const int mg_psqt[6][64] = {
    // Pawn
    {
          0,   0,   0,   0,   0,   0,   0,   0,
         98, 134,  61,  95,  68, 126,  34, -11,
         -6,   7,  26,  14, -10,   9,  31,  -9,
        -14,   4,  -3,  -6,  -4, -16, -15, -17,
        -27,  -2,  -5, -11, -17, -24, -20, -32,
        -26,  -4,  -4, -10,  -3,  -7,   2, -30,
        -35,  -1, -19, -19, -24, -26, -20, -38,
          0,   0,   0,   0,   0,   0,   0,   0
    },
    // Knight
    {
        -167, -89, -34, -49,  61, -97, -15, -107,
         -73, -41,  72,  36,  23,  62,   7,  -17,
         -47,  60,  37,  65,  84,  77,   6,   -2,
          -9,  17,  19,  53,  37,  69,  18,   22,
         -13,   4,  16,  13,  28,  19,  21,   -8,
         -23,  -9,  12,  10,  19,  17,  25,  -16,
         -62, -41,  -5,   3,  -3,  -8,  -3,  -44,
        -149, -98, -18, -38, -46, -33, -95, -134
    },
    // Bishop
    {
         -29,   4, -18, -10,  -6,   2,  -3,  -17,
         -12, -18, -11,  -4,   0,  10, -10,   -3,
          -8,  15,  11,  15,  27,  15,  10,   -9,
           7,   6,   2,  10,  15,  24,  17,    7,
           7,   0, -10,   1,   1, -14, -12,   -1,
          -4, -14,  12,  -7,  -7,  -4,  -3,  -14,
         -17, -11,   8,  -2,  -3,  -1,  -5,  -15,
         -25,  -4,  -9, -10,  -3, -12, -18,  -22
    },
    // Rook
    {
          32,  42,  32,  51,  63,   9,  31,  43,
          27,  32,  58,  62,  80,  67,  26,  44,
          -5,  19,  26,  36,  17,  45,  61,  16,
         -24, -11,   7,  26,  24,  35,  -8, -20,
         -36, -26, -12,  -1,  -9,  -7,   6, -23,
         -45, -25, -16, -17,   3,   0,  -5, -38,
         -44, -16, -20,  -9,  -1,  11,  -6, -71,
         -19, -36,   1, -25,   7, -17, -49, -51
    },
    // Queen
    {
         -28,   0,  29,  12,  59,  44,  43,  45,
         -24, -39,  -5,   1, -16,  57,  28,  54,
         -17,  15,  -1,  12,  -8,  50,  29,  41,
         -26,  -8, -10,   0,  10,   8, -16, -15,
         -18, -16, -10, -13,  -3, -16, -21, -16,
         -25,  -5,  -6, -10,  -7,  -6,   1, -10,
         -24, -11,   0, -26, -15, -10, -20, -36,
         -20, -18, -14, -28, -22, -16,  -9, -14
    },
    // King
    {
         -60, -30, -10, -50, -50, -10, -30, -60,
         -40,  -5, -10, -40, -40, -10,  -5, -40,
         -10,  15,  10, -20, -20,  10,  15, -10,
          -5,  10,  10, -30, -30,  10,  10,  -5,
         -15,  -5, -10, -30, -30, -10,  -5, -15,
         -30, -10, -20, -40, -40, -20, -10, -30,
         -60, -30, -30, -60, -60, -30, -30, -60,
         -99, -99, -99, -99, -99, -99, -99, -99
    }
};

// Endgame PSQTs
static const int eg_psqt[6][64] = {
    // Pawn
    {
          0,   0,   0,   0,   0,   0,   0,   0,
        178, 173, 158, 134, 147, 180, 165, 154,
         94, 100,  85,  67,  56,  53,  82,  84,
         32,  24,  13,   5,  -2,   4,  17,  17,
         13,   9,  -3,  -7,  -7,  -8,   3,  -1,
          4,   7,  -6,  -6,  -8, -11,  -6,  -9,
         -5,  -9, -15, -23, -23, -22, -13, -13,
          0,   0,   0,   0,   0,   0,   0,   0
    },
    // Knight
    {
         -58, -38, -13, -28, -31, -27, -44, -99,
         -25,  -8, -25,  -2,  -9, -25, -24, -52,
         -24, -20,  10,   9,  -1,  -9, -19, -41,
         -17,   3,  22,  22,  22,  11,   8, -18,
         -18,  -6,  16,  25,  16,  17,   4, -18,
         -23,  -3,  -1,  15,  10,  21,   2, -20,
         -42, -20, -10,  -5,  -2, -20, -23, -44,
         -29, -51, -23, -15, -22, -18, -50, -51
    },
    // Bishop
    {
         -14, -21, -11,  -8,  -7,  -9, -17, -24,
          -8,  -4,   7, -12,  -3, -13,  -4, -16,
          -3,   9,  12,   9,  14,  10,   3,  -2,
          -4,   7,  19,  17,  19,   7,  14,  -5,
          -3,   0,   8,  11,  11,   5,   7,  -5,
          -1,  12,  10,  11,   3,   6,  10,  -7,
         -14,  -4, -14,  -4,  -6,  -2,  -9, -17,
         -23, -17, -14,  -3, -12, -21, -17, -30
    },
    // Rook
    {
          13,  10,  18,  15,  12,  12,   8,   5,
          11,  13,  13,  14,  -3,   3,   8,   2,
           7,   7,   7,   5,   4,  -3,  -5,  -7,
           1,   2,   2,   2,   1,  -3,  -6,  -5,
          -1,   0,   1,   0,  -3,  -1,  -7,  -7,
          -5,   0,   2,   5,  -9,  -9,  -5,  -9,
          -6,  -3,  -3,  -7,  -7,  -5,  -6,  -7,
          -9,  -9,   3,  -9,  -5,  -6,  -5, -20
    },
    // Queen
    {
         -52, -28, -22, -16, -18, -28, -41, -52,
         -29, -20, -13, -11,  -4,  -7, -10, -32,
         -27, -15,   4,   3,   3,   4,  -6, -26,
         -17,  -5,   9,   8,   9,   9,   4, -17,
         -18,  -2,   3,   5,   5,   4,  -2, -18,
         -20,  -5,   0,   7,   6,   5,   1, -21,
         -27, -10, -17,  -3,  -1,  -7, -11, -24,
         -45, -34, -20, -22, -22, -28, -32, -43
    },
    // King
    {
         -50, -38, -30, -26, -26, -30, -38, -50,
         -38, -13, -14,   4,   4, -14, -13, -38,
         -30, -14,  16,  25,  25,  16, -14, -30,
         -26,   4,  25,  38,  38,  25,   4, -26,
         -26,   4,  25,  38,  38,  25,   4, -26,
         -30, -14,  16,  25,  25,  16, -14, -30,
         -38, -13, -14,   4,   4, -14, -13, -38,
         -50, -38, -30, -26, -26, -30, -38, -50
    }
};

// Endgame Mop-up evaluation helper
// When one side has winning material advantage and opponent has no pawns/few pieces,
// reward driving opponent king to edge/corner and bringing own king closer.
static int evaluate_mop_up(const Position *pos, int winning_side) {
    int losing_side = winning_side ^ 1;

    uint64_t w_k_bb = pos->piece_bbs[KING] & pos->color_bbs[winning_side];
    uint64_t l_k_bb = pos->piece_bbs[KING] & pos->color_bbs[losing_side];

    if (!w_k_bb || !l_k_bb) return 0;

    int w_king_sq = get_lsb(w_k_bb);
    int l_king_sq = get_lsb(l_k_bb);

    if (w_king_sq < 0 || w_king_sq >= 64 || l_king_sq < 0 || l_king_sq >= 64) return 0;

    int l_king_rank = l_king_sq / 8;
    int l_king_file = l_king_sq % 8;
    int w_king_rank = w_king_sq / 8;
    int w_king_file = w_king_sq % 8;

    int file_dist_from_center = l_king_file <= 3 ? (3 - l_king_file) : (l_king_file - 4);
    int rank_dist_from_center = l_king_rank <= 3 ? (3 - l_king_rank) : (l_king_rank - 4);
    int center_dist = file_dist_from_center + rank_dist_from_center;

    int rank_diff = abs(w_king_rank - l_king_rank);
    int file_diff = abs(w_king_file - l_king_file);
    int king_dist = rank_diff > file_diff ? rank_diff : file_diff;

    return (center_dist * 10) + ((7 - king_dist) * 20);
}

// Evaluate the position from White's perspective
int evaluate(const Position *pos) {
    int mg_score = 0;
    int eg_score = 0;
    int phase = 0;

    // Piece types
    uint64_t w_pawns = pos->piece_bbs[PAWN] & pos->color_bbs[WHITE];
    uint64_t b_pawns = pos->piece_bbs[PAWN] & pos->color_bbs[BLACK];
    uint64_t w_knights = pos->piece_bbs[KNIGHT] & pos->color_bbs[WHITE];
    uint64_t b_knights = pos->piece_bbs[KNIGHT] & pos->color_bbs[BLACK];
    uint64_t w_bishops = pos->piece_bbs[BISHOP] & pos->color_bbs[WHITE];
    uint64_t b_bishops = pos->piece_bbs[BISHOP] & pos->color_bbs[BLACK];
    uint64_t w_rooks = pos->piece_bbs[ROOK] & pos->color_bbs[WHITE];
    uint64_t b_rooks = pos->piece_bbs[ROOK] & pos->color_bbs[BLACK];
    uint64_t w_queens = pos->piece_bbs[QUEEN] & pos->color_bbs[WHITE];
    uint64_t b_queens = pos->piece_bbs[QUEEN] & pos->color_bbs[BLACK];
    uint64_t w_king = pos->piece_bbs[KING] & pos->color_bbs[WHITE];
    uint64_t b_king = pos->piece_bbs[KING] & pos->color_bbs[BLACK];

    // Compute game phase
    phase += count_bits(w_knights | b_knights) * 1;
    phase += count_bits(w_bishops | b_bishops) * 1;
    phase += count_bits(w_rooks | b_rooks) * 2;
    phase += count_bits(w_queens | b_queens) * 4;
    if (phase > 24) phase = 24;

    // Evaluate White pieces
    uint64_t bb;
    bb = w_pawns; while (bb) { int sq = pop_lsb(&bb); mg_score += mg_value[PAWN] + mg_psqt[PAWN][sq]; eg_score += eg_value[PAWN] + eg_psqt[PAWN][sq]; }
    bb = w_knights; while (bb) { int sq = pop_lsb(&bb); mg_score += mg_value[KNIGHT] + mg_psqt[KNIGHT][sq]; eg_score += eg_value[KNIGHT] + eg_psqt[KNIGHT][sq]; }
    bb = w_bishops; while (bb) { int sq = pop_lsb(&bb); mg_score += mg_value[BISHOP] + mg_psqt[BISHOP][sq]; eg_score += eg_value[BISHOP] + eg_psqt[BISHOP][sq]; }
    bb = w_rooks; while (bb) { int sq = pop_lsb(&bb); mg_score += mg_value[ROOK] + mg_psqt[ROOK][sq]; eg_score += eg_value[ROOK] + eg_psqt[ROOK][sq]; }
    bb = w_queens; while (bb) { int sq = pop_lsb(&bb); mg_score += mg_value[QUEEN] + mg_psqt[QUEEN][sq]; eg_score += eg_value[QUEEN] + eg_psqt[QUEEN][sq]; }
    bb = w_king; while (bb) { int sq = pop_lsb(&bb); mg_score += mg_value[KING] + mg_psqt[KING][sq]; eg_score += eg_value[KING] + eg_psqt[KING][sq]; }

    // Evaluate Black pieces (mirror squares)
    bb = b_pawns; while (bb) { int sq = pop_lsb(&bb); mg_score -= (mg_value[PAWN] + mg_psqt[PAWN][sq ^ 56]); eg_score -= (eg_value[PAWN] + eg_psqt[PAWN][sq ^ 56]); }
    bb = b_knights; while (bb) { int sq = pop_lsb(&bb); mg_score -= (mg_value[KNIGHT] + mg_psqt[KNIGHT][sq ^ 56]); eg_score -= (eg_value[KNIGHT] + eg_psqt[KNIGHT][sq ^ 56]); }
    bb = b_bishops; while (bb) { int sq = pop_lsb(&bb); mg_score -= (mg_value[BISHOP] + mg_psqt[BISHOP][sq ^ 56]); eg_score -= (eg_value[BISHOP] + eg_psqt[BISHOP][sq ^ 56]); }
    bb = b_rooks; while (bb) { int sq = pop_lsb(&bb); mg_score -= (mg_value[ROOK] + mg_psqt[ROOK][sq ^ 56]); eg_score -= (eg_value[ROOK] + eg_psqt[ROOK][sq ^ 56]); }
    bb = b_queens; while (bb) { int sq = pop_lsb(&bb); mg_score -= (mg_value[QUEEN] + mg_psqt[QUEEN][sq ^ 56]); eg_score -= (eg_value[QUEEN] + eg_psqt[QUEEN][sq ^ 56]); }
    bb = b_king; while (bb) { int sq = pop_lsb(&bb); mg_score -= (mg_value[KING] + mg_psqt[KING][sq ^ 56]); eg_score -= (eg_value[KING] + eg_psqt[KING][sq ^ 56]); }

    // Castling evaluation
    int white_castling_bonus = 0;
    bool white_castled = (pos->board[G1] == KING && (pos->color_bbs[WHITE] & (1ULL << F1)) && pos->board[F1] == ROOK) ||
                         (pos->board[C1] == KING && (pos->color_bbs[WHITE] & (1ULL << D1)) && pos->board[D1] == ROOK);
    if (white_castled) {
        white_castling_bonus = 60;
    } else {
        if (pos->castling_rights & WK_CASTLE) white_castling_bonus += 30;
        if (pos->castling_rights & WQ_CASTLE) white_castling_bonus += 30;
    }

    int black_castling_bonus = 0;
    bool black_castled = (pos->board[G8] == KING && (pos->color_bbs[BLACK] & (1ULL << F8)) && pos->board[F8] == ROOK) ||
                         (pos->board[C8] == KING && (pos->color_bbs[BLACK] & (1ULL << D8)) && pos->board[D8] == ROOK);
    if (black_castled) {
        black_castling_bonus = 60;
    } else {
        if (pos->castling_rights & BK_CASTLE) black_castling_bonus += 30;
        if (pos->castling_rights & BQ_CASTLE) black_castling_bonus += 30;
    }

    mg_score += white_castling_bonus;
    mg_score -= black_castling_bonus;

    // Endgame mop-up evaluation for decisive material advantage
    int w_mat = count_bits(w_pawns)*100 + count_bits(w_knights)*300 + count_bits(w_bishops)*300 + count_bits(w_rooks)*500 + count_bits(w_queens)*900;
    int b_mat = count_bits(b_pawns)*100 + count_bits(b_knights)*300 + count_bits(b_bishops)*300 + count_bits(b_rooks)*500 + count_bits(b_queens)*900;
    if (w_mat - b_mat >= 300 && b_mat <= 300) {
        eg_score += evaluate_mop_up(pos, WHITE);
    } else if (b_mat - w_mat >= 300 && w_mat <= 300) {
        eg_score -= evaluate_mop_up(pos, BLACK);
    }

    // Opening Piece Development & Unblocking Evaluation (Middlegame phase >= 12)
    if (phase >= 12) {
        int w_dev_score = 0;
        int b_dev_score = 0;

        // 1. Penalize minor pieces staying on home squares
        if (pos->board[B1] == KNIGHT && (pos->color_bbs[WHITE] & (1ULL << B1))) w_dev_score -= 25;
        if (pos->board[G1] == KNIGHT && (pos->color_bbs[WHITE] & (1ULL << G1))) w_dev_score -= 25;
        if (pos->board[C1] == BISHOP && (pos->color_bbs[WHITE] & (1ULL << C1))) w_dev_score -= 25;
        if (pos->board[F1] == BISHOP && (pos->color_bbs[WHITE] & (1ULL << F1))) w_dev_score -= 25;

        if (pos->board[B8] == KNIGHT && (pos->color_bbs[BLACK] & (1ULL << B8))) b_dev_score -= 25;
        if (pos->board[G8] == KNIGHT && (pos->color_bbs[BLACK] & (1ULL << G8))) b_dev_score -= 25;
        if (pos->board[C8] == BISHOP && (pos->color_bbs[BLACK] & (1ULL << C8))) b_dev_score -= 25;
        if (pos->board[F8] == BISHOP && (pos->color_bbs[BLACK] & (1ULL << F8))) b_dev_score -= 25;

        // 2. Penalize unmoved or blocked central pawns on d2/e2 (White) and d7/e7 (Black)
        if (pos->board[D2] == PAWN && (pos->color_bbs[WHITE] & (1ULL << D2))) {
            w_dev_score -= 15;
            if (pos->color_bbs[WHITE] & (1ULL << D3)) w_dev_score -= 35; // Piece blocking d2 pawn
        }
        if (pos->board[E2] == PAWN && (pos->color_bbs[WHITE] & (1ULL << E2))) {
            w_dev_score -= 15;
            if (pos->color_bbs[WHITE] & (1ULL << E3)) w_dev_score -= 35; // Piece blocking e2 pawn
        }

        if (pos->board[D7] == PAWN && (pos->color_bbs[BLACK] & (1ULL << D7))) {
            b_dev_score -= 15;
            if (pos->color_bbs[BLACK] & (1ULL << D6)) b_dev_score -= 35; // Piece blocking d7 pawn
        }
        if (pos->board[E7] == PAWN && (pos->color_bbs[BLACK] & (1ULL << E7))) {
            b_dev_score -= 15;
            if (pos->color_bbs[BLACK] & (1ULL << E6)) b_dev_score -= 35; // Piece blocking e7 pawn
        }

        // 3. Penalize premature Queen outings before minor pieces are developed
        if (pos->board[D1] != QUEEN || !(pos->color_bbs[WHITE] & (1ULL << D1))) {
            int w_home_minors = (pos->board[B1] == KNIGHT) + (pos->board[G1] == KNIGHT) +
                                (pos->board[C1] == BISHOP) + (pos->board[F1] == BISHOP);
            if (w_home_minors >= 3) w_dev_score -= 30;
        }
        if (pos->board[D8] != QUEEN || !(pos->color_bbs[BLACK] & (1ULL << D8))) {
            int b_home_minors = (pos->board[B8] == KNIGHT) + (pos->board[G8] == KNIGHT) +
                                (pos->board[C8] == BISHOP) + (pos->board[F8] == BISHOP);
            if (b_home_minors >= 3) b_dev_score -= 30;
        }

        // 4. Calibrated Penalty for Trapped Bishops blocked by own pawns (-25 cp)
        // White C1 Bishop trapped by BOTH B2 and D2 pawns
        if (pos->board[C1] == BISHOP && (pos->color_bbs[WHITE] & (1ULL << C1))) {
            if ((pos->board[B2] == PAWN && (pos->color_bbs[WHITE] & (1ULL << B2))) &&
                (pos->board[D2] == PAWN && (pos->color_bbs[WHITE] & (1ULL << D2)))) {
                w_dev_score -= 25; // Trapped C1 bishop has 0 exit diagonals!
            }
        }
        // White F1 Bishop trapped by BOTH E2 and G2 pawns
        if (pos->board[F1] == BISHOP && (pos->color_bbs[WHITE] & (1ULL << F1))) {
            if ((pos->board[E2] == PAWN && (pos->color_bbs[WHITE] & (1ULL << E2))) &&
                (pos->board[G2] == PAWN && (pos->color_bbs[WHITE] & (1ULL << G2)))) {
                w_dev_score -= 25; // Trapped F1 bishop has 0 exit diagonals!
            }
        }

        // Black C8 Bishop trapped by BOTH B7 and D7 pawns
        if (pos->board[C8] == BISHOP && (pos->color_bbs[BLACK] & (1ULL << C8))) {
            if ((pos->board[B7] == PAWN && (pos->color_bbs[BLACK] & (1ULL << B7))) &&
                (pos->board[D7] == PAWN && (pos->color_bbs[BLACK] & (1ULL << D7)))) {
                b_dev_score -= 25; // Trapped C8 bishop has 0 exit diagonals!
            }
        }
        // Black F8 Bishop trapped by BOTH E7 and G7 pawns
        if (pos->board[F8] == BISHOP && (pos->color_bbs[BLACK] & (1ULL << F8))) {
            if ((pos->board[E7] == PAWN && (pos->color_bbs[BLACK] & (1ULL << E7))) &&
                (pos->board[G7] == PAWN && (pos->color_bbs[BLACK] & (1ULL << G7)))) {
                b_dev_score -= 25; // Trapped F8 bishop has 0 exit diagonals!
            }
        }

        mg_score += w_dev_score;
        mg_score -= b_dev_score;
    }

    // 5. O(1) Bishop Mobility Evaluation (reward open diagonals, penalize trapped bishops anywhere)
    bb = w_bishops;
    while (bb) {
        int sq = pop_lsb(&bb);
        uint64_t attacks = get_bishop_attacks(sq, pos->color_bbs[WHITE] | pos->color_bbs[BLACK]);
        attacks &= ~pos->color_bbs[WHITE]; // non-friendly squares
        int mob = count_bits(attacks);
        if (mob <= 2) {
            mg_score -= (3 - mob) * 15; // Penalty for restricted/trapped bishop
        } else {
            mg_score += (mob - 2) * 5;  // Reward active bishop mobility
        }
    }

    bb = b_bishops;
    while (bb) {
        int sq = pop_lsb(&bb);
        uint64_t attacks = get_bishop_attacks(sq, pos->color_bbs[WHITE] | pos->color_bbs[BLACK]);
        attacks &= ~pos->color_bbs[BLACK]; // non-friendly squares
        int mob = count_bits(attacks);
        if (mob <= 2) {
            mg_score += (3 - mob) * 15; // Penalty for Black's restricted bishop
        } else {
            mg_score -= (mob - 2) * 5;  // Reward Black's active bishop mobility
        }
    }

    // Tapered evaluation interpolation
    int score = ((mg_score * phase) + (eg_score * (24 - phase))) / 24;

    // Return score from perspective of side to move
    return (pos->side == WHITE) ? score : -score;
}
