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
 *     ST7735 for the board. RP2350 hardware only. Runs until Ctrl-C or the
 *     TM1638 STOP key (J2, plan/phase10_chess_completion.md -- chess_run()
 *     had no software exit at all before that, only a physical board
 *     reset), then returns cleanly to the shell.
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
#include "kernel/line_editor.h"
#include "fs/vfs.h"
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
 * failure callers must actually check.
 *
 * g_chess_ready is a real acquire/release latch, not a "run once, ever"
 * flag -- chess_session_end() below clears it, so a later call re-runs
 * this in full. init_bitboards()/init_zobrist() are cheap and idempotent
 * (a few thousand static-array writes, no allocation), so redoing them on
 * every session is not worth special-casing around. */
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

/* Releases the ~100 KB (25 pages) chess_ensure_init() acquires from the
 * page allocator -- the transposition table (32 KB, tt.c's own
 * pre-existing free_tt(), simply never called before this) and J0's
 * on-demand move-list pools (68 KB, search_pools_free()) -- back to
 * "exactly as before", matching cc/ed's own acquire-per-use precedent
 * (phase8) instead of J0's original "never freed for the process
 * lifetime" choice.
 *
 * That choice was correct when J0 made it: no chess entry point had a
 * session boundary to free at yet. J1 added one (chess_console_run()'s
 * `quit`), which is what makes this callable at all now -- heap is
 * LugalOS's scarcest resource (RP2350: 512 KB total), and a program that
 * quits back to the shell should leave the heap exactly as it found it,
 * the same rule cc/ed already follow.
 *
 * Does NOT touch bitboard.c's attack/mask tables or zobrist.c's hash
 * tables -- both are plain static `.bss` (~2.1 KB and ~6.6 KB measured
 * directly: pawn/knight/king attack tables + file/rank masks; the
 * zobrist piece/castling/en-passant/side hash tables), a fixed cost of
 * `CONFIG_ENABLE_CHESS=ON` at *link* time, present in RAM from boot
 * regardless of whether chess ever runs. There is no runtime allocation
 * there for "on demand" to mean anything -- the lever for that ~9 KB is
 * the build-time flag itself (phase8), not a session boundary. */
static void chess_session_end(void) {
    if (!g_chess_ready) return;
    free_tt();
    search_pools_free();
    g_chess_ready = false;
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

/* Standardizes "has the user asked to abort whatever's currently
 * blocking" into one check, used by every polling wait in this file --
 * not just search.c's node-count poll below, but tm_wait_key() further
 * down too. Found live, not designed up front: chess_run()'s keypad wait
 * had *no* software escape at all before this, only a physical board
 * reset (this is what left a board unreachable mid-session once this
 * phase's own Ctrl-C testing entered chess_run() by mistake).
 *
 * Two independent gestures, distinguished rather than collapsed into one
 * bool -- J3 needs to know which, since they mean different things once
 * TM1638 menus exist. A terminal Ctrl-C (console_interrupt_requested(),
 * kernel/console.c, not chess-specific -- the same need recurs for
 * run-away Lisp scripts) always means "get me out of chess_run()
 * entirely," everywhere, including from inside a submenu -- the universal
 * panic button, consistent with what it means everywhere else in the OS.
 * The TM1638 STOP key (key 11) is context-dependent instead, the same way
 * it already was in upstream console.c: it stops a running search
 * (unchanged), exits chess_run() when pressed while simply waiting for a
 * move (J2's original behavior, kept -- there's no "typing in progress"
 * to back out of first the way console.c's own line-buffer approach has),
 * but *cancels a submenu back to normal play* rather than leaving the
 * game when pressed inside one of J3's board-view/level-select/options
 * screens below. Callers that care about the distinction read the enum
 * directly; callers that don't (search_poll_stop_callback() itself) just
 * check for anything other than CHESS_ABORT_NONE. */
typedef enum {
    CHESS_ABORT_NONE = 0,
    CHESS_ABORT_CTRLC,
    CHESS_ABORT_STOPKEY,
} ChessAbort;

static ChessAbort chess_abort_requested(void) {
    if (console_interrupt_requested()) {
        return CHESS_ABORT_CTRLC;
    }
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    if (tm1638_get_key() == 11) {
        /* Debounce -- wait for the key to actually release, same shape as
         * console.c:320-327, so one press doesn't register as several. */
        while (tm1638_get_key() == 11) {
            time_delay_us(10000);
        }
        return CHESS_ABORT_STOPKEY;
    }
#endif
    return CHESS_ABORT_NONE;
}

/* search.c's check_up_time() calls this every 2048 nodes -- a cooperative
 * poll already wired into the engine's inner loop. Sets `stop_search`,
 * which pv_search()/quiescence() already check on the way back out of
 * their own recursion. Either abort gesture stops a running search --
 * this one call site doesn't need to distinguish which. */
void search_poll_stop_callback(void) {
    if (chess_abort_requested() != CHESS_ABORT_NONE) {
        stop_search = true;
    }
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
 * prints a UCI bestmove line, it never returns the move directly.
 * max_depth is a J1 addition (plan/phase10_chess_completion.md) for the
 * console's `go depth N` -- chess_selftest()/chess_run() both still pass 64
 * (their only ever value), so this isn't a behavior change for them. */
static Move chess_think(Position *pos, int max_depth, int time_limit_ms) {
    g_search_best_move = 0;
    g_search_score = 0;
    /* Clears any Ctrl-C latched from a previous search (or typed at an idle
     * prompt) before this one starts -- otherwise a stale interrupt would
     * abort the very next search instantly, since console_interrupt_
     * requested() is sticky until cleared. Every search entry point goes
     * through this one function, so clearing here covers all of them. */
    console_interrupt_clear();
    search_position(pos, max_depth, time_limit_ms);

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
    Move best = chess_think(&g_chess_pos, 64, 2000);

    if (best == 0) {
        cprintf("chess: no move found\n");
        chess_session_end();
        return;
    }
    char buf[6];
    format_move(best, buf);
    cprintf("chess: best move %s, score %d, depth %d, %ld nodes\n",
            buf, g_search_score, g_search_depth, nodes_searched);
    /* One-shot benchmark, not an interactive session -- tears down like a
     * `quit` would (chess_console_run() below), so repeated
     * (chess-selftest) calls stay "stateless" rather than only the first
     * one paying the allocation cost forever after. */
    chess_session_end();
}

/* ------------------------------------------------------------------ *
 * J2 (plan/phase10_chess_completion.md): game-outcome detection --   *
 * checkmate/stalemate/draw -- and check status. Pure position logic, *
 * no stdio, built entirely from primitives H4 already vendored       *
 * (generate_moves, make_move/unmake_move, is_square_attacked).       *
 * Shared by both front ends below: chess_console_run() and           *
 * chess_run() each call these after every half-move rather than      *
 * duplicating the logic per UI, ported from console.c's               *
 * check_and_display_game_over()/check_and_update_check_status()      *
 * (:1031-1098, :993-1009) minus their printf/TM1638-specific display  *
 * calls, which the callers below do differently (cprintf text vs.    *
 * an 8-character TM1638 string). *
 * ------------------------------------------------------------------ */

typedef enum {
    CHESS_ONGOING = 0,
    CHESS_CHECKMATE_WHITE, /* White (to move) has no legal moves and is in
                             * check -- Black wins. */
    CHESS_CHECKMATE_BLACK, /* Black (to move) has no legal moves and is in
                             * check -- White wins. */
    CHESS_STALEMATE,
    CHESS_DRAW_REPETITION,
    CHESS_DRAW_50MOVE,
} ChessOutcome;

static bool chess_has_legal_move(Position *pos) {
    static MoveList list;
    generate_moves(pos, &list);
    for (int i = 0; i < list.count; i++) {
        if (make_move(pos, list.moves[i])) {
            unmake_move(pos);
            return true;
        }
    }
    return false;
}

/* Ported from console.c's is_threefold_repetition() (:1011-1029): a
 * position (by hash) recurring 3 times (including the current one) since
 * the last irreversible move (pawn push or capture, i.e. within
 * pos->halfmove plies of history) is a draw. */
static bool chess_is_threefold_repetition(const Position *pos) {
    if (pos->history_ply < 4) return false;

    int count = 1; /* the current position is the first occurrence */
    int start_ply = pos->history_ply - 1;
    int end_ply = pos->history_ply - pos->halfmove;
    if (end_ply < 0) end_ply = 0;

    for (int i = start_ply; i >= end_ply; i--) {
        if (pos->history[i].hash_key == pos->hash_key) {
            count++;
            if (count >= 3) return true;
        }
    }
    return false;
}

static bool chess_in_check(const Position *pos) {
    return is_square_attacked(pos, get_lsb(pos->piece_bbs[KING] & pos->color_bbs[pos->side]), pos->side ^ 1);
}

static ChessOutcome chess_game_outcome(Position *pos) {
    if (!chess_has_legal_move(pos)) {
        if (chess_in_check(pos)) {
            return (pos->side == WHITE) ? CHESS_CHECKMATE_WHITE : CHESS_CHECKMATE_BLACK;
        }
        return CHESS_STALEMATE;
    }
    if (chess_is_threefold_repetition(pos)) return CHESS_DRAW_REPETITION;
    if (pos->halfmove >= 100) return CHESS_DRAW_50MOVE;
    return CHESS_ONGOING;
}

/* ------------------------------------------------------------------ *
 * J1 (plan/phase10_chess_completion.md): the plain-terminal console  *
 * REPL -- LugalChess's own scenario 1.1, the baseline that had no    *
 * LugalOS equivalent at all before this. No hardware dependency,     *
 * unlike chess_run() below: builds and is fully testable on every    *
 * QEMU target, same category as chess_selftest() above.              *
 *                                                                     *
 * Ported from ~/gith/domschl/LugalChess's console.c (console_loop()  *
 * and execute_player_move(), line numbers below refer to that file)  *
 * minus everything gated on LUGALCHESS_EMBEDDED or is_uci_client_mode*
 * -- the plain-REPL command set only. save/load are the one command  *
 * pair that couldn't port as-is: upstream writes a raw QSPI flash    *
 * sector (hardware/flash.h), which doesn't exist on LugalOS at all;  *
 * this uses LugalOS's own VFS instead (H0, plan/phase9_chess_computer*
 * .md, flagged this exact incompatibility as the reason console.c    *
 * wasn't vendored wholesale).                                        *
 * ------------------------------------------------------------------ */

/* console.c's own level_times_ms[8]/search_level (:21-22), unchanged --
 * "level 8" is -1 (infinite / manual stop). No console-side interrupt
 * exists yet in this milestone (J2 adds Ctrl-C, plan/
 * phase10_chess_completion.md) -- level 8 is still selectable here,
 * matching upstream's own command surface, but picking it and moving
 * means the search runs to full depth-64 completion with nothing able to
 * cut it short until J2 lands. print_console_help() says so. */
static const int level_times_ms[8] = { 1000, 2000, 5000, 10000, 15000, 30000, 60000, -1 };
static int g_console_search_level = 2;
static int g_console_max_history_ply = 0;

/* Picks whichever of /sd0 or /ram0 is actually mounted and writable --
 * hardcoding /ram0 (found live: init.lisp only mounts a RAM disk *when
 * /sd0 isn't already writable*, so a board with a working SD card never
 * mounts /ram0 at all, and every save/load would have silently failed
 * with no such volume) or /sd0 (fails the inverse case, QEMU's own default
 * persona) would each be wrong on some board. Two small fixed-literal
 * lookups, not one path built by splitting the other -- no `strrchr` in
 * LugalOS's libc (only `strchr`, first-occurrence) to find the last '/',
 * and no `snprintf` either (position.c's own header comment already says
 * why) to build one path from parts. */
static const char *console_save_dir(void) {
    return vfs_volume_writable("/sd0") ? "/sd0/system" : "/ram0/system";
}
static const char *console_save_path(void) {
    return vfs_volume_writable("/sd0") ? "/sd0/system/chess.save"
                                        : "/ram0/system/chess.save";
}

/* Ported from console.c's execute_player_move() (:415-477), the parse
 * direction to format_move()'s already-existing format direction above.
 * `list` is a function-static, not a stack local, mirroring upstream's own
 * choice for the same reason (~516 bytes, MAX_MOVES=256) and matching how
 * every other MoveList in this file avoids the stack. */
static bool console_execute_move(Position *pos, const char *move_str) {
    if (move_str[0] < 'a' || move_str[0] > 'h' || move_str[1] < '1' || move_str[1] > '8' ||
        move_str[2] < 'a' || move_str[2] > 'h' || move_str[3] < '1' || move_str[3] > '8') {
        return false;
    }

    int from = (move_str[0] - 'a') + (move_str[1] - '1') * 8;
    int to = (move_str[2] - 'a') + (move_str[3] - '1') * 8;
    int promo_piece = NO_PIECE;
    if (move_str[4] != '\0') {
        switch (move_str[4]) {
            case 'q': promo_piece = QUEEN; break;
            case 'r': promo_piece = ROOK; break;
            case 'b': promo_piece = BISHOP; break;
            case 'n': promo_piece = KNIGHT; break;
        }
    }

    static MoveList list;
    generate_moves(pos, &list);

    /* If the caller didn't name a promotion piece but from/to only resolves
     * to promotion moves, default to Queen -- same nudge upstream applies. */
    if (promo_piece == NO_PIECE) {
        bool only_promo = false;
        for (int i = 0; i < list.count; i++) {
            Move m = list.moves[i];
            if (MOVE_FROM(m) == from && MOVE_TO(m) == to && move_is_promo(m)) {
                only_promo = true;
                break;
            }
        }
        if (only_promo) promo_piece = QUEEN;
    }

    for (int i = 0; i < list.count; i++) {
        Move m = list.moves[i];
        if (MOVE_FROM(m) != from || MOVE_TO(m) != to) continue;
        if (promo_piece != NO_PIECE) {
            if (move_is_promo(m) && move_promo_piece(m) == promo_piece) {
                if (make_move(pos, m)) return true;
            }
        } else if (!move_is_promo(m)) {
            if (make_move(pos, m)) return true;
        }
    }
    return false;
}

/* cprintf has no `+` flag (confirmed live: `%+d` printed the literal
 * characters "%+d" rather than a signed number -- kernel/printk.c's format
 * engine only recognizes '0'/width/'.'precision/'l' before the conversion
 * character, chess_platform.h's own header comment already said as much
 * for %f, this is the same gap for a different flag). `%d` already emits
 * '-' for negative values on its own; only the '+' for non-negative ones
 * needs doing by hand. */
static const char *sign_prefix(int v) { return v >= 0 ? "+" : ""; }

static void console_print_outcome(ChessOutcome outcome) {
    switch (outcome) {
        case CHESS_CHECKMATE_WHITE: cprintf("Checkmate! Black wins!\n"); break;
        case CHESS_CHECKMATE_BLACK: cprintf("Checkmate! White wins!\n"); break;
        case CHESS_STALEMATE:       cprintf("Stalemate! Game is a draw.\n"); break;
        case CHESS_DRAW_REPETITION: cprintf("Draw by 3-fold repetition.\n"); break;
        case CHESS_DRAW_50MOVE:     cprintf("Draw by 50-move rule.\n"); break;
        default: break;
    }
}

/* Prints the outcome message and returns true if the game just ended (no
 * further engine work should follow); otherwise prints "Check!" when
 * relevant and returns false. console.c's own two-function split
 * (check_and_display_game_over()/check_and_update_check_status(),
 * :1031-1098/:993-1009) collapses into one call here, since every caller
 * below wants exactly this "did it end, and if not, is it check" sequence
 * together. */
static bool console_report_outcome(Position *pos) {
    ChessOutcome outcome = chess_game_outcome(pos);
    if (outcome != CHESS_ONGOING) {
        console_print_outcome(outcome);
        return true;
    }
    if (chess_in_check(pos)) {
        cprintf("Check!\n");
    }
    return false;
}

/* Shared by the post-move auto-reply and the `go` command below --
 * upstream duplicates this logic between make_engine_move() and the `go`
 * command handler in console.c; one helper here instead. Checks outcome
 * before searching (a position with no legal replies gets the real
 * checkmate/stalemate/draw message, not the generic "resigned" fallback
 * below, which upstream's own make_engine_move() used unconditionally --
 * this milestone is what adds the distinction) and again after, printing
 * the board either way so both this function's callers (a bare move's
 * auto-reply, and the `go` command, which upstream's own console.c also
 * prints the board after) get consistent output without duplicating the
 * print_board() call at each call site. */
static void console_engine_reply(Position *pos, int max_depth, int time_limit_ms) {
    if (console_report_outcome(pos)) {
        return; /* game already over before the engine could reply */
    }
    Move best = chess_think(pos, max_depth, time_limit_ms);
    if (best == 0) {
        cprintf("Engine resigned or found no legal moves.\n");
        return;
    }
    char buf[6];
    format_move(best, buf);
    make_move(pos, best);
    g_console_max_history_ply = pos->history_ply;
    cprintf("Engine plays: %s (Score: %s%d)\n", buf, sign_prefix(g_search_score), g_search_score);
    print_board(pos);
    console_report_outcome(pos);
}

static void console_print_help(void) {
    cprintf("\nLugalOS chess console. Commands:\n");
    cprintf("  help              - Show this help message\n");
    cprintf("  new               - Start a new game from the standard starting position\n");
    cprintf("  board (or d)      - Display the current board state\n");
    cprintf("  level <1-8>       - Set engine search level (1:1s 2:2s 3:5s 4:10s 5:15s "
            "6:30s 7:60s 8:Infinite -- press Ctrl-C to stop an 8/Infinite search)\n");
    cprintf("  fen [FEN]         - Show current FEN, or set a custom FEN position\n");
    cprintf("  save              - Save the current position and level (/sd0 if writable, else /ram0)\n");
    cprintf("  load              - Load the saved position and level\n");
    cprintf("  go [depth N|movetime N] - Force the engine to think and play a move\n");
    cprintf("  stop              - No mid-search effect here (not currently thinking); press "
            "Ctrl-C instead to interrupt a running search\n");
    cprintf("  undo              - Take back 1 half-move\n");
    cprintf("  redo              - Re-apply 1 half-move\n");
    cprintf("  eval              - Print the static evaluation of the current position\n");
    cprintf("  moves             - List all legal moves in the current position\n");
    cprintf("  quit              - Leave the chess console, back to the shell\n");
    cprintf("  <move>            - Play a move in UCI format (e.g. e2e4, g1f3, e7e8q)\n\n");
}

static void console_new_game(Position *pos) {
    parse_fen(pos, STANDARD_START_FEN);
    clear_tt();
    g_console_max_history_ply = 0;
}

static void console_save(const Position *pos) {
    const char *path = console_save_path();
    char buf[300];
    int n = 0;
    generate_fen(pos, buf);
    n = (int)strlen(buf);
    buf[n++] = '\n';
    /* search_level is at most one digit (1-8) -- no snprintf needed. */
    buf[n++] = (char)('0' + g_console_search_level);
    buf[n++] = '\n';
    buf[n] = '\0';

    /* Best-effort: the volume's own "system/" directory may not exist yet
     * (found live on a freshly formatted /ram0 -- vfs_write() does not
     * create missing parent directories, and there is no reason to expect
     * every volume already has this one). Ignore the result: it either
     * already existed (this fails harmlessly) or didn't (this is what
     * makes the write below succeed). */
    vfs_mkdir(console_save_dir());

    if (vfs_write(path, buf, (uint32_t)n) == 0) {
        cprintf("Position and level saved to %s\n", path);
    } else {
        cprintf("Save failed (could not write %s)\n", path);
    }
}

static void console_load(Position *pos) {
    const char *path = console_save_path();
    char buf[300];
    int bytes = vfs_read(path, buf, sizeof(buf) - 1);
    if (bytes <= 0) {
        cprintf("No saved game found at %s\n", path);
        return;
    }
    buf[bytes] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    static Position temp_pos;
    parse_fen(&temp_pos, buf);
    if (!is_position_valid(&temp_pos)) {
        cprintf("Save file is corrupt (invalid FEN)\n");
        return;
    }
    *pos = temp_pos;
    clear_tt();
    g_console_max_history_ply = pos->history_ply;

    if (nl && nl[1] >= '1' && nl[1] <= '8') {
        g_console_search_level = nl[1] - '0';
    }
    cprintf("Position loaded from %s (level %d)\n", path, g_console_search_level);
    print_board(pos);
}

/* `go [depth N | movetime N]`, ported from console.c's `go` command
 * handler (:1441-1521) minus its UCI wtime/btime branches (this is the
 * plain console, not a UCI session -- J5 gets its own clean `go` against
 * uci.c's much smaller parser instead of this one). No arguments falls
 * back to the current level's time budget, same as a bare move's
 * auto-reply. */
static void console_handle_go(Position *pos, const char *args) {
    int max_depth = 64;
    int time_limit_ms;
    const char *p;
    if ((p = strstr(args, "movetime")) != NULL) {
        time_limit_ms = atoi(p + 8);
    } else if ((p = strstr(args, "depth")) != NULL) {
        max_depth = atoi(p + 5);
        time_limit_ms = -1;
    } else {
        time_limit_ms = level_times_ms[g_console_search_level - 1];
    }
    console_engine_reply(pos, max_depth, time_limit_ms);
}

static void console_list_moves(Position *pos) {
    static MoveList list;
    generate_moves(pos, &list);
    cprintf("Legal moves in this position:\n");
    int legal_cnt = 0;
    for (int i = 0; i < list.count; i++) {
        Move m = list.moves[i];
        if (!make_move(pos, m)) continue;
        char buf[6];
        format_move(m, buf);
        cprintf("  %s\n", buf);
        legal_cnt++;
        unmake_move(pos);
    }
    cprintf("Total legal moves: %d\n", legal_cnt);
}

/* Does not return until `quit` -- the same shape as `lsh` itself
 * (kernel/shell.c), not chess_run()'s "does not return at all" shape,
 * since there's no hardware state to reset out of here. */
void chess_console_run(void) {
    if (!chess_ensure_init()) return;
    console_new_game(&g_chess_pos);

    cprintf("\nLugalOS chess console. Type 'help' for commands, 'quit' to leave.\n");
    print_board(&g_chess_pos);

    char line[300];
    for (;;) {
        readline_interactive("chess> ", line, sizeof(line));

        if (line[0] == '\0') {
            continue;
        } else if (strcmp(line, "help") == 0) {
            console_print_help();
        } else if (strcmp(line, "new") == 0) {
            console_new_game(&g_chess_pos);
            cprintf("New game started.\n");
            print_board(&g_chess_pos);
        } else if (strcmp(line, "board") == 0 || strcmp(line, "d") == 0) {
            print_board(&g_chess_pos);
            print_position_info(&g_chess_pos);
        } else if (strncmp(line, "level", 5) == 0) {
            const char *p = line + 5;
            while (*p == ' ') p++;
            int val = atoi(p);
            if (val >= 1 && val <= 8) {
                g_console_search_level = val;
                if (level_times_ms[val - 1] != -1) {
                    cprintf("Search level set to %d (%ds per move).\n", val, level_times_ms[val - 1] / 1000);
                } else {
                    cprintf("Search level set to 8 (Infinite -- press Ctrl-C to stop it).\n");
                }
            } else {
                cprintf("Invalid level. Specify 1 to 8 (see 'help').\n");
            }
        } else if (strncmp(line, "fen", 3) == 0) {
            const char *p = line + 3;
            while (*p == ' ') p++;
            if (*p == '\0') {
                char buf[256];
                generate_fen(&g_chess_pos, buf);
                cprintf("Current FEN: %s\n", buf);
            } else {
                static Position temp_pos;
                parse_fen(&temp_pos, p);
                if (!is_position_valid(&temp_pos)) {
                    cprintf("Invalid FEN position (need exactly 1 White king, 1 Black king, valid check state).\n");
                } else {
                    g_chess_pos = temp_pos;
                    clear_tt();
                    g_console_max_history_ply = g_chess_pos.history_ply;
                    cprintf("Position loaded.\n");
                    print_board(&g_chess_pos);
                }
            }
        } else if (strcmp(line, "save") == 0) {
            console_save(&g_chess_pos);
        } else if (strcmp(line, "load") == 0) {
            console_load(&g_chess_pos);
        } else if (strncmp(line, "go", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
            console_handle_go(&g_chess_pos, line);
        } else if (strcmp(line, "undo") == 0) {
            if (g_chess_pos.history_ply > 0) {
                unmake_move(&g_chess_pos);
                cprintf("Took back 1 half-move.\n");
                print_board(&g_chess_pos);
            } else {
                cprintf("Nothing to undo.\n");
            }
        } else if (strcmp(line, "redo") == 0) {
            if (g_chess_pos.history_ply < g_console_max_history_ply) {
                Move m = g_chess_pos.history[g_chess_pos.history_ply].move;
                if (make_move(&g_chess_pos, m)) {
                    cprintf("Re-applied 1 half-move.\n");
                    print_board(&g_chess_pos);
                } else {
                    cprintf("Cannot redo move.\n");
                }
            } else {
                cprintf("Nothing to redo.\n");
            }
        } else if (strcmp(line, "eval") == 0) {
            int score = evaluate(&g_chess_pos);
            cprintf("Static evaluation: %s%d centipawns (current side's perspective)\n",
                    sign_prefix(score), score);
        } else if (strcmp(line, "moves") == 0) {
            console_list_moves(&g_chess_pos);
        } else if (strcmp(line, "stop") == 0) {
            /* UCI-protocol-compatibility no-op, matching console.c:1603-1606
             * -- the engine is never "thinking" at this point in the REPL
             * (readline_interactive() only reads this line once the
             * previous search has already returned), so there is nothing
             * to interrupt here specifically. Ctrl-C is what actually
             * reaches a search in progress, since it's polled from inside
             * search_poll_stop_callback() while search_position() itself
             * owns the call stack -- typed input can't reach this REPL
             * loop until that returns. */
            cprintf("Engine is not currently thinking.\n");
        } else if (strcmp(line, "quit") == 0) {
            chess_session_end();
            return;
        } else if (console_execute_move(&g_chess_pos, line)) {
            g_console_max_history_ply = g_chess_pos.history_ply;
            print_board(&g_chess_pos);
            if (!console_report_outcome(&g_chess_pos)) {
                console_engine_reply(&g_chess_pos, 64, level_times_ms[g_console_search_level - 1]);
            }
        } else {
            cprintf("Unknown command or invalid move: '%s'. Type 'help' for a list.\n", line);
        }
    }
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
/* -1 (already used below for "no key yet, keep polling") is not distinct
 * enough for "abort" -- tm_read_square()'s own retry loop already treats
 * any negative/out-of-range reading as "keep trying", which is correct
 * for a genuine timeout but wrong for a deliberate abort request, which
 * must unwind instead. Three sentinels, checked explicitly by every
 * caller in this chain:
 *   TM_KEY_ABORT_CTRLC/_STOPKEY -- chess_abort_requested()'s two gestures,
 *     kept distinct here too (not collapsed the way search_poll_stop_
 *     callback() collapses them) because J3's menus below need to tell
 *     them apart: Ctrl-C always propagates out of chess_run() entirely,
 *     the physical STOP key only does that at the top level (waiting for
 *     a move) and instead cancels a submenu back to normal play when
 *     pressed from inside one.
 *   TM_KEY_RESTART -- the position changed underneath the current move
 *     attempt (undo/redo/new-game/load, all below), so tm_read_move()
 *     should re-prompt from FrOM rather than continue asking for a square
 *     that no longer makes sense against the new position. */
#define TM_KEY_ABORT_CTRLC   (-2)
#define TM_KEY_ABORT_STOPKEY (-3)
#define TM_KEY_RESTART        (-4)

/* Waits for the keypad to go idle, then for a key to be pressed, then for
 * it to be released again -- each stage a plain poll of tm1638_get_key(),
 * which is an instantaneous scan, not a latch. Logs every raw reading to
 * the console (not the 7-segment display) while diagnosing a reported
 * hang -- cheap, and it's the only way to see what the hardware is
 * actually reporting versus what the physical fingers are doing. Each
 * stage is also capped at ~15s so a bad reading degrades to a clear
 * console message instead of a silent, indefinite block.
 *
 * Also checks chess_abort_requested() (J2) in the two stages that can
 * genuinely run long waiting on the human -- found live, not designed up
 * front, that this loop previously had no software escape at all: a
 * board that entered chess_run() and then received no further key
 * presses was unreachable by anything short of a physical reset, Ctrl-C
 * included, since nothing in this file's blocking waits ever checked for
 * one. */
static int tm_wait_key(void) {
    int iters;

    iters = 0;
    while (tm1638_get_key() != -1) {
        if (iters == 0) cprintf("chess: tm_wait_key: waiting for release before scan\n");
        ChessAbort a = chess_abort_requested();
        if (a == CHESS_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
        if (a == CHESS_ABORT_STOPKEY) return TM_KEY_ABORT_STOPKEY;
        time_delay_us(20000);
        if (++iters > 750) { cprintf("chess: tm_wait_key: idle-wait timed out\n"); break; }
    }

    int k;
    iters = 0;
    do {
        k = tm1638_get_key();
        if (k != -1) cprintf("chess: tm_wait_key: raw key=%d\n", k);
        ChessAbort a = chess_abort_requested();
        if (a == CHESS_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
        if (a == CHESS_ABORT_STOPKEY) return TM_KEY_ABORT_STOPKEY;
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

/* Redraws the ST7735 board/status if this build actually has one --
 * tm_read_square()/the menu functions below live in the TM1638-only
 * guard, which must keep compiling on a TM1638-without-display board
 * (H3's three independent flags), so this is the one place that
 * optionality is handled rather than repeating the #if at every call
 * site that wants a redraw after undo/redo/new-game/load. */
static void tm_redraw_if_display(const Position *pos) {
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DISPLAY
    draw_chess_board(pos);
    draw_chess_status(pos, "", false);
#else
    (void)pos;
#endif
}

/* Ported from console.c's show_board_rank() (:197-225). tm1638_display_
 * string() already special-cases '.' as "set the previous digit's decimal
 * point, don't consume a digit slot of its own" (drivers/tm1638_rp2350.c),
 * which is what lets `formatted` here safely exceed 8 characters when
 * every square on the rank holds a White piece. */
static void show_board_rank(const Position *pos, int rank) {
    char rank_name[9] = "rAnK  0 ";
    rank_name[6] = (char)('1' + rank);
    tm1638_display_string(rank_name);
    time_delay_us(350000);

    char formatted[17];
    int f_idx = 0;
    for (int file = 0; file < 8; file++) {
        int sq = file + rank * 8;
        int piece = pos->board[sq];
        if (piece == NO_PIECE) {
            formatted[f_idx++] = '_';
        } else {
            static const char piece_chars[] = "Pnbrqk";
            formatted[f_idx++] = piece_chars[piece];
            int color = (pos->color_bbs[WHITE] & (1ULL << sq)) ? WHITE : BLACK;
            if (color == WHITE) formatted[f_idx++] = '.';
        }
    }
    formatted[f_idx] = '\0';
    tm1638_display_string(formatted);
}

/* Board view (key 10): 8/9 scroll ranks, STOP exits back to the caller,
 * Ctrl-C propagates out of chess_run() entirely. Returns TM_KEY_ABORT_
 * CTRLC or 0 (STOP/normal exit) -- never RESTART, this menu never changes
 * the position. */
static int tm_board_view(const Position *pos) {
    int rank = 0;
    show_board_rank(pos, rank);
    for (;;) {
        int k = tm_wait_key();
        if (k == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
        if (k == TM_KEY_ABORT_STOPKEY) return 0;
        if (k == 8) {
            if (rank < 7) { rank++; show_board_rank(pos, rank); }
        } else if (k == 9) {
            if (rank > 0) { rank--; show_board_rank(pos, rank); }
        }
    }
}

/* Level select (key 12): 0-7 pick a level directly, 15 confirms, STOP
 * cancels -- same `level_times_ms[8]`/`g_console_search_level` J1's
 * console REPL already uses (chess_ui.c:351-352), not a second copy, so
 * both front ends share one level setting. */
static const char *tm_level_names[8] = {
    "t-1s    ", "t-2s    ", "t-5s    ", "t10s    ",
    "t15s    ", "t30s    ", "t60s    ", "t-In    "
};

static int tm_level_select(void) {
    tm1638_display_string(tm_level_names[g_console_search_level - 1]);
    for (;;) {
        int k = tm_wait_key();
        if (k == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
        if (k == TM_KEY_ABORT_STOPKEY) return 0;
        if (k >= 0 && k <= 7) {
            g_console_search_level = k + 1;
            tm1638_display_string(tm_level_names[k]);
        } else if (k == 15) {
            return 0;
        }
    }
}

/* Simple +/-dd.dd pawns format, avoiding snprintf (position.c's own
 * header comment already established there isn't one on LugalOS) --
 * console.c's own format_score_str() also handles mate-in-N and
 * 100+-pawn notation, neither of which is worth the extra complexity for
 * an 8-character status readout; chess_game_outcome() already reports
 * mate separately (tm_report_outcome() above). */
static void tm_format_score(int score, char *out) {
    int abs_score = score >= 0 ? score : -score;
    if (abs_score > 9999) abs_score = 9999;
    out[0] = score >= 0 ? '+' : '-';
    out[1] = (char)('0' + (abs_score / 1000) % 10);
    out[2] = (char)('0' + (abs_score / 100) % 10);
    out[3] = '.';
    out[4] = (char)('0' + (abs_score / 10) % 10);
    out[5] = (char)('0' + abs_score % 10);
    out[6] = ' ';
    out[7] = ' ';
    out[8] = '\0';
}

/* Save/load, reusing J1's own path-selection and directory-creation logic
 * (console_save_path()/console_save_dir(), chess_ui.c:365-372) -- one
 * save format and one save file shared by both front ends, not a second
 * copy of the /sd0-vs-/ram0 selection. Rendered on the TM1638 instead of
 * cprintf'd, otherwise the same read/write this file already does. */
static void tm_save(const Position *pos) {
    const char *path = console_save_path();
    char buf[300];
    generate_fen(pos, buf);
    int n = (int)strlen(buf);
    buf[n++] = '\n';
    buf[n++] = (char)('0' + g_console_search_level);
    buf[n++] = '\n';
    buf[n] = '\0';
    vfs_mkdir(console_save_dir());
    tm1638_display_string(vfs_write(path, buf, (uint32_t)n) == 0 ? "SAuEd   " : "nO SAuE ");
    time_delay_us(1500000);
}

/* Returns TM_KEY_RESTART on a successful load (the position changed under
 * the caller), 0 otherwise (nothing to load, or a corrupt save file). */
static int tm_load(Position *pos) {
    const char *path = console_save_path();
    char buf[300];
    int bytes = vfs_read(path, buf, sizeof(buf) - 1);
    if (bytes <= 0) {
        tm1638_display_string("nO SAuE ");
        time_delay_us(1500000);
        return 0;
    }
    buf[bytes] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    static Position temp_pos;
    parse_fen(&temp_pos, buf);
    if (!is_position_valid(&temp_pos)) {
        tm1638_display_string("bAd SAuE");
        time_delay_us(1500000);
        return 0;
    }
    *pos = temp_pos;
    clear_tt();
    g_console_max_history_ply = pos->history_ply;
    if (nl && nl[1] >= '1' && nl[1] <= '8') {
        g_console_search_level = nl[1] - '0';
    }
    tm1638_display_string("LOAdEd  ");
    time_delay_us(1500000);
    tm_redraw_if_display(pos);
    return TM_KEY_RESTART;
}

/* Options menu (key 14): 8/9 cycle, 15 confirms, STOP cancels. Seven of
 * console.c's ten options (:106-117) -- "Play Black"/"Play White" are
 * dropped rather than ported: upstream's own idx-2 handler ("Play White")
 * is just `go` (:815-820, i.e. "make the engine move now"), already
 * covered by J1's top-level `go` without a dedicated slot, and idx-1
 * ("Play Black") is a no-op in upstream (closes the menu and nothing
 * else -- nothing there actually sets up playing the other color). Level
 * select is also not duplicated in here -- key 12 already reaches it
 * directly, matching console.c's own idx-4 doing exactly that. */
#define TM_OPTION_COUNT 7
static const char *tm_option_names[TM_OPTION_COUNT] = {
    "nEU gAnE", "ScOrE   ", "SIdES   ", "HAlF    ", "MOuES   ", "SAuE    ", "LOAd    "
};

static int tm_options_menu(Position *pos) {
    int idx = 0;
    tm1638_display_string(tm_option_names[idx]);
    for (;;) {
        int k = tm_wait_key();
        if (k == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
        if (k == TM_KEY_ABORT_STOPKEY) return 0;
        if (k == 8) {
            idx = (idx - 1 + TM_OPTION_COUNT) % TM_OPTION_COUNT;
            tm1638_display_string(tm_option_names[idx]);
        } else if (k == 9) {
            idx = (idx + 1) % TM_OPTION_COUNT;
            tm1638_display_string(tm_option_names[idx]);
        } else if (k == 15) {
            char buf[9];
            switch (idx) {
                case 0: /* new game */
                    parse_fen(pos, STANDARD_START_FEN);
                    clear_tt();
                    g_console_max_history_ply = 0;
                    tm_redraw_if_display(pos);
                    return TM_KEY_RESTART;
                case 1: /* score */
                    tm_format_score(evaluate(pos), buf);
                    tm1638_display_string(buf);
                    time_delay_us(2000000);
                    tm1638_display_string(tm_option_names[idx]);
                    break;
                case 2: /* side to move */
                    tm1638_display_string(pos->side == WHITE ? "SIdE UH " : "SIdE bL ");
                    time_delay_us(2000000);
                    tm1638_display_string(tm_option_names[idx]);
                    break;
                case 3: { /* halfmove clock */
                    int v = pos->halfmove > 999 ? 999 : pos->halfmove;
                    char hbuf[9] = "H- 000  ";
                    hbuf[3] = (char)('0' + (v / 100) % 10);
                    hbuf[4] = (char)('0' + (v / 10) % 10);
                    hbuf[5] = (char)('0' + v % 10);
                    tm1638_display_string(hbuf);
                    time_delay_us(2000000);
                    tm1638_display_string(tm_option_names[idx]);
                    break;
                }
                case 4: { /* move count */
                    int v = pos->history_ply > 999 ? 999 : pos->history_ply;
                    char nbuf[9] = "n- 000  ";
                    nbuf[3] = (char)('0' + (v / 100) % 10);
                    nbuf[4] = (char)('0' + (v / 10) % 10);
                    nbuf[5] = (char)('0' + v % 10);
                    tm1638_display_string(nbuf);
                    time_delay_us(2000000);
                    tm1638_display_string(tm_option_names[idx]);
                    break;
                }
                case 5: /* save */
                    tm_save(pos);
                    return 0;
                case 6: /* load */
                    return tm_load(pos);
            }
        }
    }
}

/* One square takes two key presses, both from the same 0-7 range -- the
 * first is the file, the second is the rank (confirmed against real
 * hardware and the project author directly: keys 0-7 double up for both
 * file and rank entry, in sequence). Keys 8/9/10/12/14 are the J3 menu
 * entries above (undo/redo/board-view/level-select/options); 13/15 are
 * unhandled outside a submenu, same as an unrecognized key. */
static int tm_read_square(Position *pos, const char *prompt) {
    tm1638_display_string(prompt);
    int file = -1, rank = -1;
    while (file < 0 || rank < 0) {
        int k = tm_wait_key();
        if (k == TM_KEY_ABORT_CTRLC || k == TM_KEY_ABORT_STOPKEY) {
            /* Either one exits chess_run() from here -- there is no
             * "clear the current typing, keep waiting for FrOM" state to
             * back out to the way console.c's own line-buffer approach
             * has (:721-726), so STOP at this level means the same thing
             * Ctrl-C does. Menu STOP-cancel only applies inside a
             * submenu, handled separately above. */
            return TM_KEY_ABORT_CTRLC;
        }
        if (k == 8) { /* undo */
            if (pos->history_ply > 0) unmake_move(pos);
            tm_redraw_if_display(pos);
            return TM_KEY_RESTART;
        }
        if (k == 9) { /* redo */
            if (pos->history_ply < g_console_max_history_ply) {
                make_move(pos, pos->history[pos->history_ply].move);
            }
            tm_redraw_if_display(pos);
            return TM_KEY_RESTART;
        }
        if (k == 10) { /* board view */
            if (tm_board_view(pos) == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
            tm1638_display_string(prompt);
            continue;
        }
        if (k == 12) { /* level select */
            if (tm_level_select() == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
            tm1638_display_string(prompt);
            continue;
        }
        if (k == 14) { /* options menu */
            int r = tm_options_menu(pos);
            if (r == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
            if (r == TM_KEY_RESTART) return TM_KEY_RESTART;
            tm1638_display_string(prompt);
            continue;
        }
        if (k < 0 || k > 7) continue; /* 13/15, or -1 timeout */
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
 * move list, retrying on anything that doesn't resolve to a legal move.
 * Prompts for the promoting piece (J2's plan named this as J3's job
 * specifically -- a keypad-input concern, not outcome-detection) when
 * from/to only resolves to promotion moves, the same "1n2b3r4q" layout
 * console.c's own keypad picker uses (:651-677), defaulting to Queen on
 * an ambiguous or aborted choice rather than blocking on it a second
 * time. Returns 0 -- otherwise never a real return value here, since a
 * failed match falls through to "bAd MOuE" and retries -- if the read
 * was aborted (Ctrl-C or STOP at the top level); the sentinel chess_run()
 * below checks for to exit cleanly instead of treating it as an illegal
 * move. */
static Move tm_read_move(Position *pos) {
    for (;;) {
        int from = tm_read_square(pos, "FrOM    ");
        if (from == TM_KEY_ABORT_CTRLC) return 0;
        if (from == TM_KEY_RESTART) continue;
        int to = tm_read_square(pos, "tO      ");
        if (to == TM_KEY_ABORT_CTRLC) return 0;
        if (to == TM_KEY_RESTART) continue;

        MoveList list;
        generate_moves(pos, &list);

        bool only_promo = false;
        for (int i = 0; i < list.count; i++) {
            Move m = list.moves[i];
            if (MOVE_FROM(m) == from && MOVE_TO(m) == to && move_is_promo(m)) {
                only_promo = true;
                break;
            }
        }

        int promo_piece = NO_PIECE;
        if (only_promo) {
            tm1638_display_string("1n2b3r4q");
            int choice = tm_wait_key();
            if (choice == TM_KEY_ABORT_CTRLC || choice == TM_KEY_ABORT_STOPKEY) return 0;
            switch (choice) {
                case 0: promo_piece = KNIGHT; break;
                case 1: promo_piece = BISHOP; break;
                case 2: promo_piece = ROOK; break;
                default: promo_piece = QUEEN; break; /* 3, or anything unrecognized */
            }
        }

        Move chosen = 0;
        for (int i = 0; i < list.count; i++) {
            Move m = list.moves[i];
            if (MOVE_FROM(m) != from || MOVE_TO(m) != to) continue;
            if (move_is_promo(m)) {
                if (move_promo_piece(m) == promo_piece) { chosen = m; break; }
            } else if (!only_promo) {
                chosen = m;
                break;
            }
        }
        if (chosen != 0) {
            return chosen;
        }
        tm1638_display_string("bAd MOuE");
        time_delay_us(700000);
    }
}

#if CONFIG_ENABLE_DISPLAY
/* J2: TM1638 rendering of chess_game_outcome()/chess_in_check(), the same
 * literal 8-character display strings console.c itself uses
 * (:1049-1094) for the equivalent embedded-mode messages. Returns true if
 * the game just ended (caller freezes -- reset to play again, same shape
 * as the old undifferentiated "gAME OuEr" it replaces); false if play
 * continues, having already shown a brief "CHk     " if relevant. */
static bool tm_report_outcome(Position *pos) {
    switch (chess_game_outcome(pos)) {
        case CHESS_CHECKMATE_WHITE:
            tm1638_display_string("nAtE bL "); /* mate, black wins */
            draw_chess_status(pos, "checkmate: black wins", false);
            return true;
        case CHESS_CHECKMATE_BLACK:
            tm1638_display_string("nAtE UH "); /* mate, white wins ('U' for 'W') */
            draw_chess_status(pos, "checkmate: white wins", false);
            return true;
        case CHESS_STALEMATE:
        case CHESS_DRAW_REPETITION:
        case CHESS_DRAW_50MOVE:
            tm1638_display_string("drAU    ");
            draw_chess_status(pos, "draw", false);
            return true;
        default:
            if (chess_in_check(pos)) {
                tm1638_display_string("CHk     ");
                time_delay_us(700000);
            }
            return false;
    }
}

void chess_run(void) {
    if (!chess_ensure_init()) return;
    parse_fen(&g_chess_pos, STANDARD_START_FEN);
    g_console_max_history_ply = 0; /* J3: shared with the console REPL's
                                     * own undo/redo boundary (chess_ui.c:
                                     * 352), reset fresh for this session. */

    tm1638_display_string("LUgAL Ch");
    draw_chess_board(&g_chess_pos);
    draw_chess_status(&g_chess_pos, "", false);
    time_delay_us(1000000);

    char last_move_buf[6] = "";

    for (;;) {
        tm1638_display_string("YOUr MOu");
        Move human = tm_read_move(&g_chess_pos);
        if (human == 0) {
            /* Abort requested (Ctrl-C or the TM1638 STOP key) -- the
             * software exit path this persona lacked before J2: return
             * cleanly to the shell instead of needing a physical board
             * reset. Revises this function's own former "does not
             * return" contract now that there's a real way to ask it to. */
            tm1638_display_string("bYE     ");
            time_delay_us(700000);
            chess_session_end();
            return;
        }
        if (!make_move(&g_chess_pos, human)) {
            /* tm_read_move() only returns pseudo-legal moves from the real
             * move list, so make_move() only fails here on a king-safety
             * violation (a pin, moving into check) -- a real "no, that
             * specific move is illegal", not a parse failure. */
            tm1638_display_string("ILLEGAL ");
            time_delay_us(700000);
            continue;
        }
        g_console_max_history_ply = g_chess_pos.history_ply; /* J3: new
            move played -- the redo boundary advances, same as J1's own
            console_execute_move()/console_engine_reply() do. */
        format_move(human, last_move_buf);
        draw_chess_board(&g_chess_pos);
        draw_chess_status(&g_chess_pos, last_move_buf, false);

        if (tm_report_outcome(&g_chess_pos)) {
            for (;;) time_delay_us(1000000); /* game over: reset to play again */
        }

        tm1638_display_string("tHInKIng");
        draw_chess_status(&g_chess_pos, last_move_buf, true);
        /* J3: the level J1's console REPL and this menu's key-12 level
         * select both set (g_console_search_level) -- was a hardcoded
         * 5000ms before this milestone gave chess_run() a level concept
         * at all. */
        Move engine_move = chess_think(&g_chess_pos, 64, level_times_ms[g_console_search_level - 1]);
        if (engine_move == 0) {
            /* Defensive only -- tm_report_outcome() above already returned
             * false (ongoing, so a legal reply exists) before this search
             * ever started; a real hit here would mean engine/outcome
             * logic disagree, not an expected game state. */
            tm1638_display_string("gAME OuEr");
            draw_chess_status(&g_chess_pos, "no moves", false);
            for (;;) time_delay_us(1000000); /* game over: reset to play again */
        }
        make_move(&g_chess_pos, engine_move);
        g_console_max_history_ply = g_chess_pos.history_ply;
        format_move(engine_move, last_move_buf);
        tm1638_display_string(last_move_buf);
        draw_chess_board(&g_chess_pos);
        draw_chess_status(&g_chess_pos, last_move_buf, false);

        if (tm_report_outcome(&g_chess_pos)) {
            for (;;) time_delay_us(1000000); /* game over: reset to play again */
        }
    }
}
#endif /* CONFIG_ENABLE_DISPLAY */
#endif /* CONFIG_BOARD_RP2350 && CONFIG_ENABLE_TM1638 */
