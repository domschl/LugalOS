/*
 * Vendored from ~/gith/domschl/LugalChess (engine/src/search.c), H4,
 * plan/phase9_chess_computer.md. Changes from upstream:
 *   - stripped <sys/time.h>/<time.h>/<stdlib.h>/<string.h>/<stdio.h> and the
 *     LUGALCHESS_EMBEDDED pico-sdk includes; kernel/time.h stands in.
 *   - get_time_ms()'s embedded branch calls kernel/time.h's time_get_ms()
 *     instead of the Pico SDK's time_us_32().
 *   - get_book_move()'s embedded-only extra entropy source is
 *     time_get_us() instead of time_us_32().
 *   - the three UCI "info depth ..." lines' nps figure is now computed as
 *     plain integer division (nodes*1000/ms) instead of a double, and
 *     printed with %d instead of %.0f -- cprintf has no float support at
 *     all (chess_platform.h). %d/%ld/%s/%c elsewhere are unchanged: cprintf
 *     already reads %d as a `long` regardless of the 'l', so nodes_searched
 *     (itself declared `long`) needed no cast.
 * search_progress_callback()/search_poll_stop_callback() are declared
 * `extern` and called here but never defined here -- upstream defines them
 * in console.c (TM1638/ST7735 UI feedback, or a UCI/host stub); LugalOS
 * defines its own in user/chess/src/chess_ui.c. This is the exact extension
 * point that made dropping console.c/uci.c possible without touching a
 * single line of search logic.
 */

#include "search.h"
#include "defs.h"
#include "bitboard.h"
#include "movegen.h"
#include "move.h"
#include "evaluation.h"
#include "tt.h"
#include "kernel/time.h"

// Global search variables
int max_search_depth = 64;
long max_search_time_ms = -1;
long start_search_time_ms = 0;
bool stop_search = false;
long nodes_searched = 0;

// Move ordering heuristic tables
static int history_table[6][64]; // [moved_piece][destination]
static Move killer_moves[2][MAX_PLYS]; // [killer_index][ply]

// Approximate piece values for MVV-LVA sorting
static const int sorting_values[6] = { 100, 300, 300, 500, 900, 10000 };

typedef struct {
    const char *name;
    const char *moves;
} BookEntry;

#if defined(__GNUC__) || defined(__clang__)
#define ALIGN4 __attribute__((aligned(4)))
#else
#define ALIGN4
#endif

static const char ALIGN4 book_move_0[]  = "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7";
static const char ALIGN4 book_move_1[]  = "e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d4";
static const char ALIGN4 book_move_2[]  = "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6";
static const char ALIGN4 book_move_3[]  = "e2e4 c7c5 g1f3 e7e6 d2d4 c5d4 f3d4 b8c6 b1c3 d7d6";
static const char ALIGN4 book_move_4[]  = "e2e4 c7c5 c2c3 d7d5 e4d5 d8d5 d2d4 g8f6";
static const char ALIGN4 book_move_5[]  = "e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7";
static const char ALIGN4 book_move_6[]  = "e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5";
static const char ALIGN4 book_move_7[]  = "e2e4 d7d6 d2d4 g8f6 b1c3 g7g6 f2f4 f8g7";
static const char ALIGN4 book_move_8[]  = "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7";
static const char ALIGN4 book_move_9[]  = "d2d4 d7d5 c2c4 c7c6 g1f3 g8f6 b1c3 e7e6";
static const char ALIGN4 book_move_10[] = "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e4e5 d6d6";
static const char ALIGN4 book_move_11[] = "d2d4 g8f6 c2c4 e7e6 g1f3 b7b6 g2g3 c8b7";
static const char ALIGN4 book_move_12[] = "d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 e2e3 e1g1";
static const char ALIGN4 book_move_13[] = "g1f3 d7d5 g2g3 g8f6 f1g2 c7c6 e1g1 c8f5";
static const char ALIGN4 book_move_14[] = "c2c4 e7e5 b1c3 g8f6 g1f3 b8c6 g2g3 f8b4";
static const char ALIGN4 book_move_15[] = "f2f4 d7d5 g1f3 g8f6 e2e3 c7c5 f1e2 b8c6";

static const BookEntry book_entries[] = {
    // 1. e4 lines
    { "Ruy Lopez",        book_move_0 },
    { "Italian",          book_move_1 },
    { "Sicil Najdorf",    book_move_2 },
    { "Sicil Taimanov",   book_move_3 },
    { "Sicil Alapin",     book_move_4 },
    { "French Class.",   book_move_5 },
    { "Caro-Kann",        book_move_6 },
    { "Pirc Def.",        book_move_7 },
    // 2. d4 lines
    { "QGD",              book_move_8 },
    { "Slav Def.",        book_move_9 },
    { "King's Indian",   book_move_10 },
    { "Queen's Indian",  book_move_11 },
    { "Nimzo-Indian",    book_move_12 },
    // 3. Flank openings
    { "King's Ind Atk",   book_move_13 },
    { "English Op.",     book_move_14 },
    { "Bird's Op.",      book_move_15 }
};

#define MAX_SEARCH_PLYS 64

static MoveList search_pv_movelists[MAX_SEARCH_PLYS];
static MoveList search_q_movelists[MAX_SEARCH_PLYS];
static int sort_scores[MAX_MOVES];

__attribute__((noinline))
static bool match_book_prefix(const char *line_ptr, const char *history_ptr, int history_len) {
    if (!line_ptr || !history_ptr) return false;
    volatile const uint8_t *line = (volatile const uint8_t *)line_ptr;
    volatile const uint8_t *history = (volatile const uint8_t *)history_ptr;
    for (int i = 0; i < history_len; i++) {
        uint8_t c1 = line[i];
        uint8_t c2 = history[i];
        if (c1 == 0 || c1 != c2) return false;
    }
    uint8_t next_ch = line[history_len];
    return (next_ch == ' ' || next_ch == 0);
}

static int build_history_string(const Position *pos, char *buf, size_t buf_size) {
    if (!pos || buf_size == 0) return 0;
    buf[0] = '\0';
    int offset = 0;
    for (int i = 0; i < pos->history_ply; i++) {
        Move m = pos->history[i].move;
        int from = MOVE_FROM(m);
        int to = MOVE_TO(m);
        if (offset > 0) {
            if ((size_t)offset + 1 >= buf_size) break;
            buf[offset++] = ' ';
        }
        if ((size_t)offset + 5 >= buf_size) break;
        buf[offset++] = 'a' + (from % 8);
        buf[offset++] = '1' + (from / 8);
        buf[offset++] = 'a' + (to % 8);
        buf[offset++] = '1' + (to / 8);
        if (move_is_promo(m)) {
            int promo = move_promo_piece(m);
            const char promo_chars[] = "pnbrqk";
            if ((size_t)offset + 1 >= buf_size) break;
            buf[offset++] = promo_chars[promo];
        }
        buf[offset] = '\0';
    }
    return offset;
}

const char *get_book_line_name(const Position *pos) {
    if (pos == NULL || pos->history_ply == 0) {
        return NULL;
    }

    static char history_str[512];
    int history_len = build_history_string(pos, history_str, sizeof(history_str));
    if (history_len == 0) return NULL;

    int book_size = sizeof(book_entries) / sizeof(book_entries[0]);
    for (int i = 0; i < book_size; i++) {
        if (match_book_prefix(book_entries[i].moves, history_str, history_len)) {
            return book_entries[i].name;
        }
    }

    return NULL;
}

__attribute__((noinline))
static bool move_str_equals(const char *s1, const char *s2) {
    for (int k = 0; k < 6; k++) {
        if (s1[k] != s2[k]) return false;
        if (s1[k] == '\0') break;
    }
    return true;
}

__attribute__((noinline))
static void move_str_copy(char *dest, const char *src) {
    for (int k = 0; k < 6; k++) {
        dest[k] = src[k];
        if (src[k] == '\0') break;
    }
    dest[7] = '\0';
}

static Move get_book_move(Position *pos) {
    static char history_str[512];
    int history_len = build_history_string(pos, history_str, sizeof(history_str));

    static char next_move_strs[64][8];
    int unique_count = 0;

    int book_size = sizeof(book_entries) / sizeof(book_entries[0]);
    for (int i = 0; i < book_size && unique_count < 64; i++) {
        const char *line = book_entries[i].moves;
        if (history_len == 0) {
            char next_m[8];
            int c = 0;
            while (line[c] != ' ' && line[c] != '\0' && c < 5) {
                next_m[c] = line[c];
                c++;
            }
            next_m[c] = '\0';

            if (c >= 4) {
                bool is_unique = true;
                for (int j = 0; j < unique_count; j++) {
                    if (move_str_equals(next_move_strs[j], next_m)) {
                        is_unique = false;
                        break;
                    }
                }
                if (is_unique) {
                    move_str_copy(next_move_strs[unique_count], next_m);
                    unique_count++;
                }
            }
        } else {
            if (match_book_prefix(line, history_str, history_len)) {
                volatile const uint8_t *line_bytes = (volatile const uint8_t *)line;
                if (line_bytes[history_len] == ' ') {
                    volatile const uint8_t *p = line_bytes + history_len + 1;
                    char next_m[8];
                    int c = 0;
                    while (p[c] != ' ' && p[c] != 0 && c < 5) {
                        next_m[c] = (char)p[c];
                        c++;
                    }
                    next_m[c] = '\0';

                    if (c >= 4) {
                        bool is_unique = true;
                        for (int j = 0; j < unique_count; j++) {
                            if (move_str_equals(next_move_strs[j], next_m)) {
                                is_unique = false;
                                break;
                            }
                        }
                        if (is_unique) {
                            move_str_copy(next_move_strs[unique_count], next_m);
                            unique_count++;
                        }
                    }
                }
            }
        }
    }

    if (unique_count == 0) return 0;

    uint32_t seed = (uint32_t)start_search_time_ms;
#if defined(LUGALCHESS_EMBEDDED)
    seed ^= (uint32_t)time_get_us();
#endif
    int choice = (int)(seed % (uint32_t)unique_count);
    const char *chosen = next_move_strs[choice];

    if (chosen[0] < 'a' || chosen[0] > 'h' || chosen[1] < '1' || chosen[1] > '8' ||
        chosen[2] < 'a' || chosen[2] > 'h' || chosen[3] < '1' || chosen[3] > '8') {
        return 0;
    }

    int target_from = (chosen[0] - 'a') + (chosen[1] - '1') * 8;
    int target_to = (chosen[2] - 'a') + (chosen[3] - '1') * 8;
    int target_promo = NO_PIECE;
    if (chosen[4] != '\0') {
        switch (chosen[4]) {
            case 'q': target_promo = QUEEN; break;
            case 'r': target_promo = ROOK; break;
            case 'b': target_promo = BISHOP; break;
            case 'n': target_promo = KNIGHT; break;
        }
    }

    MoveList *list = &search_pv_movelists[0];
    generate_moves(pos, list);
    for (int i = 0; i < list->count; i++) {
        Move m = list->moves[i];
        if (MOVE_FROM(m) == target_from && MOVE_TO(m) == target_to) {
            if (target_promo != NO_PIECE) {
                if (move_is_promo(m) && move_promo_piece(m) == target_promo) {
                    if (make_move(pos, m)) {
                        unmake_move(pos);
                        return m;
                    }
                }
            } else {
                if (!move_is_promo(m)) {
                    if (make_move(pos, m)) {
                        unmake_move(pos);
                        return m;
                    }
                }
            }
        }
    }

    return 0;
}

// Time check helper
#if defined(LUGALCHESS_EMBEDDED)
static long get_time_ms(void) {
    return (long)time_get_ms();
}
#else
static long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
#endif

static void check_up_time(void) {
    if ((nodes_searched & 2047) == 0) {
        extern void search_poll_stop_callback(void);
        search_poll_stop_callback();
        if (max_search_time_ms != -1) {
            if (get_time_ms() - start_search_time_ms >= max_search_time_ms) {
                stop_search = true;
            }
        }
    }
}


// MVV-LVA capture scoring helper
static int score_capture(const Position *pos, Move move) {
    int from = MOVE_FROM(move);
    int to = MOVE_TO(move);
    int attacker = pos->board[from];
    int victim;

    if (MOVE_FLAGS(move) == MOVE_FLAG_EN_PASSANT) {
        victim = PAWN;
    } else {
        victim = pos->board[to];
    }

    // Most valuable victim, least valuable attacker
    return 1000000 + (sorting_values[victim] * 10) - sorting_values[attacker];
}

// Score a move for ordering
static int score_move(const Position *pos, Move move, Move tt_move, int ply) {
    if (move == tt_move) {
        return 2000000; // Search best hash move first
    }

    if (move_is_capture(move)) {
        return score_capture(pos, move);
    }

    if (move_is_promo(move)) {
        int promo = move_promo_piece(move);
        return 900000 + sorting_values[promo];
    }

    // Killer moves
    if (killer_moves[0][ply] == move) return 800000;
    if (killer_moves[1][ply] == move) return 700000;

    // History heuristic
    int piece = pos->board[MOVE_FROM(move)];
    int to = MOVE_TO(move);
    return history_table[piece][to];
}

// Insertion sort for move list (fast for small arrays)
static void sort_moves(const Position *pos, MoveList *list, Move tt_move, int ply) {
    if (list->count <= 1) return;

    int safe_ply = (ply >= 0 && ply < MAX_PLYS) ? ply : 0;
    for (int i = 0; i < list->count; i++) {
        sort_scores[i] = score_move(pos, list->moves[i], tt_move, safe_ply);
    }

    for (int i = 1; i < list->count; i++) {
        Move temp_m = list->moves[i];
        int temp_s = sort_scores[i];
        int j = i - 1;
        while (j >= 0 && sort_scores[j] < temp_s) {
            list->moves[j + 1] = list->moves[j];
            sort_scores[j + 1] = sort_scores[j];
            j--;
        }
        list->moves[j + 1] = temp_m;
        sort_scores[j + 1] = temp_s;
    }
}

// Quiescence Search (tactical search only)
int quiescence(Position *pos, int ply, int alpha, int beta) {
    nodes_searched++;
    check_up_time();
    if (stop_search) return 0;

    // Safety guard to prevent stack overflow/out-of-bounds history plies
    if (ply >= MAX_SEARCH_PLYS - 1 || pos->history_ply >= MAX_PLYS - 1) {
        return evaluate(pos);
    }

    // Standing pat score (assume we can stand pat and do no more moves)
    int stand_pat = evaluate(pos);
    if (stand_pat >= beta) {
        return beta;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    int safe_ply = ply % MAX_SEARCH_PLYS;
    MoveList *list = &search_q_movelists[safe_ply];
    generate_captures(pos, list);
    sort_moves(pos, list, 0, safe_ply);

    for (int i = 0; i < list->count; i++) {
        if (!make_move(pos, list->moves[i])) {
            continue;
        }
        int score = -quiescence(pos, ply + 1, -beta, -alpha);
        unmake_move(pos);

        if (stop_search) return 0;

        if (score >= beta) {
            return beta;
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    return alpha;
}

// Principal Variation Search (PVS) with Pruning
int pv_search(Position *pos, int depth, int ply, int alpha, int beta, bool null_move_allowed) {
    nodes_searched++;
    check_up_time();
    if (stop_search) return 0;

    // Safety guard to prevent stack overflow/out-of-bounds history plies
    if (ply >= MAX_SEARCH_PLYS - 1 || pos->history_ply >= MAX_PLYS - 1) {
        return evaluate(pos);
    }

    // Draw by repetition or 50-move rule
    if (ply > 0) {
        // Simple repetition check (check if current position hash matches any parent hash)
        int start_ply = pos->history_ply - 2;
        int end_ply = pos->history_ply - pos->halfmove;
        if (end_ply < 0) end_ply = 0;
        for (int i = start_ply; i >= end_ply; i -= 2) {
            if (i >= 0 && pos->history[i].hash_key == pos->hash_key) {
                return 0; // Draw score
            }
        }
        if (pos->halfmove >= 100) return 0; // 50-move rule draw
    }

    // Mate distance pruning
    int mate_val = INFINITY_VALUE - ply;
    if (alpha >= mate_val) return alpha;
    if (beta <= -mate_val) return beta;

    // TT Lookup (only apply score cutoffs at non-root plies)
    Move tt_move = 0;
    int tt_score = 0;
    if (read_tt(pos->hash_key, depth, ply, alpha, beta, &tt_score, &tt_move)) {
        if (ply > 0) {
            return tt_score;
        }
    }

    int in_check = is_square_attacked(pos, get_lsb(pos->piece_bbs[KING] & pos->color_bbs[pos->side]), pos->side ^ 1);
    if (in_check) {
        depth++; // Check extension: extend search when in check
    }

    // Leaf nodes
    if (depth <= 0) {
        return quiescence(pos, ply + 1, alpha, beta);
    }

    // Null Move Pruning (NMP)
    int total_pieces = count_bits(pos->color_bbs[WHITE] | pos->color_bbs[BLACK]);
    if (null_move_allowed && !in_check && depth >= 3 && total_pieces > 4) {
        // Verify we have major pieces left (avoid zugzwang)
        uint64_t majors = pos->piece_bbs[KNIGHT] | pos->piece_bbs[BISHOP] | pos->piece_bbs[ROOK] | pos->piece_bbs[QUEEN];
        if (majors & pos->color_bbs[pos->side]) {
            make_null_move(pos);
            int score = -pv_search(pos, depth - 1 - 2, ply + 1, -beta, -beta + 1, false);
            unmake_null_move(pos);

            if (stop_search) return 0;
            if (score >= beta) {
                return beta; // Prune!
            }
        }
    }

    // Move generation
    int safe_ply = ply % MAX_SEARCH_PLYS;
    MoveList *list = &search_pv_movelists[safe_ply];
    generate_moves(pos, list);
    sort_moves(pos, list, tt_move, safe_ply);

    int legal_moves = 0;
    int best_score = -INFINITY_VALUE;
    Move best_move = 0;
    uint8_t tt_flag = TT_ALPHA;

    for (int i = 0; i < list->count; i++) {
        Move move = list->moves[i];
        if (!make_move(pos, move)) {
            continue;
        }
        legal_moves++;

        int score;
        if (legal_moves == 1) {
            // PV move: search with full window
            score = -pv_search(pos, depth - 1, ply + 1, -beta, -alpha, true);
        } else {
            // Quiet moves LMR (Late Move Reduction)
            if (depth >= 3 && !in_check && !move_is_capture(move) && !move_is_promo(move) && legal_moves > 4 && total_pieces > 4) {
                // Reduce depth
                score = -pv_search(pos, depth - 2, ply + 1, -alpha - 1, -alpha, true);
                if (score > alpha) {
                    // Re-search at full depth with null window
                    score = -pv_search(pos, depth - 1, ply + 1, -alpha - 1, -alpha, true);
                }
            } else {
                // Regular null window search
                score = -pv_search(pos, depth - 1, ply + 1, -alpha - 1, -alpha, true);
            }

            // Re-search with full window if fail-high
            if (score > alpha && score < beta) {
                score = -pv_search(pos, depth - 1, ply + 1, -beta, -alpha, true);
            }
        }

        unmake_move(pos);
        if (stop_search) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = move;
            if (ply == 0) {
                extern void search_progress_callback(Move move, int score, int depth);
                search_progress_callback(best_move, best_score, depth);
            }
        }

        if (score >= beta) {
            // Store killer moves and history heuristic for quiet moves
            if (!move_is_capture(move) && !move_is_promo(move)) {
                killer_moves[1][ply] = killer_moves[0][ply];
                killer_moves[0][ply] = move;

                int piece = pos->board[MOVE_FROM(move)];
                int to = MOVE_TO(move);
                history_table[piece][to] += depth * depth;
                if (history_table[piece][to] > 400000) {
                    // Scale down to prevent overflow
                    for (int p = 0; p < 6; p++) {
                        for (int sq = 0; sq < 64; sq++) {
                            history_table[p][sq] /= 2;
                        }
                    }
                }
            }
            write_tt(pos->hash_key, move, score_to_tt(beta, ply), depth, TT_BETA);
            return beta;
        }

        if (score > alpha) {
            alpha = score;
            tt_flag = TT_EXACT;
        }
    }

    // Checkmate/Stalemate detection
    if (legal_moves == 0) {
        if (in_check) {
            return -INFINITY_VALUE + ply; // Mate in ply
        } else {
            return 0; // Stalemate
        }
    }

    write_tt(pos->hash_key, best_move, score_to_tt(best_score, ply), depth, tt_flag);
    return best_score;
}

// Iterative deepening entry point
void search_position(Position *pos, int depth, int time_limit_ms) {
    if (!is_position_valid(pos)) {
        printf("info string Error: Invalid position\n");
        fflush(stdout);
        return;
    }

    // Check if there is an opening book move
    Move book_move = get_book_move(pos);
    if (book_move != 0) {
        extern void search_progress_callback(Move move, int score, int depth);
        search_progress_callback(book_move, 0, 1);

        int from = MOVE_FROM(book_move);
        int to = MOVE_TO(book_move);
        printf("bestmove %c%d%c%d", 'a' + (from % 8), (from / 8) + 1, 'a' + (to % 8), (to / 8) + 1);
        if (move_is_promo(book_move)) {
            int promo = move_promo_piece(book_move);
            const char promo_chars[] = "pnbrqk";
            printf("%c", promo_chars[promo]);
        }
        printf("\n");
        fflush(stdout);

        // Store in TT so that caller (e.g. make_engine_move) can retrieve it
        write_tt(pos->hash_key, book_move, 0, depth, TT_EXACT);
        return;
    }

    // Dynamically adjust search depth in endgame when there are fewer pieces
    int piece_count = count_bits(pos->color_bbs[WHITE] | pos->color_bbs[BLACK]);
    if (piece_count <= 6) {
        depth += 6;
    } else if (piece_count <= 10) {
        depth += 4;
    } else if (piece_count <= 16) {
        depth += 2;
    }

    start_search_time_ms = get_time_ms();
    srand((unsigned int)start_search_time_ms);
    max_search_time_ms = time_limit_ms;
    stop_search = false;
    nodes_searched = 0;
    increment_tt_age();

    // Reset move ordering heuristics
    memset(killer_moves, 0, sizeof(killer_moves));

    static MoveList root_list;
    generate_moves(pos, &root_list);
    Move fallback_move = 0;
    for (int i = 0; i < root_list.count; i++) {
        if (make_move(pos, root_list.moves[i])) {
            unmake_move(pos);
            fallback_move = root_list.moves[i];
            break;
        }
    }

    Move completed_best_move = fallback_move;
    int completed_best_score = -INFINITY_VALUE;

    long prev_iter_time_ms = 0;

    // Iterative Deepening
    for (int d = 1; d <= depth; d++) {
        long iter_start_time_ms = get_time_ms();

        extern void search_progress_callback(Move move, int score, int depth);
        search_progress_callback(0, 0, d);
        int score = pv_search(pos, d, 0, -INFINITY_VALUE, INFINITY_VALUE, true);

        // If search was interrupted mid-iteration by stop/timer, discard incomplete depth results!
        if (stop_search) {
            break;
        }

        Move temp_move = 0;
        int temp_score = 0;
        if (read_tt(pos->hash_key, d, 0, -INFINITY_VALUE, INFINITY_VALUE, &temp_score, &temp_move) && temp_move != 0) {
            completed_best_move = temp_move;
        } else {
            Move probe_move = 0;
            if (probe_tt_entry(pos->hash_key, NULL, &probe_move) && probe_move != 0) {
                completed_best_move = probe_move;
            }
        }
        completed_best_score = score;

        // Send progress callback with current best move and score for this completed depth
        search_progress_callback(completed_best_move, completed_best_score, d);

        long now_ms = get_time_ms();
        long time_spent = now_ms - start_search_time_ms;
        long last_iter_time_ms = now_ms - iter_start_time_ms;

        long nps = time_spent > 0 ? (nodes_searched * 1000) / time_spent : 0;

        // Print UCI info block with proper mate score formatting
        if (completed_best_score >= MATE_VALUE - 1000) {
            int mate_plies = INFINITY_VALUE - completed_best_score;
            int mate_moves = (mate_plies + 1) / 2;
            printf("info depth %d score mate %d nodes %ld nps %ld time %ld pv ",
                   d, mate_moves, nodes_searched, nps, time_spent);
        } else if (completed_best_score <= -MATE_VALUE + 1000) {
            int mate_plies = INFINITY_VALUE + completed_best_score;
            int mate_moves = (mate_plies + 1) / 2;
            printf("info depth %d score mate -%d nodes %ld nps %ld time %ld pv ",
                   d, mate_moves, nodes_searched, nps, time_spent);
        } else {
            printf("info depth %d score cp %d nodes %ld nps %ld time %ld pv ",
                   d, completed_best_score, nodes_searched, nps, time_spent);
        }

        // Print Principal Variation (PV) path
        static Position temp_pos;
        temp_pos = *pos;
        int pv_ply = 0;
        Move pv_move = completed_best_move;

        while (pv_move != 0 && pv_ply < d) {
            int from = MOVE_FROM(pv_move);
            int to = MOVE_TO(pv_move);
            printf("%c%d%c%d", 'a' + (from % 8), (from / 8) + 1, 'a' + (to % 8), (to / 8) + 1);
            if (move_is_promo(pv_move)) {
                int promo = move_promo_piece(pv_move);
                const char promo_chars[] = "pnbrqk";
                printf("%c", promo_chars[promo]);
            }
            printf(" ");

            if (!make_move(&temp_pos, pv_move)) break;
            pv_ply++;

            // Look up next move in PV
            int dummy_score;
            read_tt(temp_pos.hash_key, d - pv_ply, pv_ply, -INFINITY_VALUE, INFINITY_VALUE, &dummy_score, &pv_move);
        }
        printf("\n");
        fflush(stdout);

        // If mate is detected, only stop if search depth 'd' is sufficient to guarantee the mate distance,
        // or if it's mate in 1 move (M1). Otherwise, continue searching to find shorter mates!
        int mate_plies = 0;
        if (completed_best_score >= MATE_VALUE - 1000) {
            mate_plies = INFINITY_VALUE - completed_best_score;
        } else if (completed_best_score <= -MATE_VALUE + 1000) {
            mate_plies = INFINITY_VALUE + completed_best_score;
        }
        if (mate_plies > 0 && d >= mate_plies) {
            break;
        }

        // Dynamic time-based cutoff:
        // Estimate time required for the next depth iteration (d + 1) based on effective branching factor b
        if (max_search_time_ms != -1) {
            double b = 3.5; // default branching factor estimate
            if (d >= 2 && prev_iter_time_ms > 0) {
                b = (double)last_iter_time_ms / (double)prev_iter_time_ms;
                if (b < 2.5) b = 2.5;
                if (b > 5.0) b = 5.0;
            }
            prev_iter_time_ms = last_iter_time_ms;

            double est_next_iter_ms = b * (last_iter_time_ms > 0 ? last_iter_time_ms : 5);

            // If time spent plus half of estimated next iteration time exceeds allotted time, stop before launching d+1
            if (time_spent + (long)(est_next_iter_ms / 2.0) >= max_search_time_ms) {
                break;
            }
        }
    }

    // Output UCI bestmove from last fully completed depth
    int from = MOVE_FROM(completed_best_move);
    int to = MOVE_TO(completed_best_move);
    printf("bestmove %c%d%c%d", 'a' + (from % 8), (from / 8) + 1, 'a' + (to % 8), (to / 8) + 1);
    if (move_is_promo(completed_best_move)) {
        int promo = move_promo_piece(completed_best_move);
        const char promo_chars[] = "pnbrqk";
        printf("%c", promo_chars[promo]);
    }
    printf("\n");
    fflush(stdout);
    stop_search = false;
}
