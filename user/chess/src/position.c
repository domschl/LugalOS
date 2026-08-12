/*
 * Vendored from ~/gith/domschl/LugalChess (engine/src/position.c), H4,
 * plan/phase9_chess_computer.md. Two changes from upstream:
 *   - print_position_info()'s hash line reformatted the same way
 *     bitboard.c's print_bitboard() was (no %llx -- see chess_platform.h).
 *   - generate_fen()'s single snprintf() call replaced with a small
 *     hand-written decimal-int appender (chess_platform.h provides atoi but
 *     not a general snprintf; this is the only site that needed one).
 */

#include "position.h"
#include "zobrist.h"
#include "bitboard.h"

// Castling rights update mask for each square on the board
static const int castling_rights_mask[64] = {
    13, 15, 15, 15, 12, 15, 15, 14, // A1..H1 (E1=12: clears WK/WQ, A1=13: clears WQ, H1=14: clears WK)
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
     7, 15, 15, 15,  3, 15, 15, 11  // A8..H8 (E8=3: clears BK/BQ, A8=7: clears BQ, H8=11: clears BK)
};

// Generate full hash key from scratch
uint64_t generate_hash_key(const Position *pos) {
    uint64_t key = 0ULL;

    // Pieces
    for (int sq = 0; sq < 64; sq++) {
        int piece = pos->board[sq];
        if (piece != NO_PIECE) {
            int color = get_bit(pos->color_bbs[WHITE], sq) ? WHITE : BLACK;
            key ^= zobrist_pieces[piece][color][sq];
        }
    }

    // Castling
    key ^= zobrist_castling[pos->castling_rights];

    // En-passant
    if (pos->en_passant != NO_SQ) {
        key ^= zobrist_en_passant[pos->en_passant];
    }

    // Side
    if (pos->side == BLACK) {
        key ^= zobrist_side;
    }

    return key;
}

// Check if a square is attacked by any piece of attacker_side
bool is_square_attacked(const Position *pos, int square, int attacker_side) {
    if (square < 0 || square >= 64) return false;

    // Pawn attacks
    int defender_side = attacker_side ^ 1;
    uint64_t pawns = pos->piece_bbs[PAWN] & pos->color_bbs[attacker_side];
    if (get_pawn_attacks(defender_side, square) & pawns) return true;

    // Knight attacks
    uint64_t knights = pos->piece_bbs[KNIGHT] & pos->color_bbs[attacker_side];
    if (get_knight_attacks(square) & knights) return true;

    // Bishop & Queen attacks (diagonal)
    uint64_t occupied = pos->color_bbs[WHITE] | pos->color_bbs[BLACK];
    uint64_t diagonal_sliders = (pos->piece_bbs[BISHOP] | pos->piece_bbs[QUEEN]) & pos->color_bbs[attacker_side];
    if (get_bishop_attacks(square, occupied) & diagonal_sliders) return true;

    // Rook & Queen attacks (orthogonal)
    uint64_t orthogonal_sliders = (pos->piece_bbs[ROOK] | pos->piece_bbs[QUEEN]) & pos->color_bbs[attacker_side];
    if (get_rook_attacks(square, occupied) & orthogonal_sliders) return true;

    // King attacks
    uint64_t king = pos->piece_bbs[KING] & pos->color_bbs[attacker_side];
    if (get_king_attacks(square) & king) return true;

    return false;
}

// Validate whether a position meets standard chess rules
bool is_position_valid(const Position *pos) {
    if (pos == NULL) return false;

    // 1. Must have exactly 1 White King and 1 Black King
    int w_kings = count_bits(pos->piece_bbs[KING] & pos->color_bbs[WHITE]);
    int b_kings = count_bits(pos->piece_bbs[KING] & pos->color_bbs[BLACK]);
    if (w_kings != 1 || b_kings != 1) return false;

    // 2. The side NOT to move cannot be in check (illegal board state)
    int other_side = pos->side ^ 1;
    int other_king_sq = get_lsb(pos->piece_bbs[KING] & pos->color_bbs[other_side]);
    if (other_king_sq != NO_SQ && is_square_attacked(pos, other_king_sq, pos->side)) {
        return false;
    }

    return true;
}

// Parse FEN string and initialize board state
void parse_fen(Position *pos, const char *fen) {
    memset(pos, 0, sizeof(Position));
    for (int sq = 0; sq < 64; sq++) pos->board[sq] = NO_PIECE;

    pos->en_passant = NO_SQ;
    pos->side = WHITE;
    pos->fullmove = 1;
    pos->halfmove = 0;

    if (fen == NULL) return;

    int rank = 7;
    int file = 0;
    const char *char_ptr = fen;

    // 1. Parse piece placement
    while (*char_ptr != ' ' && *char_ptr != '\0') {
        if (*char_ptr == '/') {
            rank--;
            file = 0;
        } else if (*char_ptr >= '1' && *char_ptr <= '8') {
            file += *char_ptr - '0';
        } else {
            int color = (*char_ptr >= 'a' && *char_ptr <= 'z') ? BLACK : WHITE;
            int piece = NO_PIECE;
            char lower_char = (*char_ptr >= 'a' && *char_ptr <= 'z') ? *char_ptr : (*char_ptr + 32);

            switch (lower_char) {
                case 'p': piece = PAWN; break;
                case 'n': piece = KNIGHT; break;
                case 'b': piece = BISHOP; break;
                case 'r': piece = ROOK; break;
                case 'q': piece = QUEEN; break;
                case 'k': piece = KING; break;
            }

            if (piece != NO_PIECE) {
                if (rank >= 0 && rank < 8 && file >= 0 && file < 8) {
                    int sq = rank * 8 + file;
                    pos->board[sq] = piece;
                    set_bit(pos->piece_bbs[piece], sq);
                    set_bit(pos->color_bbs[color], sq);
                }
            }
            file++;
        }
        char_ptr++;
    }

    if (*char_ptr == '\0') {
        pos->history_ply = 0;
        pos->hash_key = generate_hash_key(pos);
        return;
    }

    char_ptr++; // skip space

    // 2. Parse active color
    if (*char_ptr != '\0') {
        pos->side = (*char_ptr == 'w') ? WHITE : BLACK;
        char_ptr++;
        if (*char_ptr == ' ') char_ptr++;
    }

    // 3. Parse castling rights
    pos->castling_rights = 0;
    while (*char_ptr != ' ' && *char_ptr != '\0') {
        switch (*char_ptr) {
            case 'K': pos->castling_rights |= WK_CASTLE; break;
            case 'Q': pos->castling_rights |= WQ_CASTLE; break;
            case 'k': pos->castling_rights |= BK_CASTLE; break;
            case 'q': pos->castling_rights |= BQ_CASTLE; break;
            case '-': break;
        }
        char_ptr++;
    }

    if (*char_ptr == ' ') char_ptr++;

    // 4. Parse en-passant square
    if (*char_ptr == '-') {
        pos->en_passant = NO_SQ;
        char_ptr++;
    } else if (char_ptr[0] >= 'a' && char_ptr[0] <= 'h' && char_ptr[1] >= '1' && char_ptr[1] <= '8') {
        int f = char_ptr[0] - 'a';
        int r = char_ptr[1] - '1';
        pos->en_passant = r * 8 + f;
        char_ptr += 2;
    }

    // 5. Parse halfmove and fullmove if present
    if (*char_ptr == ' ') {
        char_ptr++;
        pos->halfmove = atoi(char_ptr);
        while (*char_ptr != ' ' && *char_ptr != '\0') char_ptr++;
        if (*char_ptr == ' ') {
            char_ptr++;
            pos->fullmove = atoi(char_ptr);
        } else {
            pos->fullmove = 1;
        }
    } else {
        pos->halfmove = 0;
        pos->fullmove = 1;
    }

    pos->history_ply = 0;
    pos->hash_key = generate_hash_key(pos);
}

// Make a move on the board. Returns false if move was illegal (leaves King in check).
bool make_move(Position *pos, Move move) {
    if (pos == NULL) return false;
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    if (from < 0 || from >= 64 || to < 0 || to >= 64) {
        return false;
    }
    if (pos->history_ply < 0 || pos->history_ply >= MAX_PLYS - 1) {
        return false;
    }
    int us = pos->side;
    int them = us ^ 1;
    int moved_piece = pos->board[from];
    if (moved_piece == NO_PIECE || moved_piece < PAWN || moved_piece > KING) {
        return false;
    }
    if ((pos->color_bbs[us] & (1ULL << from)) == 0) {
        return false;
    }
    int flags = MOVE_FLAGS(move);
    int cap_piece = NO_PIECE;
    int cap_sq = to;

    // Save history state
    pos->history[pos->history_ply].move = move;
    pos->history[pos->history_ply].en_passant = pos->en_passant;
    pos->history[pos->history_ply].castling_rights = pos->castling_rights;
    pos->history[pos->history_ply].halfmove = pos->halfmove;
    pos->history[pos->history_ply].fullmove = pos->fullmove;
    pos->history[pos->history_ply].hash_key = pos->hash_key;

    // Determine capture
    if (move_is_capture(move)) {
        if (flags == MOVE_FLAG_EN_PASSANT) {
            cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
            cap_piece = PAWN;
        } else {
            cap_piece = pos->board[to];
        }
        pos->history[pos->history_ply].captured_piece = cap_piece;

        // Remove captured piece from bitboards
        clear_bit(pos->piece_bbs[cap_piece], cap_sq);
        clear_bit(pos->color_bbs[them], cap_sq);
        pos->board[cap_sq] = NO_PIECE;

        // Update hash for captured piece
        pos->hash_key ^= zobrist_pieces[cap_piece][them][cap_sq];

        pos->halfmove = 0; // reset 50-move rule on capture
    } else {
        pos->history[pos->history_ply].captured_piece = NO_PIECE;
        pos->halfmove++;
    }

    // Hash updates for en-passant square
    if (pos->en_passant != NO_SQ) {
        pos->hash_key ^= zobrist_en_passant[pos->en_passant];
    }

    // Hash updates for castling rights
    pos->hash_key ^= zobrist_castling[pos->castling_rights];

    // Update castling rights
    pos->castling_rights &= castling_rights_mask[from] & castling_rights_mask[to];

    // Re-hash new castling rights
    pos->hash_key ^= zobrist_castling[pos->castling_rights];

    // Remove moved piece from source
    clear_bit(pos->piece_bbs[moved_piece], from);
    clear_bit(pos->color_bbs[us], from);
    pos->board[from] = NO_PIECE;
    pos->hash_key ^= zobrist_pieces[moved_piece][us][from];

    // Move piece or promote
    if (move_is_promo(move)) {
        int promo = move_promo_piece(move);
        set_bit(pos->piece_bbs[promo], to);
        set_bit(pos->color_bbs[us], to);
        pos->board[to] = promo;
        pos->hash_key ^= zobrist_pieces[promo][us][to];
    } else {
        set_bit(pos->piece_bbs[moved_piece], to);
        set_bit(pos->color_bbs[us], to);
        pos->board[to] = moved_piece;
        pos->hash_key ^= zobrist_pieces[moved_piece][us][to];
    }

    // Handle Pawn double pushes (and update EP square)
    if (flags == MOVE_FLAG_DOUBLE_PUSH) {
        pos->en_passant = (us == WHITE) ? (from + 8) : (from - 8);
        pos->hash_key ^= zobrist_en_passant[pos->en_passant];
    } else {
        pos->en_passant = NO_SQ;
    }

    // Reset halfmove clock if Pawn moved
    if (moved_piece == PAWN) {
        pos->halfmove = 0;
    }

    // Handle Castling moves
    if (flags == MOVE_FLAG_K_CASTLE) {
        int r_from = (us == WHITE) ? H1 : H8;
        int r_to = (us == WHITE) ? F1 : F8;
        clear_bit(pos->piece_bbs[ROOK], r_from);
        clear_bit(pos->color_bbs[us], r_from);
        set_bit(pos->piece_bbs[ROOK], r_to);
        set_bit(pos->color_bbs[us], r_to);
        pos->board[r_from] = NO_PIECE;
        pos->board[r_to] = ROOK;
        pos->hash_key ^= zobrist_pieces[ROOK][us][r_from] ^ zobrist_pieces[ROOK][us][r_to];
    } else if (flags == MOVE_FLAG_Q_CASTLE) {
        int r_from = (us == WHITE) ? A1 : A8;
        int r_to = (us == WHITE) ? D1 : D8;
        clear_bit(pos->piece_bbs[ROOK], r_from);
        clear_bit(pos->color_bbs[us], r_from);
        set_bit(pos->piece_bbs[ROOK], r_to);
        set_bit(pos->color_bbs[us], r_to);
        pos->board[r_from] = NO_PIECE;
        pos->board[r_to] = ROOK;
        pos->hash_key ^= zobrist_pieces[ROOK][us][r_from] ^ zobrist_pieces[ROOK][us][r_to];
    }

    // Finalize side turn (fully execute)
    pos->side = them;
    pos->hash_key ^= zobrist_side;
    if (pos->side == WHITE) {
        pos->fullmove++;
    }
    pos->history_ply++;

    // Check King safety (verify legality) for the side that just moved (us)
    int king_sq = get_lsb(pos->piece_bbs[KING] & pos->color_bbs[us]);
    if (is_square_attacked(pos, king_sq, them)) {
        // Legal violation! Roll back changes.
        unmake_move(pos);
        return false;
    }

    return true;
}


// Unmake a previously executed move
void unmake_move(Position *pos) {
    if (pos == NULL || pos->history_ply <= 0) {
        return;
    }
    pos->history_ply--;

    Move move = pos->history[pos->history_ply].move;
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    int flags = MOVE_FLAGS(move);

    // Toggle side back
    pos->side ^= 1;

    int us = pos->side;
    int them = us ^ 1;

    int moved_piece = pos->board[to];

    if (move_is_promo(move)) {
        // If it was a promotion, the moved piece on the board is currently the promoted piece,
        // but the original piece that moved was a PAWN.
        clear_bit(pos->piece_bbs[moved_piece], to);
        clear_bit(pos->color_bbs[us], to);
        moved_piece = PAWN;
    } else {
        clear_bit(pos->piece_bbs[moved_piece], to);
        clear_bit(pos->color_bbs[us], to);
    }

    pos->board[to] = NO_PIECE;

    // Put moved piece back to starting square
    set_bit(pos->piece_bbs[moved_piece], from);
    set_bit(pos->color_bbs[us], from);
    pos->board[from] = moved_piece;

    // Restore captured piece if any
    if (move_is_capture(move)) {
        int cap_piece = pos->history[pos->history_ply].captured_piece;
        int cap_sq = to;
        if (flags == MOVE_FLAG_EN_PASSANT) {
            cap_sq = (us == WHITE) ? (to - 8) : (to + 8);
        }
        set_bit(pos->piece_bbs[cap_piece], cap_sq);
        set_bit(pos->color_bbs[them], cap_sq);
        pos->board[cap_sq] = cap_piece;
    }

    // Restore Rook in castling
    if (flags == MOVE_FLAG_K_CASTLE) {
        int r_from = (us == WHITE) ? H1 : H8;
        int r_to = (us == WHITE) ? F1 : F8;
        clear_bit(pos->piece_bbs[ROOK], r_to);
        clear_bit(pos->color_bbs[us], r_to);
        set_bit(pos->piece_bbs[ROOK], r_from);
        set_bit(pos->color_bbs[us], r_from);
        pos->board[r_to] = NO_PIECE;
        pos->board[r_from] = ROOK;
    } else if (flags == MOVE_FLAG_Q_CASTLE) {
        int r_from = (us == WHITE) ? A1 : A8;
        int r_to = (us == WHITE) ? D1 : D8;
        clear_bit(pos->piece_bbs[ROOK], r_to);
        clear_bit(pos->color_bbs[us], r_to);
        set_bit(pos->piece_bbs[ROOK], r_from);
        set_bit(pos->color_bbs[us], r_from);
        pos->board[r_to] = NO_PIECE;
        pos->board[r_from] = ROOK;
    }

    // Restore history variables
    pos->en_passant = pos->history[pos->history_ply].en_passant;
    pos->castling_rights = pos->history[pos->history_ply].castling_rights;
    pos->halfmove = pos->history[pos->history_ply].halfmove;
    pos->fullmove = pos->history[pos->history_ply].fullmove;
    pos->hash_key = pos->history[pos->history_ply].hash_key;
}

// Make a Null Move (used in Null Move Pruning during search)
bool make_null_move(Position *pos) {
    // Save history state
    pos->history[pos->history_ply].move = 0;
    pos->history[pos->history_ply].en_passant = pos->en_passant;
    pos->history[pos->history_ply].castling_rights = pos->castling_rights;
    pos->history[pos->history_ply].halfmove = pos->halfmove;
    pos->history[pos->history_ply].fullmove = pos->fullmove;
    pos->history[pos->history_ply].captured_piece = NO_PIECE;
    pos->history[pos->history_ply].hash_key = pos->hash_key;

    // Toggle en-passant hash if active
    if (pos->en_passant != NO_SQ) {
        pos->hash_key ^= zobrist_en_passant[pos->en_passant];
    }

    // Clear en-passant square
    pos->en_passant = NO_SQ;

    // Switch side
    pos->side ^= 1;
    pos->hash_key ^= zobrist_side;

    if (pos->side == WHITE) {
        pos->fullmove++;
    }

    pos->history_ply++;
    return true;
}

// Unmake a Null Move
void unmake_null_move(Position *pos) {
    pos->history_ply--;

    pos->side ^= 1;

    pos->en_passant = pos->history[pos->history_ply].en_passant;
    pos->castling_rights = pos->history[pos->history_ply].castling_rights;
    pos->halfmove = pos->history[pos->history_ply].halfmove;
    pos->fullmove = pos->history[pos->history_ply].fullmove;
    pos->hash_key = pos->history[pos->history_ply].hash_key;
}

// Print the board to standard output
void print_board(const Position *pos) {
    /* Unicode chess symbols (U+2654-U+265F, "Miscellaneous Symbols") --
     * White's the hollow/outline glyphs, Black the solid ones, by
     * Unicode's own design, so no separate color-swap logic is needed
     * beyond picking the right table. Written as raw UTF-8 byte
     * sequences rather than \u escapes: this is a freestanding cross-
     * compiled build with no guarantee about the compiler's assumed
     * execution charset, and nothing else in this tree does i18n, so
     * there's no existing convention to lean on for encoding these
     * correctly -- explicit bytes sidestep the question entirely.
     * Replaces the old ASCII letters (PNBRQK/pnbrqk), which existed so
     * LugalGUI could scrape this exact text as position state -- no
     * longer needed now that nothing parses print_board()'s stdout as
     * data (user's own note, 2026-08-12). cprintf's own %s (kernel/
     * printk.c) just walks bytes to the console's putc with no encoding
     * awareness either way, so this is a drop-in swap, not a new
     * capability the rest of the pipe needs to understand. */
    static const char *white_glyphs[6] = {
        "\xE2\x99\x99", /* Pawn   U+2659 */
        "\xE2\x99\x98", /* Knight U+2658 */
        "\xE2\x99\x97", /* Bishop U+2657 */
        "\xE2\x99\x96", /* Rook   U+2656 */
        "\xE2\x99\x95", /* Queen  U+2655 */
        "\xE2\x99\x94", /* King   U+2654 */
    };
    static const char *black_glyphs[6] = {
        "\xE2\x99\x9F", /* Pawn   U+265F */
        "\xE2\x99\x9E", /* Knight U+265E */
        "\xE2\x99\x9D", /* Bishop U+265D */
        "\xE2\x99\x9C", /* Rook   U+265C */
        "\xE2\x99\x9B", /* Queen  U+265B */
        "\xE2\x99\x9A", /* King   U+265A */
    };
    printf("\n");
    for (int r = 7; r >= 0; r--) {
        printf("%d  ", r + 1);
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            int piece = pos->board[sq];
            if (piece == NO_PIECE) {
                printf(". ");
            } else {
                int color = get_bit(pos->color_bbs[WHITE], sq) ? WHITE : BLACK;
                printf("%s ", (color == WHITE ? white_glyphs : black_glyphs)[piece]);
            }
        }
        printf("\n");
    }
    printf("   a b c d e f g h\n\n");
}

// Print side to move, castling, en-passant, etc.
void print_position_info(const Position *pos) {
    printf("Side: %s\n", pos->side == WHITE ? "White" : "Black");
    printf("Castling: ");
    if (pos->castling_rights & WK_CASTLE) printf("K");
    if (pos->castling_rights & WQ_CASTLE) printf("Q");
    if (pos->castling_rights & BK_CASTLE) printf("k");
    if (pos->castling_rights & BQ_CASTLE) printf("q");
    if (pos->castling_rights == 0) printf("-");
    printf("\n");
    if (pos->en_passant != NO_SQ) {
        int r = pos->en_passant / 8;
        int f = pos->en_passant % 8;
        printf("En-passant: %c%d\n", 'a' + f, r + 1);
    } else {
        printf("En-passant: -\n");
    }
    printf("Halfmove: %d\n", pos->halfmove);
    printf("Fullmove: %d\n", pos->fullmove);
    printf("Hash: 0x%x%08x\n", (unsigned int)(pos->hash_key >> 32), (unsigned int)pos->hash_key);
}

// Append a decimal int to buf at *idx, advancing it (no sign handling --
// halfmove/fullmove clocks are never negative).
static void append_uint(char *buf, int *idx, int val) {
    char tmp[12];
    int n = 0;
    if (val == 0) {
        buf[(*idx)++] = '0';
        return;
    }
    while (val > 0) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (n > 0) {
        buf[(*idx)++] = tmp[--n];
    }
}

// Generate FEN string from position state
void generate_fen(const Position *pos, char *buf) {
    int char_idx = 0;
    // Ranks from 8 down to 1 (index 7 down to 0)
    for (int r = 7; r >= 0; r--) {
        int empty_cnt = 0;
        for (int f = 0; f < 8; f++) {
            int sq = f + r * 8;
            int piece = pos->board[sq];
            if (piece == NO_PIECE) {
                empty_cnt++;
            } else {
                if (empty_cnt > 0) {
                    buf[char_idx++] = '0' + empty_cnt;
                    empty_cnt = 0;
                }
                const char piece_chars[] = "PNBRQK";
                char c = piece_chars[piece];
                int color = (pos->color_bbs[WHITE] & (1ULL << sq)) ? WHITE : BLACK;
                buf[char_idx++] = (color == WHITE) ? c : (c + ('a' - 'A'));
            }
        }
        if (empty_cnt > 0) {
            buf[char_idx++] = '0' + empty_cnt;
        }
        if (r > 0) {
            buf[char_idx++] = '/';
        }
    }

    // Side to move
    buf[char_idx++] = ' ';
    buf[char_idx++] = (pos->side == WHITE) ? 'w' : 'b';

    // Castling rights
    buf[char_idx++] = ' ';
    int rights = pos->castling_rights;
    int initial_char_idx = char_idx;
    if (rights & WK_CASTLE) buf[char_idx++] = 'K';
    if (rights & WQ_CASTLE) buf[char_idx++] = 'Q';
    if (rights & BK_CASTLE) buf[char_idx++] = 'k';
    if (rights & BQ_CASTLE) buf[char_idx++] = 'q';
    if (char_idx == initial_char_idx) {
        buf[char_idx++] = '-';
    }

    // En passant target square
    buf[char_idx++] = ' ';
    if (pos->en_passant != NO_SQ) {
        buf[char_idx++] = 'a' + (pos->en_passant % 8);
        buf[char_idx++] = '1' + (pos->en_passant / 8);
    } else {
        buf[char_idx++] = '-';
    }

    // Halfmove clock and Fullmove number
    buf[char_idx++] = ' ';
    append_uint(buf, &char_idx, pos->halfmove);
    buf[char_idx++] = ' ';
    append_uint(buf, &char_idx, pos->fullmove);
    buf[char_idx] = '\0';
}
