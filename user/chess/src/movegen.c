/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/src/movegen.c),
 * H4, plan/phase9_chess_computer.md. No platform dependency at all.
 */

#include "movegen.h"
#include "bitboard.h"
#include "move.h"

// Generate all pseudo-legal moves (quiets and captures)
void generate_moves(const Position *pos, MoveList *list) {
    list->count = 0;

    int us = pos->side;
    int them = us ^ 1;
    uint64_t us_pieces = pos->color_bbs[us];
    uint64_t them_pieces = pos->color_bbs[them];
    uint64_t occupied = us_pieces | them_pieces;
    uint64_t empty = ~occupied;

    // 1. Pawn Moves
    uint64_t pawns = pos->piece_bbs[PAWN] & us_pieces;
    if (us == WHITE) {
        // Single push
        uint64_t single_push = (pawns << 8) & empty;
        uint64_t single_push_tmp = single_push;
        while (single_push_tmp) {
            int to = pop_lsb(&single_push_tmp);
            int from = to - 8;
            if (to >= A8) { // Promotion rank
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_Q));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_R));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_B));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_N));
            } else {
                add_move(list, build_move(from, to, 0));
            }
        }

        // Double push
        uint64_t double_push = (((pawns & rank_masks[RANK_2]) << 8) & empty);
        double_push = (double_push << 8) & empty;
        while (double_push) {
            int to = pop_lsb(&double_push);
            int from = to - 16;
            add_move(list, build_move(from, to, MOVE_FLAG_DOUBLE_PUSH));
        }

        // Captures East
        uint64_t cap_east = (pawns << 9) & them_pieces & not_a_file;
        while (cap_east) {
            int to = pop_lsb(&cap_east);
            int from = to - 9;
            if (to >= A8) {
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // Captures West
        uint64_t cap_west = (pawns << 7) & them_pieces & not_h_file;
        while (cap_west) {
            int to = pop_lsb(&cap_west);
            int from = to - 7;
            if (to >= A8) {
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // En-passant capture
        if (pos->en_passant != NO_SQ) {
            int ep_sq = pos->en_passant;
            if (ep_sq % 8 != 0) { // not File A
                int from = ep_sq - 9;
                if (get_bit(pawns, from)) {
                    add_move(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
            if (ep_sq % 8 != 7) { // not File H
                int from = ep_sq - 7;
                if (get_bit(pawns, from)) {
                    add_move(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
        }
    } else { // us == BLACK
        // Single push
        uint64_t single_push = (pawns >> 8) & empty;
        uint64_t single_push_tmp = single_push;
        while (single_push_tmp) {
            int to = pop_lsb(&single_push_tmp);
            int from = to + 8;
            if (to <= H1) { // Promotion rank
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_Q));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_R));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_B));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_N));
            } else {
                add_move(list, build_move(from, to, 0));
            }
        }

        // Double push
        uint64_t double_push = (((pawns & rank_masks[RANK_7]) >> 8) & empty);
        double_push = (double_push >> 8) & empty;
        while (double_push) {
            int to = pop_lsb(&double_push);
            int from = to + 16;
            add_move(list, build_move(from, to, MOVE_FLAG_DOUBLE_PUSH));
        }

        // Captures East (from black perspective, capturing to east means index decreases by 7)
        uint64_t cap_east = (pawns >> 7) & them_pieces & not_a_file;
        while (cap_east) {
            int to = pop_lsb(&cap_east);
            int from = to + 7;
            if (to <= H1) {
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // Captures West (from black perspective, capturing to west means index decreases by 9)
        uint64_t cap_west = (pawns >> 9) & them_pieces & not_h_file;
        while (cap_west) {
            int to = pop_lsb(&cap_west);
            int from = to + 9;
            if (to <= H1) {
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_move(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // En-passant capture
        if (pos->en_passant != NO_SQ) {
            int ep_sq = pos->en_passant;
            if (ep_sq % 8 != 0) { // not File A
                int from = ep_sq + 7;
                if (get_bit(pawns, from)) {
                    add_move(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
            if (ep_sq % 8 != 7) { // not File H
                int from = ep_sq + 9;
                if (get_bit(pawns, from)) {
                    add_move(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
        }
    }

    // 2. Knight Moves
    uint64_t knights = pos->piece_bbs[KNIGHT] & us_pieces;
    while (knights) {
        int from = pop_lsb(&knights);
        uint64_t attacks = get_knight_attacks(from) & ~us_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            if (get_bit(them_pieces, to)) {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            } else {
                add_move(list, build_move(from, to, 0));
            }
        }
    }

    // 3. Bishop Moves
    uint64_t bishops = pos->piece_bbs[BISHOP] & us_pieces;
    while (bishops) {
        int from = pop_lsb(&bishops);
        uint64_t attacks = get_bishop_attacks(from, occupied) & ~us_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            if (get_bit(them_pieces, to)) {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            } else {
                add_move(list, build_move(from, to, 0));
            }
        }
    }

    // 4. Rook Moves
    uint64_t rooks = pos->piece_bbs[ROOK] & us_pieces;
    while (rooks) {
        int from = pop_lsb(&rooks);
        uint64_t attacks = get_rook_attacks(from, occupied) & ~us_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            if (get_bit(them_pieces, to)) {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            } else {
                add_move(list, build_move(from, to, 0));
            }
        }
    }

    // 5. Queen Moves
    uint64_t queens = pos->piece_bbs[QUEEN] & us_pieces;
    while (queens) {
        int from = pop_lsb(&queens);
        uint64_t attacks = get_queen_attacks(from, occupied) & ~us_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            if (get_bit(them_pieces, to)) {
                add_move(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            } else {
                add_move(list, build_move(from, to, 0));
            }
        }
    }

    // 6. King Moves
    uint64_t king = pos->piece_bbs[KING] & us_pieces;
    int king_sq = get_lsb(king);
    uint64_t king_attacks_bb = get_king_attacks(king_sq) & ~us_pieces;
    while (king_attacks_bb) {
        int to = pop_lsb(&king_attacks_bb);
        if (get_bit(them_pieces, to)) {
            add_move(list, build_move(king_sq, to, MOVE_FLAG_CAPTURE));
        } else {
            add_move(list, build_move(king_sq, to, 0));
        }
    }

    // Castling rights
    if (us == WHITE) {
        if (!is_square_attacked(pos, E1, BLACK)) {
            if ((pos->castling_rights & WK_CASTLE) && !get_bit(occupied, F1) && !get_bit(occupied, G1)) {
                if (!is_square_attacked(pos, F1, BLACK) && !is_square_attacked(pos, G1, BLACK)) {
                    add_move(list, build_move(E1, G1, MOVE_FLAG_K_CASTLE));
                }
            }
            if ((pos->castling_rights & WQ_CASTLE) && !get_bit(occupied, D1) && !get_bit(occupied, C1) && !get_bit(occupied, B1)) {
                if (!is_square_attacked(pos, D1, BLACK) && !is_square_attacked(pos, C1, BLACK)) {
                    add_move(list, build_move(E1, C1, MOVE_FLAG_Q_CASTLE));
                }
            }
        }
    } else { // BLACK
        if (!is_square_attacked(pos, E8, WHITE)) {
            if ((pos->castling_rights & BK_CASTLE) && !get_bit(occupied, F8) && !get_bit(occupied, G8)) {
                if (!is_square_attacked(pos, F8, WHITE) && !is_square_attacked(pos, G8, WHITE)) {
                    add_move(list, build_move(E8, G8, MOVE_FLAG_K_CASTLE));
                }
            }
            if ((pos->castling_rights & BQ_CASTLE) && !get_bit(occupied, D8) && !get_bit(occupied, C8) && !get_bit(occupied, B8)) {
                if (!is_square_attacked(pos, D8, WHITE) && !is_square_attacked(pos, C8, WHITE)) {
                    add_move(list, build_move(E8, C8, MOVE_FLAG_Q_CASTLE));
                }
            }
        }
    }
}

// Generate captures only (and promotions) for Quiescence Search
void generate_captures(const Position *pos, CaptureList *list) {
    list->count = 0;

    int us = pos->side;
    int them = us ^ 1;
    uint64_t us_pieces = pos->color_bbs[us];
    uint64_t them_pieces = pos->color_bbs[them];
    uint64_t occupied = us_pieces | them_pieces;
    uint64_t empty = ~occupied;

    // 1. Pawn Captures & Promotions
    uint64_t pawns = pos->piece_bbs[PAWN] & us_pieces;
    if (us == WHITE) {
        // Promotions (Quiet pushes to rank 8)
        uint64_t single_push = (pawns << 8) & empty & rank_masks[RANK_8];
        while (single_push) {
            int to = pop_lsb(&single_push);
            int from = to - 8;
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_Q));
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_R));
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_B));
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_N));
        }

        // Captures East
        uint64_t cap_east = (pawns << 9) & them_pieces & not_a_file;
        while (cap_east) {
            int to = pop_lsb(&cap_east);
            int from = to - 9;
            if (to >= A8) {
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // Captures West
        uint64_t cap_west = (pawns << 7) & them_pieces & not_h_file;
        while (cap_west) {
            int to = pop_lsb(&cap_west);
            int from = to - 7;
            if (to >= A8) {
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // En-passant capture
        if (pos->en_passant != NO_SQ) {
            int ep_sq = pos->en_passant;
            if (ep_sq % 8 != 0) { // not File A
                int from = ep_sq - 9;
                if (get_bit(pawns, from)) {
                    add_capture(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
            if (ep_sq % 8 != 7) { // not File H
                int from = ep_sq - 7;
                if (get_bit(pawns, from)) {
                    add_capture(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
        }
    } else { // us == BLACK
        // Promotions (Quiet pushes to rank 1)
        uint64_t single_push = (pawns >> 8) & empty & rank_masks[RANK_1];
        while (single_push) {
            int to = pop_lsb(&single_push);
            int from = to + 8;
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_Q));
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_R));
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_B));
            add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_N));
        }

        // Captures East
        uint64_t cap_east = (pawns >> 7) & them_pieces & not_a_file;
        while (cap_east) {
            int to = pop_lsb(&cap_east);
            int from = to + 7;
            if (to <= H1) {
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // Captures West
        uint64_t cap_west = (pawns >> 9) & them_pieces & not_h_file;
        while (cap_west) {
            int to = pop_lsb(&cap_west);
            int from = to + 9;
            if (to <= H1) {
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_Q));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_R));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_B));
                add_capture(list, build_move(from, to, MOVE_FLAG_PROMO_CAP_N));
            } else {
                add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
            }
        }

        // En-passant capture
        if (pos->en_passant != NO_SQ) {
            int ep_sq = pos->en_passant;
            if (ep_sq % 8 != 0) { // not File A
                int from = ep_sq + 7;
                if (get_bit(pawns, from)) {
                    add_capture(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
            if (ep_sq % 8 != 7) { // not File H
                int from = ep_sq + 9;
                if (get_bit(pawns, from)) {
                    add_capture(list, build_move(from, ep_sq, MOVE_FLAG_EN_PASSANT));
                }
            }
        }
    }

    // 2. Knight Captures
    uint64_t knights = pos->piece_bbs[KNIGHT] & us_pieces;
    while (knights) {
        int from = pop_lsb(&knights);
        uint64_t attacks = get_knight_attacks(from) & them_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
        }
    }

    // 3. Bishop Captures
    uint64_t bishops = pos->piece_bbs[BISHOP] & us_pieces;
    while (bishops) {
        int from = pop_lsb(&bishops);
        uint64_t attacks = get_bishop_attacks(from, occupied) & them_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
        }
    }

    // 4. Rook Captures
    uint64_t rooks = pos->piece_bbs[ROOK] & us_pieces;
    while (rooks) {
        int from = pop_lsb(&rooks);
        uint64_t attacks = get_rook_attacks(from, occupied) & them_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
        }
    }

    // 5. Queen Captures
    uint64_t queens = pos->piece_bbs[QUEEN] & us_pieces;
    while (queens) {
        int from = pop_lsb(&queens);
        uint64_t attacks = get_queen_attacks(from, occupied) & them_pieces;
        while (attacks) {
            int to = pop_lsb(&attacks);
            add_capture(list, build_move(from, to, MOVE_FLAG_CAPTURE));
        }
    }

    // 6. King Captures
    uint64_t king = pos->piece_bbs[KING] & us_pieces;
    int king_sq = get_lsb(king);
    uint64_t king_attacks_bb = get_king_attacks(king_sq) & them_pieces;
    while (king_attacks_bb) {
        int to = pop_lsb(&king_attacks_bb);
        add_capture(list, build_move(king_sq, to, MOVE_FLAG_CAPTURE));
    }
}
