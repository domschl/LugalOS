/*
 * LugalOS chess UI (H4, plan/phase9_chess_computer.md) -- NOT vendored.
 * ~/gith/domschl/LugalChess's own UI (engine/src/console.c, 1637 lines) is a
 * hybrid text-console/UCI-protocol/TM1638+ST7735 driver deeply tied to
 * stdio and a menu system this phase doesn't need; porting it mechanically
 * would have meant touching ~150 printf call sites and ~1600 lines for
 * behavior (UCI, save/load, level menus) explicitly out of scope here (see
 * this phase's "Deliberately out of scope" section). Instead this is a
 * fresh, small implementation against the same two extension points
 * search.c already calls (search_progress_callback/search_poll_stop_callback,
 * declared extern there, defined here) and H1/H2's canvas/TM1638 drivers.
 *
 * Two entry points:
 *   - chess_selftest(): runs the engine from the start position for a fixed
 *     depth and reports the chosen move via cprintf. No hardware
 *     dependency at all -- this is what makes the engine testable on QEMU
 *     before ever touching real silicon (falsify on hardware, not QEMU,
 *     works both ways: prove the *logic* fast and cheap on QEMU first).
 *   - chess_run(): the actual interactive game, TM1638 for move entry,
 *     ST7735 for the board. RP2350 hardware only. Does not return --
 *     reset the board to exit, the same shape as p9serve.
 */

#include "position.h"
#include "movegen.h"
#include "move.h"
#include "search.h"
#include "evaluation.h"
#include "tt.h"
#include "bitboard.h"
#include "zobrist.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DISPLAY
#include "drivers/st7735.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
#include "drivers/tm1638.h"
#endif

#include "chess_ui.h"

#define STANDARD_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

static bool g_chess_ready = false;
static Position g_chess_pos; /* static: ~8 KB (MAX_PLYS=256 history), never a stack local */

static Move g_search_best_move = 0;
static int g_search_score = 0;
static int g_search_depth = 0;

/* Returns false only if search_pools_init() (J0,
 * plan/phase10_chess_completion.md) hits genuine page-allocator exhaustion
 * -- everything else here (bitboards/zobrist tables, the transposition
 * table) is either a fixed small cost or already tolerates its own failure
 * internally (init_tt() degrades to "no TT" rather than crashing if it
 * can't allocate). The move-list pools have no such degraded mode -- the
 * search recurses through them unconditionally -- so this is the one
 * failure callers must actually check. */
static bool chess_ensure_init(void) {
    if (g_chess_ready) return true;
    init_bitboards();
    init_zobrist();
    init_tt(0);
    if (!search_pools_init()) {
        cprintf("chess: out of memory (search move-list pools)\n");
        return false;
    }
    g_chess_ready = true;
    return true;
}

/* search.c calls both of these (`extern void ...` at each call site) but
 * never defines them -- upstream's definitions live in console.c, which
 * this phase doesn't vendor. This is the entire engine/UI seam. */
void search_progress_callback(Move move, int score, int depth) {
    if (depth == 1 && move == 0) {
        g_search_best_move = 0;
        g_search_score = 0;
    }
    if (move != 0) {
        g_search_best_move = move;
        g_search_score = score;
    }
    g_search_depth = depth;
}

void search_poll_stop_callback(void) {
    /* Nothing to poll yet in this first version -- no mid-search abort key,
     * no background housekeeping needed (the search runs synchronously on
     * the calling context's own stack, not a task the scheduler could
     * starve). Present because search.c always calls it every 2048 nodes;
     * an empty body costs one function call, not a busy-wait. */
}

static void format_move(Move m, char *out) {
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    out[0] = (char)('a' + (from % 8));
    out[1] = (char)('1' + (from / 8));
    out[2] = (char)('a' + (to % 8));
    out[3] = (char)('1' + (to / 8));
    if (move_is_promo(m)) {
        static const char promo_chars[] = "nbrq";
        out[4] = promo_chars[move_promo_piece(m) - KNIGHT];
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

/* Runs the engine's own iterative-deepening search and retrieves the move
 * it settled on -- the same two-step "callback, then TT fallback" shape
 * upstream's make_engine_move() uses, since search_position() itself only
 * prints a UCI bestmove line, it never returns the move directly. */
static Move chess_think(Position *pos, int time_limit_ms) {
    g_search_best_move = 0;
    g_search_score = 0;
    search_position(pos, 64, time_limit_ms);

    Move best = g_search_best_move;
    if (best == 0) {
        int score;
        probe_tt_entry(pos->hash_key, &score, &best);
    }
    return best;
}

void chess_selftest(void) {
    if (!chess_ensure_init()) return;
    /* A midgame position, well outside the opening book, so this exercises
     * the real iterative-deepening search (pv_search/quiescence/evaluate/tt)
     * rather than the book-move shortcut. get_book_move() (search.c,
     * vendored) decides "are we still in book" purely from
     * pos->history_ply == 0, not from whether the actual board state is
     * book-reachable -- parse_fen() always resets history_ply to 0, so
     * without this nudge a directly-loaded midgame FEN can still hit the
     * book path (found live on real hardware: a random book-move pick that
     * happened to also be legal in this position returned instantly,
     * depth 1, 0 nodes, while a different random pick on a different boot
     * correctly fell through to real search -- same code, seed-dependent).
     * Not a bug real gameplay ever hits (chess_run() always starts from the
     * true start position and reaches every later position through real
     * make_move() calls, so history_ply genuinely tracks book-reachability
     * there); it's a gap specific to loading a FEN mid-game, which only
     * this self-test does. */
    parse_fen(&g_chess_pos, "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4");
    g_chess_pos.history_ply = 1;

    cprintf("chess: searching a midgame position (2s)...\n");
    Move best = chess_think(&g_chess_pos, 2000);

    if (best == 0) {
        cprintf("chess: no move found\n");
        return;
    }
    char buf[6];
    format_move(best, buf);
    cprintf("chess: best move %s, score %d, depth %d, %ld nodes\n",
            buf, g_search_score, g_search_depth, nodes_searched);
}

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DISPLAY
/* 16x16 monochrome chess piece bitmaps, vendored from
 * ~/gith/domschl/LugalChess (firmware/st7735.c) -- presentation data, not
 * logic, and specific to this UI (H1's canvas driver deliberately doesn't
 * carry chess content). Indexed PAWN..KING (defs.h's piece enum). */
static const uint16_t piece_bitmaps[6][16] = {
    // Pawn
    { 0x0000, 0x0000, 0x03c0, 0x07e0, 0x07e0, 0x03c0, 0x0180, 0x03c0,
      0x0ff0, 0x1ff8, 0x1ff8, 0x0ff0, 0x0ff0, 0x1ff8, 0x3ffc, 0x0000 },
    // Knight
    { 0x0000, 0x03c0, 0x0fe0, 0x1fe0, 0x3de0, 0x3be0, 0x3c00, 0x3f00,
      0x3fc0, 0x1ff0, 0x0ff8, 0x0ff8, 0x1ffc, 0x3ffe, 0x3ffe, 0x0000 },
    // Bishop
    { 0x0000, 0x0180, 0x0180, 0x07e0, 0x0ff0, 0x1e78, 0x3c3c, 0x381c,
      0x3c3c, 0x1e78, 0x0ff0, 0x07e0, 0x03c0, 0x1ff8, 0x3ffc, 0x0000 },
    // Rook
    { 0x0000, 0x0000, 0x3cc3, 0x3ffc, 0x3ffc, 0x1ff8, 0x1ff8, 0x1ff8,
      0x1ff8, 0x1ff8, 0x1ff8, 0x1ff8, 0x3ffc, 0x3ffc, 0x3ffc, 0x0000 },
    // Queen
    { 0x0000, 0x2492, 0x2cda, 0x3ffe, 0x1ff8, 0x1ff8, 0x1e78, 0x1c38,
      0x1ff8, 0x1ff8, 0x0ff0, 0x0ff0, 0x1ff8, 0x3ffc, 0x3ffc, 0x0000 },
    // King
    { 0x0180, 0x07e0, 0x0180, 0x0ff0, 0x1ff8, 0x3ffe, 0x3c3c, 0x3c3c,
      0x3ffe, 0x1ff8, 0x1e78, 0x0ff0, 0x0ff0, 0x1ff8, 0x3ffc, 0x0000 }
};

/* Light square darkened from the vendored value (0xEF7B, a near-white
 * cream) to a warm tan -- confirmed live on the physical panel that white
 * pieces (drawn solid ST7735_WHITE) were nearly imperceptible against it.
 * RGB(222,196,160) here instead. */
#define SQ_LIGHT 0xDE13
#define SQ_DARK  0x2444

static void draw_chess_board(const Position *pos) {
    for (int r = 7; r >= 0; r--) {
        for (int f = 0; f < 8; f++) {
            int sq = f + r * 8;
            int x = f * 16;
            int y = (7 - r) * 16;
            uint16_t sq_color = ((r + f) % 2 == 1) ? SQ_LIGHT : SQ_DARK;

            int piece = pos->board[sq];
            if (piece != NO_PIECE) {
                int side = (pos->color_bbs[WHITE] & (1ULL << sq)) ? WHITE : BLACK;
                uint16_t fg = (side == WHITE) ? ST7735_WHITE : ST7735_BLACK;
                st7735_draw_bitmap_mono(x, y, 16, 16, piece_bitmaps[piece], fg, sq_color);
            } else {
                st7735_draw_rect(x, y, 16, 16, sq_color);
            }
        }
    }
}

static void draw_chess_status(const Position *pos, const char *last_move, bool thinking) {
    st7735_draw_rect(0, 128, 128, 32, ST7735_BLACK);
    st7735_draw_rect(0, 127, 128, 1, ST7735_GRAY);

    const char *side_str = (pos->side == WHITE) ? "White" : "Black";
    st7735_draw_string(2, 131, thinking ? "Thinking..." : side_str,
                        thinking ? ST7735_CYAN : ST7735_YELLOW, 1);

    if (last_move && last_move[0] != '\0') {
        st7735_draw_string(2, 145, last_move, ST7735_WHITE, 1);
    }
}
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
/* Waits for the keypad to go idle, then for a key to be pressed, then for
 * it to be released again -- each stage a plain poll of tm1638_get_key(),
 * which is an instantaneous scan, not a latch. Logs every raw reading to
 * the console (not the 7-segment display) while diagnosing a reported
 * hang -- cheap, and it's the only way to see what the hardware is
 * actually reporting versus what the physical fingers are doing. Each
 * stage is also capped at ~15s so a bad reading degrades to a clear
 * console message instead of a silent, indefinite block. */
static int tm_wait_key(void) {
    int iters;

    iters = 0;
    while (tm1638_get_key() != -1) {
        if (iters == 0) cprintf("chess: tm_wait_key: waiting for release before scan\n");
        time_delay_us(20000);
        if (++iters > 750) { cprintf("chess: tm_wait_key: idle-wait timed out\n"); break; }
    }

    int k;
    iters = 0;
    do {
        k = tm1638_get_key();
        if (k != -1) cprintf("chess: tm_wait_key: raw key=%d\n", k);
        time_delay_us(20000);
        if (++iters > 750) { cprintf("chess: tm_wait_key: press-wait timed out\n"); return -1; }
    } while (k == -1);

    iters = 0;
    while (tm1638_get_key() != -1) {
        time_delay_us(20000);
        if (++iters > 750) { cprintf("chess: tm_wait_key: release-wait timed out\n"); break; }
    }
    return k;
}

/* One square takes two key presses, both from the same 0-7 range -- the
 * first is the file, the second is the rank (confirmed against real
 * hardware and the project author directly: keys 0-7 double up for both
 * file and rank entry, in sequence; 8-15 are reserved for menu functions
 * (level/board/info/options) this phase doesn't implement yet, so they're
 * ignored here rather than misread as a rank digit -- an earlier version
 * of this code assumed 8-15 meant rank, which live hardware testing showed
 * was wrong: it just silently waited forever for a key range the physical
 * pad never sends for a normal move). */
static int tm_read_square(const char *prompt) {
    tm1638_display_string(prompt);
    int file = -1, rank = -1;
    while (file < 0 || rank < 0) {
        int k = tm_wait_key();
        if (k < 0 || k > 7) continue; /* 8-15: menu keys, not handled yet */
        if (file < 0) {
            file = k;
        } else {
            rank = k;
        }
        char buf[9] = "        ";
        if (file >= 0) buf[0] = (char)('A' + file);
        if (rank >= 0) buf[1] = (char)('1' + rank);
        tm1638_display_string(buf);
    }
    return rank * 8 + file;
}

/* Prompts for from/to squares and resolves them against the actual legal
 * move list (defaulting promotions to Queen -- the keypad has no piece
 * picker in this version), retrying on anything that doesn't resolve to a
 * legal move. */
static Move tm_read_move(Position *pos) {
    for (;;) {
        int from = tm_read_square("FrOM    ");
        int to = tm_read_square("tO      ");

        MoveList list;
        generate_moves(pos, &list);
        Move chosen = 0;
        for (int i = 0; i < list.count; i++) {
            Move m = list.moves[i];
            if (MOVE_FROM(m) != from || MOVE_TO(m) != to) continue;
            if (move_is_promo(m) && move_promo_piece(m) != QUEEN) continue;
            chosen = m;
            break;
        }
        if (chosen != 0) {
            return chosen;
        }
        tm1638_display_string("bAd MOuE");
        time_delay_us(700000);
    }
}

#if CONFIG_ENABLE_DISPLAY
void chess_run(void) {
    if (!chess_ensure_init()) return;
    parse_fen(&g_chess_pos, STANDARD_START_FEN);

    tm1638_display_string("LUgAL Ch");
    draw_chess_board(&g_chess_pos);
    draw_chess_status(&g_chess_pos, "", false);
    time_delay_us(1000000);

    char last_move_buf[6] = "";

    for (;;) {
        tm1638_display_string("YOUr MOu");
        Move human = tm_read_move(&g_chess_pos);
        if (!make_move(&g_chess_pos, human)) {
            /* tm_read_move() only returns pseudo-legal moves from the real
             * move list, so make_move() only fails here on a king-safety
             * violation (a pin, moving into check) -- a real "no, that
             * specific move is illegal", not a parse failure. */
            tm1638_display_string("ILLEGAL ");
            time_delay_us(700000);
            continue;
        }
        format_move(human, last_move_buf);
        draw_chess_board(&g_chess_pos);
        draw_chess_status(&g_chess_pos, last_move_buf, false);

        tm1638_display_string("tHInKIng");
        draw_chess_status(&g_chess_pos, last_move_buf, true);
        Move engine_move = chess_think(&g_chess_pos, 5000);
        if (engine_move == 0) {
            tm1638_display_string("gAME OuEr");
            draw_chess_status(&g_chess_pos, "no moves", false);
            for (;;) time_delay_us(1000000); /* game over: reset to play again */
        }
        make_move(&g_chess_pos, engine_move);
        format_move(engine_move, last_move_buf);
        tm1638_display_string(last_move_buf);
        draw_chess_board(&g_chess_pos);
        draw_chess_status(&g_chess_pos, last_move_buf, false);
    }
}
#endif /* CONFIG_ENABLE_DISPLAY */
#endif /* CONFIG_BOARD_RP2350 && CONFIG_ENABLE_TM1638 */
