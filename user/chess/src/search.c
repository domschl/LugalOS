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
 *   - the quiescence pool is CaptureList-wide, not MoveList-wide (see
 *     movegen.h and MAX_CAPTURES in defs.h), and sort_moves() takes
 *     (Move *moves, int count) so it can serve both list types.
 *   - MAX_SEARCH_PLYS is 32 on RP2350 and 64 everywhere else, where it was
 *     unconditionally 64 -- see that constant's own comment below for the
 *     measurement behind the number.
 *   - the PV-printing block in search_position() had its own permanent
 *     `static Position temp_pos` (~8 KB, almost all of it history[MAX_PLYS]).
 *     It now borrows chess_ui.c's already-existing on-demand scratch via
 *     chess_scratch_position(), declared extern at the call site the same way
 *     search_progress_callback()/search_poll_stop_callback() already are --
 *     see §1.3 of plan/phase15_memory_reclamation.md, and that function's own
 *     comment in chess_ui.c for why sharing one scratch between the engine
 *     and the UI cannot race.
 *   - search_pv_movelists/search_q_movelists (~65 KB combined) were plain
 *     `static` arrays until J0 (plan/phase10_chess_completion.md): permanent
 *     .bss on every board with chess enabled (the default), whether or not
 *     it was ever played -- dropped RP2350's managed heap from 78 to 48
 *     pages (H4's own finding, phase9). search_pools_init() now allocates
 *     them from palloc_pages() on first use instead, matching cc/ed's own
 *     on-demand-pool precedent (phase8) rather than reinventing a different
 *     lifetime policy. Every existing `search_pv_movelists[i]` /
 *     `search_q_movelists[i]` access below is unchanged -- pointer indexing
 *     reads identically to array indexing in C, so this is a declaration-only
 *     change plus the init call.
 */

#include "search.h"
#include "defs.h"
#include "bitboard.h"
#include "movegen.h"
#include "move.h"
#include "evaluation.h"
#include "tt.h"
#include "kernel/time.h"
#include "kernel/palloc.h"

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

/* How many plies of move-list pool search_pools_init() reserves, and, via
 * the guards in pv_search()/quiescence() below, the hard ceiling on how far
 * ahead a search may look at all.
 *
 * Board-scoped (§2.1, plan/phase15_memory_reclamation.md): the pools are
 * `2 * MAX_SEARCH_PLYS * sizeof(MoveList)` bytes taken from the page
 * allocator, which at 64 is 66,048 bytes -- 17 of RP2350's pages, by a wide
 * margin the largest single thing chess asks the heap for. 32 makes it 9.
 * Guarded on CONFIG_BOARD_RP2350 rather than LUGALCHESS_EMBEDDED, which
 * defs.h's MAX_PLYS uses: that macro is true on the QEMU targets too (see
 * version.h -- it means "bare metal", not "small"), and this split is about
 * one board having 512 KB of RAM while the others have 128 MB. Same axis,
 * and the same spelling, as user/lisp/lisp.c's own pool constants.
 *
 * ## Why 32 is not a functional regression here
 *
 * This is a ceiling on *total* lookahead, iterative-deepening depth plus
 * whatever quiescence extends on top, and hitting it is graceful -- the
 * guards return a static evaluation rather than recursing further, which is
 * what they already did at 64.
 *
 * Measured before choosing it, rather than argued from the constant's size.
 * A temporary counter on the guard itself (added and fully removed) was run
 * against the 32-ply value on QEMU, which at ~19.8M nodes/s searches vastly
 * deeper in a given wall-clock budget than RP2350's ~44K -- so it is a
 * strictly harder test than this board can ever face:
 *
 *     Kiwipete (tactical, 48 legal moves)   depth 7    guard hits: 0
 *     promotion tactics                     depth 9    guard hits: 0
 *     rook-and-pawn endgame                 depth 11   guard hits: 0
 *     bare K+P endgame (deepest by far)     depth 15   guard hits: 0
 *
 * Separate instrumentation put the peak ply reached at roughly d+3..d+4
 * (quiescence's extension beyond the iterative-deepening depth), so a
 * 31-ply ceiling corresponds to an iterative-deepening depth near 27 --
 * about twice the deepest figure above, on hardware some 450x slower than
 * the machine that produced it. */
#if defined(CONFIG_BOARD_RP2350)
#define MAX_SEARCH_PLYS 32
#else
#define MAX_SEARCH_PLYS 64
#endif

/* Allocated on demand by search_pools_init() (J0) rather than reserved
 * statically -- see the file header comment above. NULL until then; every
 * caller of search_position()/get_book_move() is required to have already
 * called search_pools_init() successfully (chess_ui.c's chess_ensure_init()
 * does, before any search can run), so no per-access NULL check is added to
 * the hot recursive path below. */
static MoveList *search_pv_movelists = NULL;
static CaptureList *search_q_movelists = NULL;
static uint32_t search_pools_pages = 0;
static int sort_scores[MAX_MOVES];

bool search_pools_init(void) {
    if (search_pv_movelists != NULL) {
        return true; /* already allocated -- idempotent, like init_tt(). */
    }

    /* Two different element widths since §2.1 (see CaptureList), so the two
     * halves are sized separately and the split is a byte offset rather than
     * the pointer arithmetic this used before. sizeof(MoveList) is a multiple
     * of 4 and both structs align to 4 (their widest member is an int), so
     * the second half lands aligned with no padding step needed -- asserted
     * rather than assumed, since it is the kind of thing that silently stops
     * being true if either struct gains a wider field. */
    _Static_assert(sizeof(MoveList) % _Alignof(CaptureList) == 0,
                   "the capture pool would start misaligned inside the shared block");

    uint32_t pv_bytes = (uint32_t)(MAX_SEARCH_PLYS * sizeof(MoveList));
    uint32_t q_bytes  = (uint32_t)(MAX_SEARCH_PLYS * sizeof(CaptureList));
    uint32_t bytes = pv_bytes + q_bytes;
    search_pools_pages = (bytes + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    uint8_t *block = (uint8_t *)palloc_pages(search_pools_pages);
    if (block == NULL) {
        search_pools_pages = 0;
        return false;
    }

    /* One allocation, not two -- halves the page-rounding waste a pair of
     * palloc_pages() calls would each pay separately (chibicc's pools.c
     * arena makes the same call for the same reason). */
    search_pv_movelists = (MoveList *)block;
    search_q_movelists = (CaptureList *)(block + pv_bytes);
    return true;
}

/* Mirrors tt.c's free_tt() -- added when chess_ui.c grew a real
 * acquire/release lifecycle (revising J0's original "never freed for the
 * process lifetime" choice once J1 gave chess a session boundary to free
 * at, plan/phase10_chess_completion.md). Safe to call whether or not
 * search_pools_init() ever ran. `search_pools_pages` is stored rather than
 * recomputed at free time -- same reason tt.c stores `tt_pages` instead of
 * recomputing it -- so init and free can never compute a different page
 * count even if MAX_SEARCH_PLYS or sizeof(MoveList) ever change. */
void search_pools_free(void) {
    if (search_pv_movelists == NULL) {
        return;
    }
    palloc_free(search_pv_movelists, search_pools_pages);
    search_pv_movelists = NULL;
    search_q_movelists = NULL;
    search_pools_pages = 0;
}

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

/* Insertion sort for move list (fast for small arrays).
 *
 * Takes the array and its length rather than a list struct: since §2.1
 * (plan/phase15_memory_reclamation.md) there are two list types, MoveList and
 * the narrower CaptureList, and this function only ever reads and permutes
 * `moves[]` -- it never needs to know which struct the array came out of, and
 * it never changes the count. */
static void sort_moves(const Position *pos, Move *moves, int count,
                       Move tt_move, int ply) {
    if (count <= 1) return;

    int safe_ply = (ply >= 0 && ply < MAX_PLYS) ? ply : 0;
    for (int i = 0; i < count; i++) {
        sort_scores[i] = score_move(pos, moves[i], tt_move, safe_ply);
    }

    for (int i = 1; i < count; i++) {
        Move temp_m = moves[i];
        int temp_s = sort_scores[i];
        int j = i - 1;
        while (j >= 0 && sort_scores[j] < temp_s) {
            moves[j + 1] = moves[j];
            sort_scores[j + 1] = sort_scores[j];
            j--;
        }
        moves[j + 1] = temp_m;
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
    CaptureList *list = &search_q_movelists[safe_ply];
    generate_captures(pos, list);
    sort_moves(pos, list->moves, list->count, 0, safe_ply);

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
    sort_moves(pos, list->moves, list->count, tt_move, safe_ply);

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

        /* Found live while smoke-testing J0 (plan/phase10_chess_completion.md),
         * not caused by it: on a 32-bit `long` target (rv32/RP2350's ILP32,
         * not rv64's LP64), `nodes_searched * 1000` overflows a signed 32-bit
         * int once nodes_searched passes ~2.1M -- reached mid-search on real
         * hardware isn't a risk (H4 measured ~44K nodes/s there), but QEMU's
         * host-speed emulation (H4's own ~19.8M nodes/s figure) crosses it
         * within a couple of iterative-deepening iterations, and did, with
         * UBSAN_PANIC halting the system. The 64-bit intermediate avoids it;
         * the result narrows back to `long` for the unchanged `%ld` below. */
        long nps = time_spent > 0 ? (long)(((int64_t)nodes_searched * 1000) / time_spent) : 0;

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
        extern Position *chess_scratch_position(void);
        Position *temp_pos = chess_scratch_position();
        int pv_ply = 0;
        Move pv_move = completed_best_move;

        /* Non-NULL for every reachable caller (see chess_scratch_position()'s
         * own comment), so this guard should never fire -- it exists because
         * the failure it prevents is a null dereference partway through a
         * search on real hardware, which is worth one branch per depth. */
        if (temp_pos != NULL) {
            *temp_pos = *pos;

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

                if (!make_move(temp_pos, pv_move)) break;
                pv_ply++;

                // Look up next move in PV
                int dummy_score;
                read_tt(temp_pos->hash_key, d - pv_ply, pv_ply, -INFINITY_VALUE, INFINITY_VALUE, &dummy_score, &pv_move);
            }
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
        //
        // Found and fixed 2026-08-12 (plan/phase10_chess_completion.md J1):
        // this was `double` arithmetic until now, which built and ran fine
        // on every target this engine had actually been exercised on
        // (RV32 QEMU + real RP2350 hardware, both ilp32/soft-float -- no
        // hardware FP registers exist in that ABI, so GCC lowers `double`
        // to libgcc calls with no FPU involved at all) but crashed
        // immediately -- Illegal Instruction trapping the `fsd` register
        // spill in this function's own prologue -- the first time anything
        // called search_position() on QEMU RV64 (`-march=rv64gc -mabi=lp64d`,
        // real hardware F/D registers in the ABI), because LugalOS's boot
        // code never sets `mstatus.FS` to enable the FPU at all. Fixed by
        // removing the float dependency rather than enabling the FPU: `b`
        // (the branching-factor estimate) only ever needs two decimal
        // digits of precision for a >= comparison, so it's fixed-point
        // (tenths) integer math below, consistent with this same function's
        // own `nps` calculation a few lines up (already integer, same
        // reasoning, done in H4). Enabling the FPU kernel-wide would also
        // have opened a second question this avoids entirely: nothing
        // currently saves/restores FP register state across a preemptive
        // task switch (B6), so a genuinely working FPU needs that solved
        // too before it's safe to use anywhere, not just here.
        if (max_search_time_ms != -1) {
            long b10 = 35; // default branching factor estimate, x10 (3.5)
            if (d >= 2 && prev_iter_time_ms > 0) {
                b10 = (last_iter_time_ms * 10) / prev_iter_time_ms;
                if (b10 < 25) b10 = 25;
                if (b10 > 50) b10 = 50;
            }
            prev_iter_time_ms = last_iter_time_ms;

            long est_next_iter_ms = (b10 * (last_iter_time_ms > 0 ? last_iter_time_ms : 5)) / 10;

            // If time spent plus half of estimated next iteration time exceeds allotted time, stop before launching d+1
            if (time_spent + est_next_iter_ms / 2 >= max_search_time_ms) {
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
