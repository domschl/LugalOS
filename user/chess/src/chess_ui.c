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
#include "perft.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "kernel/line_editor.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "fs/vfs.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
#include "drivers/st7735.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
#include "drivers/tm1638.h"
#endif

#include "chess_ui.h"
#include "kernel/hart.h"
#include "pgn.h"
#include "kernel/scratch.h"

#define STANDARD_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

static bool g_chess_ready = false;
static Position g_chess_pos; /* static: ~8 KB (MAX_PLYS=256 history), never a stack local */

/* On-demand scratch Position, shared by tm_load() and tm_probe_pv() below
 * -- each needs a throwaway copy of a position to mutate (parsing a
 * candidate FEN before committing it; walking a few plies of a PV without
 * touching the real game). A second permanent ~8 KB static `Position`
 * for that (each of the two had its own before this) is exactly the kind
 * of avoidable heap cost [[heap_stateless_user_programs]] exists to catch
 * -- allocated in chess_ensure_init(), released in chess_session_end(),
 * same lifetime as search.c's own on-demand pools. Safe to share between
 * the two call sites: this is single-threaded, cooperative code, and
 * neither caller's use of it is ever nested inside the other's. */
static Position *g_chess_scratch = NULL;
static uint32_t g_chess_scratch_pages = 0;

/* The same scratch, reached from search.c (§1.3,
 * plan/phase15_memory_reclamation.md).
 *
 * search.c had a third permanent `static Position` of its own, for walking
 * the principal variation while printing it. Three such statics existed --
 * that one plus two in this file, one per FEN-parsing site -- costing 25 KB
 * of `.bss` between them, which on RP2350 is 6 pages taken straight out of
 * the heap (palloc_init() starts at _kernel_end). None of them is ever live
 * at the same time as any other, or as this one.
 *
 * Exposed through an accessor rather than the variable itself so the
 * allocate/free lifecycle stays wholly in chess_ensure_init()/
 * chess_session_end(), where it already was. Declared `extern` at search.c's
 * call site rather than in a header, matching exactly how
 * search_progress_callback()/search_poll_stop_callback() -- the only other
 * two things search.c reaches into this file for -- are already wired.
 *
 * ## Why it is safe for the engine and the UI to share one
 *
 * The overlap that would break this is search.c's PV walk running while
 * tm_probe_pv() below is mid-walk of its own, since both mutate the scratch.
 * It cannot happen. tm_probe_pv() is reached only from
 * search_poll_stop_callback() -> tm_search_ticker_tick(), and that callback
 * fires only from search.c's check_up_time(), i.e. only while pv_search()
 * or quiescence() is on the stack. search.c's PV walk runs between
 * iterative-deepening iterations, after pv_search() has returned for that
 * depth. Same task, no interrupt path into either, so the two are strictly
 * sequential.
 *
 * NULL before chess_ensure_init() and after chess_session_end(). Every
 * search_position() call is gated behind a successful chess_ensure_init()
 * (there is exactly one call site, chess_think() below, and all four entry
 * points into this file gate first), so search.c's use can never see NULL --
 * it checks anyway, because the cost of being wrong about that on a board is
 * a null dereference in the middle of a search. */
Position *chess_scratch_position(void) {
    return g_chess_scratch;
}

static Move g_search_best_move = 0;
static int g_search_score = 0;
static int g_search_depth = 0;
static int g_search_root_side = 0; /* pos->side snapshotted once at the
    start of chess_think() -- the live pos->side fluctuates during search
    (make_move/unmake_move on the same object, mid-recursion), so this is
    the only safe place for anything watching the search live (the TM1638
    ticker/TFT status line below) to read "which side is this actually
    for", needed to show scores from a consistent White POV regardless of
    who's on move. */

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
 * this in full. init_bitboards() is cheap and idempotent (a few thousand
 * static-array writes, no allocation), so redoing it on every session is not
 * worth special-casing around. init_zobrist() used to sit beside it and no
 * longer exists at all: its tables are const in flash now (section 3.2,
 * plan/phase15_memory_reclamation.md), so there is nothing to initialise. */
static bool chess_ensure_init(void) {
    if (g_chess_ready) return true;
    init_bitboards();
    init_tt(0);
    if (!search_pools_init()) {
        cprintf("chess: out of memory (search move-list pools)\n");
        return false;
    }
    g_chess_scratch_pages = ((uint32_t)sizeof(Position) + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    g_chess_scratch = (Position *)palloc_pages(g_chess_scratch_pages);
    if (g_chess_scratch == NULL) {
        cprintf("chess: out of memory (scratch position)\n");
        g_chess_scratch_pages = 0;
        search_pools_free();
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
    if (g_chess_scratch != NULL) {
        palloc_free(g_chess_scratch, g_chess_scratch_pages);
        g_chess_scratch = NULL;
        g_chess_scratch_pages = 0;
    }
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
        /* Consume the latch immediately -- it stays set until cleared
         * (kernel/console.c), and this is the only reader of it in chess_ui.c.
         * Leaving it set after reporting the abort here meant a Ctrl-C that
         * exited chess_run() from the idle FrOM prompt (not mid-search) was
         * still latched the next time (chess) ran, exiting it again on the
         * very first poll -- found live, on hardware, as the game silently
         * refusing to start a second time after a Ctrl-C exit. */
        console_interrupt_clear();
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

/* --- One session, two input devices (§"chess_next_event",
 *     plan/phase15_memory_reclamation.md) ---
 *
 * Before this, the keypad and the terminal were two separate programs over
 * one game: `(chess)` entered the TM1638 loop and `(chess-console)` the text
 * REPL, each with its own blocking wait, and neither could hear the other's
 * device. Playing on the board meant no way to type `level 6` or `save`
 * without leaving the session.
 *
 * They are now one loop with one event source. chess_next_event() polls
 * whichever devices exist and reports the first thing to happen:
 *
 *   CHESS_EVENT_KEY    a TM1638 keypad press (hardware only)
 *   CHESS_EVENT_LINE   a complete command line typed at the terminal
 *   CHESS_EVENT_ABORT  Ctrl-C, or the keypad's STOP key
 *   CHESS_EVENT_NONE   `timeout_ms` elapsed with nothing (never, if < 0)
 *
 * A line arrives through readline_poll() (kernel/line_editor.h), which is
 * the *same* editor readline_interactive() drives -- history, cursor keys and
 * all -- rather than a cut-down reader, so typing into a game feels like
 * typing at the shell. Lines are dispatched by console_dispatch_line(), so
 * every command means the same thing from either loop.
 *
 * Ordering here is deliberate: abort, then line, then key. Abort first
 * because a user reaching for Ctrl-C wants out now, not after whatever else
 * is queued. */
typedef enum {
    CHESS_EVENT_NONE = 0,
    CHESS_EVENT_KEY,
    CHESS_EVENT_LINE,
    CHESS_EVENT_ABORT,
} ChessEventKind;

typedef struct {
    ChessEventKind kind;
    int            key;    /* CHESS_EVENT_KEY */
    ChessAbort     abort;  /* CHESS_EVENT_ABORT */
    const char    *line;   /* CHESS_EVENT_LINE; owned by chess_ui.c */
} ChessEvent;

typedef enum { CHESS_CMD_OK = 0, CHESS_CMD_QUIT } ChessCmdResult;

/* --- One state change, every output (phase 16) ---
 *
 * The session has two input devices and up to three output devices, and until
 * now which outputs got updated depended on *which loop noticed the change*
 * rather than on the change itself. Typing a move at the terminal redrew the
 * ASCII board and left the TFT showing the previous position; playing one on
 * the keypad updated the TFT and 7-segment and left the terminal showing only
 * raw key codes. Both halves were correct in isolation and the combination was
 * unusable: whichever device you were not looking at was silently stale.
 *
 * So rendering hangs off the position changing, not off the input path. Every
 * site that alters g_chess_pos calls this, and it updates everything the board
 * actually has:
 *
 *   - the terminal's ASCII board, always;
 *   - the ST7735 board and status line, where one is built in;
 *   - the TM1638 move slots, recomputed from the position's own history
 *     (tm_sync_move_slots()) rather than from "the move we just made", so it
 *     is equally right after an undo, a redo, a `fen`, a load or a new game --
 *     none of which have a "move just made" to pass.
 *
 * The output side is now symmetric with the input side that phase 15's
 * chess_next_event() already made symmetric. */
static void chess_show(const Position *pos);

static ChessCmdResult console_dispatch_line(const char *line);

/* 300, matching what chess_console_run()'s own `line` buffer always was --
 * a FEN plus a level fits with room to spare. Static rather than a stack
 * local for the reason every other buffer in this file is: the deepest call
 * chain in the system runs through here. */
static char g_event_line[300];

/* The poll interval. 20 ms is tm_wait_key()'s own long-standing debounce
 * cadence, kept so keypad timing is unchanged by this rework; it is also far
 * below anything a typist notices. */
#define CHESS_EVENT_POLL_US 20000

static ChessEventKind chess_next_event(ChessEvent *ev, int timeout_ms,
                                       const char *prompt) {
    int waited_ms = 0;

    ev->kind = CHESS_EVENT_NONE;
    ev->key = -1;
    ev->abort = CHESS_ABORT_NONE;
    ev->line = NULL;

    for (;;) {
        ChessAbort a = chess_abort_requested();
        if (a != CHESS_ABORT_NONE) {
            ev->kind = CHESS_EVENT_ABORT;
            ev->abort = a;
            return ev->kind;
        }

        int n = readline_poll(prompt, g_event_line, (int)sizeof(g_event_line));
        if (n >= 0) {
            ev->kind = CHESS_EVENT_LINE;
            ev->line = g_event_line;
            return ev->kind;
        }

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
        int k = tm1638_get_key();
        if (k != -1) {
            ev->kind = CHESS_EVENT_KEY;
            ev->key = k;
            return ev->kind;
        }
#endif

        if (timeout_ms >= 0 && waited_ms >= timeout_ms) return CHESS_EVENT_NONE;

        /* Yield before sleeping. time_delay_us() busy-waits (it services
         * usb_cdc_task() in the spin, which is why it is still used here --
         * dropping it would stall USB console output during a wait), so
         * without this the loop would hold the CPU against the 9P server and
         * the driver tasks between polls. The blocking uart_getc() this
         * replaced in the console REPL parked in a sched_yield() loop of its
         * own, so yielding here keeps that behaviour rather than changing it. */
        sched_yield();
        time_delay_us(CHESS_EVENT_POLL_US);
        waited_ms += CHESS_EVENT_POLL_US / 1000;
    }
}

/* Only tm_wait_key() calls this, so it exists only where a keypad does --
 * otherwise it is an unused-function warning on both QEMU targets.
 *
 * Non-blocking: handle whatever the terminal has to say, and nothing else.
 * Used from the keypad debounce phases in tm_wait_key(), where a key press is
 * precisely what is being waited *out* rather than read, so KEY events are
 * left for the phase that wants them. Returns true if the session should end.
 */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
static bool chess_pump_console(const char *prompt) {
    ChessEvent ev;
    if (chess_next_event(&ev, 0, prompt) != CHESS_EVENT_LINE) return false;
    return console_dispatch_line(ev.line) == CHESS_CMD_QUIT;
}
#endif


#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
/* Defined further down, in the TM1638-only region (needs tm1638_display_
 * string() and friends) -- forward-declared here so search_poll_stop_
 * callback() below (shared by every front end, defined up here next to
 * chess_think()) can drive it without moving TM1638-only code out of its
 * own guarded region. */
static void tm_search_ticker_tick(void);
#endif

/* search.c's check_up_time() calls this every 2048 nodes -- a cooperative
 * poll already wired into the engine's inner loop. Sets `stop_search`,
 * which pv_search()/quiescence() already check on the way back out of
 * their own recursion. Either abort gesture stops a running search --
 * this one call site doesn't need to distinguish which.
 *
 * Also drives the TM1638/TFT live search ticker (design agreed with the
 * user 2026-08-12) -- the same 2048-node cadence the abort-check already
 * piggybacks on is the only reentry point available while search_
 * position() runs synchronously on this call stack, so the ticker's own
 * ~1 Hz wall-clock gate lives inside tm_search_ticker_tick() itself
 * rather than here. */
void search_poll_stop_callback(void) {
    if (chess_abort_requested() != CHESS_ABORT_NONE) {
        stop_search = true;
    }
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    tm_search_ticker_tick();
#endif
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

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
/* Uppercase, fixed 4 chars, no promo suffix -- the TM1638 ticker's own
 * move format (design agreed with the user 2026-08-12), distinct from
 * format_move() above (lowercase, promo-aware) which every other display
 * in this file already uses. Placed here, guarded, next to format_move()
 * rather than inside the TM1638-only region further below, since draw_
 * chess_status_thinking() (further down still, TM1638+DISPLAY-only) also
 * needs it and textually precedes that region. */
static void tm_format_move4(Move m, char *out) {
    int from = MOVE_FROM(m);
    int to = MOVE_TO(m);
    out[0] = (char)('A' + (from % 8));
    out[1] = (char)('1' + (from / 8));
    out[2] = (char)('A' + (to % 8));
    out[3] = (char)('1' + (to / 8));
    out[4] = '\0';
}

/* Compact White-POV score for the TM1638 search ticker and the TFT status
 * line (design agreed with the user 2026-08-12). Always shown as if White
 * is reading the eval, even while the engine is calculating for Black --
 * g_search_score itself is side-to-move-relative like every other score
 * this file prints (chess_ui.c:730's own comment), so `searching_side`
 * (g_search_root_side) un-flips it here. Range -99.9..+99.9 pawns
 * (clamped), no leading '+' on positive values; mate scores become
 * "M25"/"-M25" instead -- leading minus when White is the one getting
 * mated, mirroring search.c's own UCI "score mate -N" convention
 * (search.c:765-771).
 *
 * `out` must have room for 6 bytes (worst case "-99.9\0"). Right-aligned
 * within its 4 physical digit positions (found live to read as noticeably
 * more natural for a numeric field than the left-aligned first version)
 * -- built into a scratch buffer first since the total physical width
 * isn't known until the number itself is generated, then copied out with
 * leading-space padding. The '.' relies on tm1638_display_string()'s own
 * non-consuming decimal-point trick (drivers/tm1638_rp2350.c) --
 * physical digit *positions* actually consumed on the 7-segment display
 * (`phys` below) are tracked separately from characters written (`n`),
 * since the dot doesn't occupy one; the padding *count* has to use
 * `phys`, not the scratch buffer's raw length, for the same reason.
 * draw_chess_status() (TFT, no such physical-digit constraint, trims the
 * leading padding back off) reuses this verbatim anyway, for one shared
 * number format across both displays rather than two. */
static void tm_format_score_compact(int score, int searching_side, char *out) {
    char tmp[6];
    int n = 0, phys = 0;
    if (score >= MATE_VALUE - 1000) {
        int mate_moves = (INFINITY_VALUE - score + 1) / 2;
        if (mate_moves > 99) mate_moves = 99;
        bool white_delivers = (searching_side == WHITE);
        if (!white_delivers) { tmp[n++] = '-'; phys++; }
        tmp[n++] = 'M'; phys++;
        if (mate_moves >= 10) { tmp[n++] = (char)('0' + mate_moves / 10); phys++; }
        tmp[n++] = (char)('0' + mate_moves % 10); phys++;
    } else if (score <= -MATE_VALUE + 1000) {
        int mate_moves = (INFINITY_VALUE + score + 1) / 2;
        if (mate_moves > 99) mate_moves = 99;
        bool white_delivers = (searching_side != WHITE);
        if (!white_delivers) { tmp[n++] = '-'; phys++; }
        tmp[n++] = 'M'; phys++;
        if (mate_moves >= 10) { tmp[n++] = (char)('0' + mate_moves / 10); phys++; }
        tmp[n++] = (char)('0' + mate_moves % 10); phys++;
    } else {
        int white_cp = (searching_side == WHITE) ? score : -score;
        bool neg = white_cp < 0;
        int abs_cp = neg ? -white_cp : white_cp;
        if (abs_cp > 9990) abs_cp = 9990;
        int whole = abs_cp / 100;
        int tenth = (abs_cp / 10) % 10;
        if (neg) { tmp[n++] = '-'; phys++; }
        if (whole >= 10) { tmp[n++] = (char)('0' + whole / 10); phys++; }
        tmp[n++] = (char)('0' + whole % 10); phys++;
        tmp[n++] = '.';
        tmp[n++] = (char)('0' + tenth); phys++;
    }
    int op = 0;
    while (phys < 4) { out[op++] = ' '; phys++; }
    for (int i = 0; i < n; i++) out[op++] = tmp[i];
    out[op] = '\0';
}
#endif /* CONFIG_BOARD_RP2350 && CONFIG_ENABLE_TM1638 */

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
    g_search_root_side = pos->side; /* snapshot once -- pos->side itself
        fluctuates during search (make_move/unmake_move on this same
        object, mid-recursion), see this variable's own comment above. */
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
    chess_selftest_cores(1);
}

/* X8b: the same fixed-position benchmark, with the search allowed `cores`.
 * This is the measurement, not the game: one position, one time budget, and
 * a node count -- which is what makes 1 against 2 a comparison rather than an
 * impression. */
void chess_selftest_cores(int cores) {
    chess_selftest_bench(cores, 0);
}

/* `fixed_depth > 0` searches to exactly that depth with no time limit and
 * reports how long it took; 0 keeps the original 2-second budget.
 *
 * Time-to-depth is the measurement Lazy SMP is judged on, and the reason is
 * that the 2-second variant cannot see the thing being tested: at that budget
 * this position bottoms out at depth 8 either way, so both core counts score
 * the same and the number says nothing. A fixed depth turns "how deep in a
 * fixed time" -- which saturates -- into "how long to a fixed depth", which
 * does not. */
void chess_selftest_bench(int cores, int fixed_depth) {
    g_search_cores = (cores < 1) ? 1 : cores;
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

    uint64_t bench_start_ms = time_get_ms();
    Move best;
    if (fixed_depth > 0) {
        cprintf("chess: searching a midgame position to depth %d, no time limit...\n",
                fixed_depth);
        best = chess_think(&g_chess_pos, fixed_depth, -1);
    } else {
        cprintf("chess: searching a midgame position (2s)...\n");
        best = chess_think(&g_chess_pos, 64, 2000);
    }
    uint64_t bench_ms = time_get_ms() - bench_start_ms;

    if (best == 0) {
        cprintf("chess: no move found\n");
        chess_session_end();
        return;
    }
    char buf[6];
    format_move(best, buf);
    cprintf("chess: best move %s, score %d, depth %d, %ld nodes\n",
            buf, g_search_score, g_search_depth, nodes_searched);
    cprintf("chess: cores requested %d, harts online %u, helper nodes %ld, %lu ms\n",
            g_search_cores, smp_harts_online(), search_helper_nodes(),
            (unsigned long)bench_ms);
    g_search_cores = 1;   /* one session's setting must not leak into the next */
    /* One-shot benchmark, not an interactive session -- tears down like a
     * `quit` would (chess_console_run() below), so repeated
     * (chess-selftest) calls stay "stateless" rather than only the first
     * one paying the allocation cost forever after. */
    chess_session_end();
}

/* J4 (plan/phase10_chess_completion.md): perft.c's own move-generation
 * correctness suite, exposed the same no-hardware-dependency, heap-
 * stateless way chess_selftest() is above -- `max_depth <= 0` gets
 * run_perft_tests_depth()'s own documented default (5), matching
 * upstream's own `run_perft_tests()` convenience wrapper. Move generation
 * doesn't touch the transposition table or search's on-demand pools at
 * all, but chess_ensure_init() is still the right gate here rather than a
 * narrower one built just for this: it's the one place bitboard/zobrist
 * init (genuinely required, `generate_moves()`'s attack tables) already
 * happens, and every other chess entry point uses the same acquire/
 * release pair, so a session-boundary bug fixed once for the others is
 * fixed here too rather than needing its own copy to get right. */
void chess_perft(int max_depth) {
    chess_perft_cores(max_depth, 1);
}

/* X8: the same suite, split across `cores`. The session bracket is unchanged
 * -- workers share the bitboard/zobrist tables chess_ensure_init() builds,
 * which are read-only once built, so one init still covers every core. */
void chess_perft_cores(int max_depth, int cores) {
    if (!chess_ensure_init()) return;
    run_perft_tests_cores(max_depth, cores);
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
/* Games live in their own directory rather than the volume's system/ area
 * (14b): there are several of them now, and they are the user's data, not the
 * board's. SD when writable, RAM disk otherwise -- the latter is volatile, so
 * auto-save there lasts only until reboot, which still beats nothing on a
 * board with no card in it. */
static const char *console_save_dir(void) {
    return vfs_volume_writable("/sd0") ? "/sd0/chess" : "/ram0/chess";
}

/* Where `new` retires the outgoing game to. */
static const char *console_archive_dir(void) {
    return vfs_volume_writable("/sd0") ? "/sd0/chess/games" : "/ram0/chess/games";
}
/* The auto-saved game in progress. */
static const char *console_save_path(void) {
    return vfs_volume_writable("/sd0") ? "/sd0/chess/current.pgn"
                                        : "/ram0/chess/current.pgn";
}

/* Builds "<dir>/<name>.pgn". The name is bounded and may contain no path
 * separators or spaces, so it cannot address anything outside the games
 * directory. Returns false rather than silently sanitising: a name the user
 * typed and a different name being written is worse than a refusal. */
static bool console_named_path(const char *dir, const char *name, char *out, int max) {
    if (!name || !*name) return false;
    int n = 0;
    for (const char *q = dir; *q && n < max - 1; q++) out[n++] = *q;
    if (n < max - 1) out[n++] = '/';
    for (const char *q = name; *q; q++) {
        if (*q == '/' || *q == '\\' || *q == ' ' || n >= max - 6) return false;
        out[n++] = *q;
    }
    for (const char *q = ".pgn"; *q && n < max - 1; q++) out[n++] = *q;
    out[n] = '\0';
    return true;
}

static void console_path_current(char *out, int max) {
    int n = 0;
    for (const char *q = console_save_path(); *q && n < max - 1; q++) out[n++] = *q;
    out[n] = '\0';
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
/* Defined below with the rest of the save machinery; used here, one call site
 * earlier, because every completed move auto-saves (14b). */
static void console_autosave(Position *pos);

static void console_engine_reply(Position *pos, int max_depth, int time_limit_ms) {
    if (console_report_outcome(pos)) {
        return; /* game already over before the engine could reply */
    }
    Move best = chess_think(pos, max_depth, time_limit_ms);
    if (best == 0) {
        cprintf("Engine resigned or found no legal moves.\n");
        return;
    }
    /* SAN, like everywhere else the session names a move since 14b -- one
     * notation across both front ends and the PGN files, rather than the
     * terminal speaking long algebraic while the saved game speaks SAN.
     * Formatted before the move, which is what SAN requires. */
    char buf[SAN_MAX];
    format_move_san(pos, best, buf);
    make_move(pos, best);
    g_console_max_history_ply = pos->history_ply;
    cprintf("Engine plays: %s (Score: %s%d)\n", buf, sign_prefix(g_search_score), g_search_score);
    chess_show(pos);
    console_autosave(pos);   /* 14b: after every completed move */
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
    cprintf("  save [name]       - Save the game as PGN (no name: the auto-saved current game)\n");
    cprintf("  load [name]       - Load a PGN game (no name: the auto-saved current game)\n");
    cprintf("  games             - List saved and archived games\n");
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

/* Retires the auto-saved game before `new` lets it be overwritten (user's
 * own suggestion, and it closes a real hole: with auto-save on, the first
 * move of a new game would silently replace the previous one).
 *
 * Numbered, not timestamped. The RTC is optional on this board -- "No
 * DS1307/DS3231 RTC module found" is an ordinary boot line -- so a
 * timestamped filename would collide or read as the software clock's epoch on
 * a board without one. A counter derived from what is already in the
 * directory always works, and the real date still reaches the file: pgn_save()
 * writes it into the [Date] tag when an RTC is present, and "????.??.??" when
 * it is not.
 *
 * The file is copied rather than regenerated from the live position, so what
 * is archived is exactly what was saved -- including a game that was loaded
 * and never moved in. */
static void console_archive_current(void) {
    const char *dir = console_archive_dir();
    char cur[128];
    console_path_current(cur, (int)sizeof(cur));

    scratch_t sc;
    if (!scratch_acquire(&sc, 4096)) return;
    char *buf = (char *)sc.base;
    int len = vfs_read(cur, buf, 4095);
    if (len <= 0) { scratch_release(&sc); return; }   /* nothing to retire */

    vfs_mkdir(console_save_dir());
    vfs_mkdir(dir);

    /* First free slot. Bounded rather than unbounded: 999 archived games is
     * far past anything this board will see, and a loop with no end is worse
     * than a full archive. */
    char path[160];
    for (int n = 1; n <= 999; n++) {
        char name[16];
        int k = 0;
        name[k++] = 'g'; name[k++] = 'a'; name[k++] = 'm'; name[k++] = 'e'; name[k++] = '-';
        name[k++] = (char)('0' + (n / 100) % 10);
        name[k++] = (char)('0' + (n / 10) % 10);
        name[k++] = (char)('0' + n % 10);
        name[k] = '\0';
        if (!console_named_path(dir, name, path, (int)sizeof(path))) break;
        vfs_stat_t st;
        if (vfs_stat(path, &st) != 0) {          /* free slot */
            if (vfs_write(path, buf, (uint32_t)len) == 0) {
                cprintf("Previous game archived to %s\n", path);
            }
            break;
        }
    }
    scratch_release(&sc);
}

/* A fresh board, and nothing else. Split out from console_new_game() because
 * starting a *session* and asking for a *new game* are different things that
 * used to be the same call: the session start must not archive the game it is
 * about to resume. */
static void console_reset_game(Position *pos) {
    parse_fen(pos, STANDARD_START_FEN);
    clear_tt();
    g_console_max_history_ply = 0;
}

/* The `new` command: retire whatever was in progress, then reset. */
static void console_new_game(Position *pos) {
    console_archive_current();
    console_reset_game(pos);
}

/* Session start (14b). Auto-save is only half a promise without this -- a
 * board that saves every move and then boots to an empty position has kept
 * the game and hidden it. Resuming is what makes the pair worth having, and
 * it matters most on the persona that boots straight into chess with no shell
 * to type `load` at.
 *
 * Announced rather than silent: the board not being in the start position is
 * exactly the kind of surprise that needs one line of explanation, and the
 * line also says how to get a fresh board. */
static void console_resume_or_new(Position *pos) {
    char path[128];
    console_path_current(path, (int)sizeof(path));

    if (pgn_load(pos, path) == 0 && is_position_valid(pos) && pos->history_ply > 0) {
        clear_tt();
        g_console_max_history_ply = pos->history_ply;
        cprintf("Resumed game from %s (%d half-moves). 'new' starts a fresh game.\n",
                path, pos->history_ply);
        return;
    }
    /* No game, an empty one, or a file this build cannot read: start clean
     * without archiving -- there is nothing worth keeping in any of those
     * cases, and archiving here would fill the directory with empty games
     * every time the board powers on. */
    console_reset_game(pos);
}

/* The PGN result tag implied by the position itself, so a finished game is
 * archived as finished rather than as "*". */
static const char *console_result_tag(Position *pos) {
    static MoveList l;
    generate_moves(pos, &l);
    for (int i = 0; i < l.count; i++) {
        if (make_move(pos, l.moves[i])) { unmake_move(pos); return "*"; }
    }
    uint64_t kb = pos->piece_bbs[KING] & pos->color_bbs[pos->side];
    if (kb) {
        int ksq = 0; uint64_t t = kb;
        while (!(t & 1ULL)) { t >>= 1; ksq++; }
        if (is_square_attacked(pos, ksq, pos->side ^ 1)) {
            return (pos->side == WHITE) ? "0-1" : "1-0";
        }
    }
    return "1/2-1/2";
}

/* Every save route goes through here -- the explicit command, the keypad
 * menu, and the auto-save after each move -- so all three write the same
 * file. The mkdir is best-effort: vfs_write() does not create missing
 * parents, and a freshly formatted volume has none, so this either fails
 * harmlessly (it existed) or is what makes the write below succeed. */
static bool console_write_pgn(Position *pos, const char *path) {
    vfs_mkdir(console_save_dir());
    return pgn_save(pos, path, console_result_tag(pos)) == 0;
}

/* Auto-save (14b), after every completed move from either input device.
 *
 * Deliberately silent, in both directions: it does not announce success,
 * because a chess computer interrupting a game to say it saved is noise; and
 * it does not report failure, because a full or absent card must not stop the
 * board being a chess computer. Failure is still visible where it matters --
 * an explicit `save` reports properly. */
static void console_autosave(Position *pos) {
    (void)console_write_pgn(pos, console_save_path());
}

static void console_save(Position *pos, const char *name) {
    char path[128];
    if (name && *name) {
        if (!console_named_path(console_save_dir(), name, path, (int)sizeof(path))) {
            cprintf("save: bad name '%s' (no spaces or path separators)\n", name);
            return;
        }
    } else {
        console_path_current(path, (int)sizeof(path));
    }

    if (console_write_pgn(pos, path)) {
        cprintf("Game saved to %s\n", path);
    } else {
        cprintf("Save failed (could not write %s)\n", path);
    }
}

static void console_load(Position *pos, const char *name) {
    char path[128];
    if (name && *name) {
        if (!console_named_path(console_save_dir(), name, path, (int)sizeof(path))) {
            cprintf("load: bad name '%s'\n", name);
            return;
        }
    } else {
        console_path_current(path, (int)sizeof(path));
    }

    if (pgn_load(pos, path) != 0) {
        cprintf("No saved game at %s\n", path);
        return;
    }
    if (!is_position_valid(pos)) {
        cprintf("Save file is corrupt\n");
        return;
    }
    clear_tt();
    g_console_max_history_ply = pos->history_ply;
    cprintf("Game loaded from %s (%d half-moves)\n", path, pos->history_ply);
    chess_show(pos);
}

/* Lists the games directory and the archive, so `load <name>` has something
 * to name. */
static void console_list_games(void) {
    const char *dirs[2] = { console_save_dir(), console_archive_dir() };
    for (int d = 0; d < 2; d++) {
        int fd = vfs_open(dirs[d], VFS_O_READ);
        if (fd < 0) continue;
        cprintf("%s:\n", dirs[d]);
        char name[64];
        vfs_stat_t st;
        int shown = 0;
        for (uint32_t i = 0; vfs_readdir(fd, i, name, sizeof(name), &st) == 0; i++) {
            if (st.is_dir) continue;
            cprintf("  %s (%lu bytes)\n", name, (unsigned long)st.size);
            shown++;
        }
        if (!shown) cprintf("  (empty)\n");
        vfs_close(fd);
    }
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
/* One command surface, reachable from either input device.
 *
 * This was the body of chess_console_run()'s own `for (;;)`. Lifting it out
 * is what lets a typed command work while the TM1638 game loop is the thing
 * actually running -- chess_run() dispatches console lines through exactly
 * this function, so `level 4`, `fen ...`, `save` and a typed move behave
 * identically whether the keypad or the terminal is driving the session.
 * (§"chess_next_event", plan/phase15_memory_reclamation.md.)
 *
 * `quit` no longer returns out of the REPL directly; it reports
 * CHESS_CMD_QUIT and lets the caller decide what leaving means, since that
 * differs between the two loops -- the console REPL returns to `lsh`, the
 * keypad loop has hardware state to put back first. Note it deliberately
 * does NOT call chess_session_end() itself for the same reason. */
static ChessCmdResult console_dispatch_line(const char *line) {
    if (line[0] == '\0') {
        return CHESS_CMD_OK;
    } else if (strcmp(line, "help") == 0) {
        console_print_help();
    } else if (strcmp(line, "new") == 0) {
        console_new_game(&g_chess_pos);
        cprintf("New game started.\n");
        chess_show(&g_chess_pos);
    } else if (strcmp(line, "board") == 0 || strcmp(line, "d") == 0) {
        chess_show(&g_chess_pos);
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
            parse_fen(g_chess_scratch, p);
            if (!is_position_valid(g_chess_scratch)) {
                cprintf("Invalid FEN position (need exactly 1 White king, 1 Black king, valid check state).\n");
            } else {
                g_chess_pos = *g_chess_scratch;
                clear_tt();
                g_console_max_history_ply = g_chess_pos.history_ply;
                cprintf("Position loaded.\n");
                chess_show(&g_chess_pos);
            }
        }
    } else if (strncmp(line, "save", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        const char *nm = line + 4;
        while (*nm == ' ') nm++;
        console_save(&g_chess_pos, nm);
    } else if (strncmp(line, "load", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        const char *nm = line + 4;
        while (*nm == ' ') nm++;
        console_load(&g_chess_pos, nm);
    } else if (strcmp(line, "games") == 0) {
        console_list_games();
    } else if (strncmp(line, "go", 2) == 0 && (line[2] == '\0' || line[2] == ' ')) {
        console_handle_go(&g_chess_pos, line);
    } else if (strcmp(line, "undo") == 0) {
        if (g_chess_pos.history_ply > 0) {
            unmake_move(&g_chess_pos);
            cprintf("Took back 1 half-move.\n");
            chess_show(&g_chess_pos);
        } else {
            cprintf("Nothing to undo.\n");
        }
    } else if (strcmp(line, "redo") == 0) {
        if (g_chess_pos.history_ply < g_console_max_history_ply) {
            Move m = g_chess_pos.history[g_chess_pos.history_ply].move;
            if (make_move(&g_chess_pos, m)) {
                cprintf("Re-applied 1 half-move.\n");
                chess_show(&g_chess_pos);
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
        return CHESS_CMD_QUIT;
    } else if (console_execute_move(&g_chess_pos, line)) {
        g_console_max_history_ply = g_chess_pos.history_ply;
        chess_show(&g_chess_pos);
        console_autosave(&g_chess_pos);   /* 14b */
        if (!console_report_outcome(&g_chess_pos)) {
            console_engine_reply(&g_chess_pos, 64, level_times_ms[g_console_search_level - 1]);
        }
    } else {
        cprintf("Unknown command or invalid move: '%s'. Type 'help' for a list.\n", line);
    }
    return CHESS_CMD_OK;
}

void chess_console_run(void) {
    if (!chess_ensure_init()) return;
    console_resume_or_new(&g_chess_pos);

    cprintf("\nLugalOS chess console. Type 'help' for commands, 'quit' to leave.\n");
    print_board(&g_chess_pos);

    bool keypad_hint_shown = false;

    for (;;) {
        ChessEvent ev;
        /* Blocks until either device speaks. The keypad is polled here too,
         * so a board sitting next to the terminal stays live: pressing a
         * square registers as a move without having to leave this REPL. */
        switch (chess_next_event(&ev, -1, "chess> ")) {
        case CHESS_EVENT_LINE:
            if (console_dispatch_line(ev.line) == CHESS_CMD_QUIT) {
                chess_session_end();
                return;
            }
            break;
        case CHESS_EVENT_KEY:
            /* The keypad reached us, but this loop has no square-selection
             * state machine -- that lives in chess_run(), along with the
             * board display a keypad move is meaningless without. Rather
             * than half-implement it here, say so once and carry on; the
             * session is not disturbed. */
            if (!keypad_hint_shown) {
                cprintf("\n(keypad input needs the board UI -- leave with 'quit' "
                        "and start (chess) for keypad play; typed commands work "
                        "there too.)\n");
                keypad_hint_shown = true;
            }
            break;
        case CHESS_EVENT_ABORT:
            cprintf("\n");
            chess_session_end();
            return;
        default:
            break;
        }
    }
}

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
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

/* Appends the decimal digits of a non-negative int, no leading zeros
 * (except "0" itself) -- the one piece of manual formatting the two
 * status functions below need repeatedly, no snprintf on this
 * freestanding build (position.c's own header comment already
 * established why). Returns the new position. */
static int append_uint(char *buf, int pos, unsigned v) {
    char tmp[10];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) buf[pos++] = tmp[--n];
    return pos;
}

/* draw_chess_status() itself is defined further down, in the TM1638-only
 * region (needs g_console_search_level/tm_format_score_compact(), design
 * revised 2026-08-12 to also show level/score/depth, not just side-to-
 * move) -- append_uint() above stays here since draw_chess_board() (this
 * region) doesn't need it, but nothing else in this general DISPLAY-only
 * region does either any more, and it's needed before that point in the
 * file. */
#endif

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
/* -1 (already used below for "no key yet, keep polling") is not distinct
 * enough for "abort" -- tm_read_square()'s own retry loop already treats
 * any negative/out-of-range reading as "keep trying", which is correct
 * for a genuine timeout but wrong for a deliberate abort request, which
 * must unwind instead. Four sentinels, checked explicitly by every caller
 * in this chain:
 *   TM_KEY_ABORT_CTRLC -- the universal panic button, propagates out of
 *     chess_run() entirely from anywhere, including from inside a submenu
 *     or an in-progress move entry.
 *   TM_KEY_ABORT_STOPKEY -- chess_abort_requested()'s other gesture, the
 *     physical STOP key. **Revised, found live on hardware:** STOP used
 *     to mean "exit chess_run()" when pressed while simply waiting for a
 *     move, the same as Ctrl-C -- this created a real race the user
 *     flagged after testing: STOP is also what force-stops a running
 *     search, so a human pressing STOP to interrupt a slow search, just
 *     as the search finishes on its own, could instead find themselves
 *     unexpectedly thrown out of the whole game. STOP now never exits
 *     chess_run() -- it only ever aborts whatever's in progress (a
 *     partial move entry, a submenu) back to normal play, exactly like it
 *     already did inside board-view/level-select/options-menu. Leaving
 *     the game deliberately via the keypad is now the options menu's own
 *     EXIT item (TM_KEY_EXIT_GAME) instead.
 *   TM_KEY_RESTART -- the position changed underneath the current move
 *     attempt (undo/redo/new-game/load, all below), so tm_read_move()
 *     should re-prompt from FrOM rather than continue asking for a square
 *     that no longer makes sense against the new position.
 *   TM_KEY_EXIT_GAME -- the options menu's EXIT item: a deliberate,
 *     unambiguous request to leave chess_run(), propagated the same way
 *     TM_KEY_ABORT_CTRLC is once it reaches tm_read_move(), but kept a
 *     distinct sentinel rather than reusing TM_KEY_ABORT_CTRLC so nothing
 *     downstream has to guess which gesture actually asked for it.
 *   TM_KEY_GO -- key 13 (added live, user request 2026-08-12): skip
 *     entering a move for the current side, let the engine play it
 *     instead. Pressed repeatedly, one press per turn, it turns
 *     chess_run() into a self-play viewer -- each press only ever
 *     substitutes for *this* side's input, the engine already plays
 *     every move on the other side automatically regardless. */
/* The prompt the terminal shows while the board UI has the session. Distinct
 * from chess_console_run()'s "chess> " so it is obvious which loop is live
 * and that the keypad is also active. */
#define TM_CONSOLE_PROMPT "chess[board]> "

#define TM_KEY_ABORT_CTRLC   (-2)
#define TM_KEY_ABORT_STOPKEY (-3)
#define TM_KEY_RESTART        (-4)
#define TM_KEY_EXIT_GAME      (-5)
#define TM_KEY_GO              (-6)
/* tm_read_move()'s own sentinel, distinct from the int key-codes above --
 * a Move (uint16_t, MOVE_FROM/MOVE_TO packed into bits 0-5/6-11, defs.h)
 * where from == to == 63 can never be a real generated move (a move
 * always goes somewhere else), so it's safe to use as an out-of-band
 * "the human asked the engine to move instead" signal without an extra
 * output parameter. */
#define TM_MOVE_GO ((Move)0xFFFF)

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

    /* The per-keypress traces this used to print are gone (phase 16). They were
     * scaffolding from when the key protocol itself was being worked out --
     * H4 found the "hang" reports that way -- and reading them cost nothing
     * while the terminal was a debugging channel. It is now the session's
     * other half: four "raw key=N" lines per move, interleaved with the board
     * a human is reading, is noise where the move itself is the news. What a
     * completed move actually was is announced by the caller instead, in
     * algebraic notation.
     *
     * This one function is every keypad wait in the board UI -- tm_read_move()
     * and each of J3's menus reach the hardware only through here. That is
     * what makes terminal commands work across the whole board loop from a
     * single change: the console is pumped in all three phases below, so a
     * typed `level 6` or `save` lands whether the UI is between moves, mid
     * square-selection, or inside a menu. (§"chess_next_event",
     * plan/phase15_memory_reclamation.md.) */
    iters = 0;
    while (tm1638_get_key() != -1) {
        ChessAbort a = chess_abort_requested();
        if (a == CHESS_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
        if (a == CHESS_ABORT_STOPKEY) return TM_KEY_ABORT_STOPKEY;
        if (chess_pump_console(TM_CONSOLE_PROMPT)) return TM_KEY_ABORT_CTRLC;
        time_delay_us(20000);
        if (++iters > 750) break;   /* stuck key: scan anyway rather than hang */
    }

    /* The press phase is the event loop proper: whichever device speaks
     * first wins. A typed command is dispatched and the wait resumes, so
     * issuing one does not cost the user the move they were about to make. */
    iters = 0;
    int key = -1;
    int pressed_key = -1;
    for (;;) {
        ChessEvent ev;
        switch (chess_next_event(&ev, 20, TM_CONSOLE_PROMPT)) {
        case CHESS_EVENT_KEY:
            pressed_key = ev.key;
            goto pressed;
        case CHESS_EVENT_LINE:
            if (console_dispatch_line(ev.line) == CHESS_CMD_QUIT) {
                return TM_KEY_ABORT_CTRLC;
            }
            iters = 0;  /* the user is present and typing; don't time out */
            continue;
        case CHESS_EVENT_ABORT:
            return (ev.abort == CHESS_ABORT_STOPKEY) ? TM_KEY_ABORT_STOPKEY
                                                     : TM_KEY_ABORT_CTRLC;
        default:
            break;
        }
        if (++iters > 750) return -1;   /* nobody is pressing anything */
    }
pressed:
    key = pressed_key;

    iters = 0;
    while (tm1638_get_key() != -1) {
        (void)chess_pump_console(TM_CONSOLE_PROMPT);
        time_delay_us(20000);
        if (++iters > 750) break;   /* stuck key: stop waiting for a release */
    }
    return key;
}

/* --- Live search ticker + persistent two-slot display (design agreed
 * with the user 2026-08-12, revised same day to be color-based rather
 * than human/engine-based -- the "human always plays the left slot,
 * engine always plays the right" convention only made sense back when
 * the human could only ever play White against an engine-only Black;
 * once "go" (below) lets the engine play *either* side on demand, the
 * slots have to mean something that stays true regardless of who's
 * controlling which color). Two persistent 4-char slots: chars 0-3 are
 * always White's last move (or an entry cursor while White is being
 * decided, by human or engine), chars 4-7 are always Black's, the same
 * way a real board's move-pair notation works. Whichever side is
 * currently being decided shows a cursor (human typing) or alternates
 * between its current best move and White-POV score once a second (the
 * engine thinking, "go" or fully automatic), using exactly the same
 * g_search_best_move/g_search_score/g_search_root_side data the console
 * REPL's own `eval`/`go` output already reads); the other side's slot
 * stays frozen throughout.
 * Declared here (before tm_new_game()/tm_load() below, both of which
 * need to resync the slots) rather than down by the ticker function
 * itself, which needs them too but is fine reading them forward. --- */
static char g_tm_white_slot[5] = "    "; /* White's last completed move,
    or blank before White has moved yet. Kept in sync with the
    position's own history by tm_sync_move_slots() below -- found live
    to matter, not just theoretical: new game and undo/redo used to
    leave the (then single) engine slot showing whatever the engine last
    actually searched, which after a new game or several moves back was
    a stale move from a previous game entirely. */
static char g_tm_black_slot[5] = "    "; /* Black's last completed move,
    or blank before Black has moved yet. Same sync discipline as White's. */
static char g_tm_frozen_slot[5] = "    "; /* whichever of the two slots
    above is *not* being calculated right now -- set by chess_run() right
    before each chess_think() call, read by the ticker below since
    search_poll_stop_callback() runs deep inside search.c with no context
    to pass this through as a parameter. */
static int g_tm_active_half = 0; /* 0 = White's slot (chars 0-3) is the
    one being calculated this search, 1 = Black's (chars 4-7). Set
    alongside g_tm_frozen_slot, same reason. */
static uint64_t g_tm_ticker_last_ms = 0;
static bool g_tm_ticker_show_score = false;

/* Recomputes both slots from the position's own move history -- "the
 * last move played by each color", which after undo/redo/new-game/load
 * is not necessarily either side's last *search* result any more, it's
 * whatever moves are now actually last in the position for each color.
 * pos->history[ply] stores the move that connects ply -> ply+1
 * (position.c's make_move()/unmake_move() both index it this way), and
 * color strictly alternates starting from White at ply 0, so scanning
 * backward from the current ply finds each side's most recent move in
 * at most two steps in the common case. Called everywhere the position
 * changes out from under the display: undo, redo, new game, load. */
static void tm_sync_move_slots(const Position *pos) {
    bool have_white = false, have_black = false;
    for (int i = pos->history_ply - 1; i >= 0 && !(have_white && have_black); i--) {
        bool mover_is_white = (i % 2 == 0);
        if (mover_is_white && !have_white) {
            tm_format_move4(pos->history[i].move, g_tm_white_slot);
            have_white = true;
        } else if (!mover_is_white && !have_black) {
            tm_format_move4(pos->history[i].move, g_tm_black_slot);
            have_black = true;
        }
    }
    if (!have_white) for (int i = 0; i < 4; i++) g_tm_white_slot[i] = ' ';
    if (!have_black) for (int i = 0; i < 4; i++) g_tm_black_slot[i] = ' ';
}


#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
/* Defined further down (needs g_console_search_level/tm_format_score_
 * compact()/tm_probe_pv(), all declared later) -- forward-declared here
 * so tm_redraw_if_display() below can call it. */
static void draw_chess_status(const Position *pos, const char *last_move, bool thinking);
#endif

/* Redraws the ST7735 board/status if this build actually has one --
 * tm_read_square()/the menu functions below live in the TM1638-only
 * guard, which must keep compiling on a TM1638-without-display board
 * (H3's three independent flags), so this is the one place that
 * optionality is handled rather than repeating the #if at every call
 * site that wants a redraw after undo/redo/new-game/load. */
static void tm_redraw_if_display(const Position *pos) {
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    draw_chess_board(pos);
    draw_chess_status(pos, "", false);
#else
    (void)pos;
#endif
}


/* Resets to a fresh game -- shared by the options menu's own "new game"
 * item and chess_run()'s game-over screen (STOP there now starts a new
 * game rather than exiting, matching STOP never being the way to leave
 * chess_run(), see the sentinel block's comment above), so there's one
 * definition of what "new game" actually resets rather than two. */
static void tm_new_game(Position *pos) {
    /* Through console_new_game(), so the keypad archives the outgoing game
     * exactly as the console's `new` does (14b). It did not, which left the
     * board's own new-game key as the one route that could still discard a
     * game auto-save had been carefully keeping. */
    console_new_game(pos);
    cprintf("New game started.\n");
    chess_show(pos);
}

#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
/* Reconstructs a short PV starting from `first_move` (the caller's own
 * already-known, reliable root move -- g_search_best_move, tracked
 * directly via search_progress_callback(), not TT-dependent), then
 * extends it by walking the transposition table the same way search.c's
 * own UCI "pv" line does internally (search.c:777-800), redone here
 * since search.c never exposes its PV as data, only prints it.
 *
 * Found live to matter, not just theoretical: probing the TT for the
 * *root* move too (the first version's approach) intermittently returned
 * nothing at all after a while -- tt.c's table is small (32 KB,
 * LUGALCHESS_EMBEDDED) and direct-mapped with no collision chaining, so
 * a long session (several searches deep, more so under "go" self-play)
 * can evict even the root position's own entry via an unrelated hash
 * collision, which read as the whole PV display randomly going blank.
 * Taking the root move directly instead means the PV can never show
 * zero moves while a search is genuinely in progress; only the extension
 * beyond the first move is still TT-probed and can legitimately dead-end
 * early, which is normal, not an error. Writes up to `max_moves`
 * uppercase 4-char move strings (tm_format_move4()'s own convention, the
 * same one the TM1638 side of this ticker uses -- matching it here too
 * rather than format_move()'s lowercase, per the user's own example)
 * into `out`, returns the count actually found. Uses the shared
 * on-demand scratch Position (g_chess_scratch) rather than a second
 * permanent static -- safe since chess_run()'s own call stack never
 * re-enters this while a previous call is still using it (single-
 * threaded, cooperative code). */
static int tm_probe_pv(const Position *pos, Move first_move, char out[][6], int max_moves) {
    if (first_move == 0 || max_moves <= 0) return 0;
    *g_chess_scratch = *pos;
    int n = 0;
    tm_format_move4(first_move, out[n++]);
    if (!make_move(g_chess_scratch, first_move)) return n;
    while (n < max_moves) {
        int score;
        Move m;
        if (!probe_tt_entry(g_chess_scratch->hash_key, &score, &m) || m == 0) break;
        if (!make_move(g_chess_scratch, m)) break;
        tm_format_move4(m, out[n]);
        n++;
    }
    return n;
}

/* Status line, both idle and live-during-search -- one function instead
 * of two (design revised 2026-08-12: the first version's live-only
 * variant left line 1 blank once a move was actually made, since it only
 * ever read score/depth while chess_think() was still running; those are
 * exactly the values it leaves behind once it returns too, so reading
 * them unconditionally covers idle just as well as thinking, no separate
 * "last completed" bookkeeping needed).
 *
 * Line 1: "Lv <level>/<last completed depth> Sc <White-POV score>
 * <side to move, or 'Thinking...'>". Line 2: idle shows "<move number>.
 * <last move>"; while thinking, "<move number>. ... <PV moves>" instead,
 * walking the TT (tm_probe_pv() above) for a short principal variation --
 * up to 3 plies, trimmed to whatever fits the line's buffer, per the
 * user's own "if that fits, otherwise less PV depth" framing. `last_move`
 * is ignored while thinking (the PV replaces it) -- callers may pass
 * NULL there. */
static void draw_chess_status(const Position *pos, const char *last_move, bool thinking) {
    st7735_draw_rect(0, 128, 128, 32, ST7735_BLACK);
    st7735_draw_rect(0, 127, 128, 1, ST7735_GRAY);

    char line1[40];
    int p = 0;
    line1[p++] = 'L'; line1[p++] = 'v'; line1[p++] = ' ';
    p = append_uint(line1, p, (unsigned)g_console_search_level);
    line1[p++] = '/';
    p = append_uint(line1, p, (unsigned)g_search_depth);
    line1[p++] = ' '; line1[p++] = 'S'; line1[p++] = 'c'; line1[p++] = ' ';
    char score_buf[6];
    tm_format_score_compact(g_search_score, g_search_root_side, score_buf);
    char *trimmed = score_buf;
    while (*trimmed == ' ') trimmed++; /* tm_format_score_compact()'s own
        padding right-aligns for the TM1638's fixed 4-digit slot -- not
        wanted here, the TFT just wants the number. */
    for (char *c = trimmed; *c; c++) line1[p++] = *c;
    line1[p++] = ' ';
    const char *tail = thinking ? "Thinking..." : ((pos->side == WHITE) ? "White" : "Black");
    for (const char *c = tail; *c; c++) line1[p++] = *c;
    line1[p] = '\0';
    st7735_draw_string(2, 131, line1, thinking ? ST7735_CYAN : ST7735_YELLOW, 1);

    /* "12. <moves>" for White (period, no dots) vs "12 ... <moves>" for
     * Black (space + ellipsis, no period) while thinking -- standard
     * chess move-pair notation (Black's half of a move pair is written
     * "N..." rather than repeating "N."), per the user's own example;
     * idle mode (a specific last move, not a PV) always uses the plain
     * "N. " form regardless of which color just moved. */
    char line2[40];
    int q = 0;
    q = append_uint(line2, q, (unsigned)(pos->history_ply / 2 + 1));
    if (thinking && g_search_root_side != WHITE) {
        line2[q++] = ' '; line2[q++] = '.'; line2[q++] = '.'; line2[q++] = '.'; line2[q++] = ' ';
    } else {
        line2[q++] = '.'; line2[q++] = ' ';
    }
    if (thinking) {
        char pv[3][6];
        int n = tm_probe_pv(pos, g_search_best_move, pv, 3);
        for (int i = 0; i < n && q < 34; i++) {
            for (char *c = pv[i]; *c && q < 38; c++) line2[q++] = *c;
            if (i < n - 1 && q < 38) line2[q++] = ' ';
        }
    } else if (last_move && last_move[0] != '\0') {
        for (const char *c = last_move; *c && q < 38; c++) line2[q++] = *c;
    }
    line2[q] = '\0';
    st7735_draw_string(2, 145, line2, ST7735_WHITE, 1);
}
#endif /* CONFIG_BOARD_RP2350 && CONFIG_ENABLE_ST7735 */

/* Called from search_poll_stop_callback() -- already invoked every 2048
 * nodes, the same cadence the abort-check itself piggybacks on. Gated by
 * a real wall-clock interval rather than the node cadence (which varies
 * with search speed and position complexity), so the alternation reads
 * as a steady ~1 Hz blink regardless of how fast the position searches.
 * A no-op until the first depth completes (g_search_best_move == 0) --
 * there's nothing real to show yet, and the blank active-slot state set
 * the moment calculation starts (chess_run() below) already covers that
 * gap on screen.
 *
 * Formats into an isolated scratch buffer first, whose *text* length
 * varies (tm_format_score_compact() is 4-6 raw chars for always-4
 * physical digit positions, the '.' trick; tm_format_move4() is always
 * exactly 4) -- concatenated with the frozen slot's own fixed 4 raw
 * chars in the right order for whichever side is active, using the
 * scratch buffer's *actual* length (strlen(), not a hardcoded 4).
 *
 * Found live, a second time: an earlier version of this copied a fixed
 * 4 raw bytes out of the scratch buffer regardless of its real length,
 * which truncated the tenths digit off every score that used the '.'
 * trick -- "  0. " instead of "  0.7" -- since that trick means the
 * *physical* 4-position score is routinely 5 raw characters, not 4.
 * g_tm_active_half being able to put this variable-length write on
 * *either* side of `disp` (color-based slots, not human/engine-based,
 * see the sentinel block above) is also why this can't just always
 * write first and rely on a fixed trailing offset the way the original
 * single-direction ticker could. */
static void tm_search_ticker_tick(void) {
    if (g_search_best_move == 0) return;
    uint64_t now = time_get_ms();
    if (now - g_tm_ticker_last_ms < 1000) return;
    g_tm_ticker_last_ms = now;
    g_tm_ticker_show_score = !g_tm_ticker_show_score;

    char active4[8];
    if (g_tm_ticker_show_score) {
        tm_format_score_compact(g_search_score, g_search_root_side, active4);
    } else {
        tm_format_move4(g_search_best_move, active4);
    }
    int active_len = (int)strlen(active4);
    char disp[16];
    int pos = 0;
    if (g_tm_active_half == 0) {
        for (int i = 0; i < active_len; i++) disp[pos++] = active4[i];
        for (int i = 0; i < 4; i++) disp[pos++] = g_tm_frozen_slot[i];
    } else {
        for (int i = 0; i < 4; i++) disp[pos++] = g_tm_frozen_slot[i];
        for (int i = 0; i < active_len; i++) disp[pos++] = active4[i];
    }
    disp[pos] = '\0';
    tm1638_display_string(disp);
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    draw_chess_status(&g_chess_pos, NULL, true);
#endif
}

/* Alternates one 4-char half of an 8-char display between whatever's
 * already there (the move that just happened) and a 4-char annotation
 * word ("CHk ", "MAtE", a promotion piece name, ...) -- the single
 * mechanism behind every "something notable happened" moment, replacing
 * what used to be several different one-off full-8-char freeze screens
 * with one that keeps the actual move visible instead of hiding it
 * behind a fixed message. `half` is 0 for the human's slot (chars 0-3),
 * 1 for the engine's (chars 4-7) -- whichever slot the annotated move
 * landed in; the other half of `base8` is left untouched throughout.
 *
 * Flashes `cycles` times (~1.4s each); if `end_on_word` the display is
 * left showing the word rather than the move once done. For checkmate/
 * stalemate/draw, tm_wait_game_over() takes over afterward with its own
 * indefinite (but static, not alternating) wait -- a deliberate scope
 * choice, not an oversight: keeping a smooth ~1 Hz alternation going
 * *while also* waiting indefinitely on a keypress would need tm_wait_
 * key()'s own coarser polling loop restructured, which this round
 * doesn't attempt. A few flashes are enough for a human to read both
 * pieces of information before the display settles. */
static void tm_show_move_annotation(char *base8, int half, const char *word4,
                                     int cycles, bool end_on_word) {
    int off = half * 4;
    char saved[5];
    for (int i = 0; i < 4; i++) saved[i] = base8[off + i];
    saved[4] = '\0';
    for (int c = 0; c < cycles; c++) {
        for (int i = 0; i < 4; i++) base8[off + i] = word4[i];
        tm1638_display_string(base8);
        time_delay_us(700000);
        for (int i = 0; i < 4; i++) base8[off + i] = saved[i];
        tm1638_display_string(base8);
        time_delay_us(700000);
    }
    if (end_on_word) {
        for (int i = 0; i < 4; i++) base8[off + i] = word4[i];
        tm1638_display_string(base8);
    }
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
            /* Worded exactly as the console's own `level` command, so a
             * session driven from both devices reads as one transcript
             * whichever one changed the setting (phase 16). */
            if (level_times_ms[k] != -1) {
                cprintf("Search level set to %d (%ds per move).\n",
                        k + 1, level_times_ms[k] / 1000);
            } else {
                cprintf("Search level set to 8 (Infinite -- press Ctrl-C to stop it).\n");
            }
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
static void tm_save(Position *pos) {
    /* PGN, through the same writer the terminal uses (14b) -- the keypad and
     * the console must not produce different files for the same game. The
     * keypad deliberately has no *named* saves: an eight-character
     * seven-segment display is a poor file picker, so this writes the one
     * current game and `save <name>` at a terminal does the rest. */
    bool ok = console_write_pgn(pos, console_save_path());
    cprintf(ok ? "Game saved to %s\n" : "Save failed (could not write %s)\n",
            console_save_path());
    tm1638_display_string(ok ? "SAuEd   " : "nO SAuE ");
    time_delay_us(1500000);
}

/* Returns TM_KEY_RESTART on a successful load (the position changed under
 * the caller), 0 otherwise (nothing to load, or a corrupt save file). */
static int tm_load(Position *pos) {
    if (pgn_load(g_chess_scratch, console_save_path()) != 0) {
        tm1638_display_string("nO SAuE ");
        time_delay_us(1500000);
        return 0;
    }
    if (!is_position_valid(g_chess_scratch)) {
        tm1638_display_string("bAd SAuE");
        time_delay_us(1500000);
        return 0;
    }
    *pos = *g_chess_scratch;
    clear_tt();
    g_console_max_history_ply = pos->history_ply;
    cprintf("Game loaded from %s (%d half-moves)\n",
            console_save_path(), pos->history_ply);
    tm1638_display_string("LOAdEd  ");
    time_delay_us(1500000);
    tm_sync_move_slots(pos);
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
#define TM_OPTION_COUNT 9
static const char *tm_option_names[TM_OPTION_COUNT] = {
    "nEU gAnE", "ScOrE   ", "SIdES   ", "HAlF    ", "MOuES   ", "SAuE    ", "LOAd    ", "AUtO    ", "End     "
};

/* Whether a human-typed move automatically triggers one engine reply for
 * the other side (design agreed with the user 2026-08-12, after "go"
 * alone turned out ambiguous about this). ON (default) is the classic
 * "you move, computer replies" experience -- ply N is human-typed, ply
 * N+1 is engine-computed with no separate prompt or "go" press. OFF
 * requires "go" for *every* move, either color -- useful for entering a
 * whole known sequence by hand (replaying a game, setting up a specific
 * position) without the engine jumping in after the very first
 * half-move. "go" itself never auto-chains regardless of this setting --
 * only ever plays the one turn it was pressed for -- otherwise it could
 * play both sides in an unstoppable loop with nothing to break it. */
static bool g_tm_auto_reply = true;

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
                    tm_new_game(pos);
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
                case 7: /* auto-reply toggle */
                    g_tm_auto_reply = !g_tm_auto_reply;
                    cprintf("Auto-reply %s.\n", g_tm_auto_reply ? "on" : "off");
                    tm1638_display_string(g_tm_auto_reply ? "AUtO On " : "AUtO OFF");
                    time_delay_us(2000000);
                    tm1638_display_string(tm_option_names[idx]);
                    break;
                case 8: /* exit -- the deliberate, keypad-only way to leave
                         * chess_run() now that STOP no longer does (see
                         * TM_KEY_EXIT_GAME's own comment above) */
                    return TM_KEY_EXIT_GAME;
            }
        }
    }
}

/* One square takes two key presses, both from the same 0-7 range -- the
 * first is the file, the second is the rank (confirmed against real
 * hardware and the project author directly: keys 0-7 double up for both
 * file and rank entry, in sequence). Keys 8/9/10/12/13/14 are the J3
 * menu entries above (undo/redo/board-view/level-select/go/options); 15
 * is unhandled outside a submenu, same as an unrecognized key.
 *
 * Reads one square into disp[slot_offset] and disp[slot_offset+1],
 * showing a '_' cursor at the position not yet entered and redrawing
 * the shared 8-char display (disp[9], owned by tm_read_move() below --
 * chars 0-3 are White's slot, 4-7 are Black's, `slot_offset` picks
 * whichever one is actually to move) after every digit. Both halves
 * stay visible throughout entry this way (design agreed with the user
 * 2026-08-12), unlike the old "FrOM"/"tO" prompt labels this replaces,
 * which each overwrote the other's square the instant the second one
 * started. */
static int tm_read_square(Position *pos, char *disp, int slot_offset) {
    int file = -1, rank = -1;
    disp[slot_offset] = '_';
    tm1638_display_string(disp);
    while (file < 0 || rank < 0) {
        int k = tm_wait_key();
        if (k == TM_KEY_ABORT_CTRLC) {
            return TM_KEY_ABORT_CTRLC;
        }
        if (k == TM_KEY_ABORT_STOPKEY) {
            /* STOP never exits chess_run() (revised, see the sentinel
             * block's own comment above) -- it aborts whatever's in
             * progress, same as everywhere else STOP appears. There is no
             * "clear the current typing, keep waiting" state to back out
             * to the way console.c's own line-buffer approach has
             * (:721-726), so this restarts the whole move entry from
             * FrOM rather than trying to preserve a half-entered square. */
            return TM_KEY_RESTART;
        }
        if (k == 8) { /* undo */
            if (pos->history_ply > 0) unmake_move(pos);
            tm_sync_move_slots(pos);
            tm_redraw_if_display(pos);
            return TM_KEY_RESTART;
        }
        if (k == 9) { /* redo */
            if (pos->history_ply < g_console_max_history_ply) {
                make_move(pos, pos->history[pos->history_ply].move);
            }
            tm_sync_move_slots(pos);
            tm_redraw_if_display(pos);
            return TM_KEY_RESTART;
        }
        if (k == 10) { /* board view */
            if (tm_board_view(pos) == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
            tm1638_display_string(disp);
            continue;
        }
        if (k == 12) { /* level select */
            if (tm_level_select() == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
            tm1638_display_string(disp);
            continue;
        }
        if (k == 14) { /* options menu */
            int r = tm_options_menu(pos);
            if (r == TM_KEY_ABORT_CTRLC) return TM_KEY_ABORT_CTRLC;
            if (r == TM_KEY_RESTART) return TM_KEY_RESTART;
            if (r == TM_KEY_EXIT_GAME) return TM_KEY_EXIT_GAME;
            tm1638_display_string(disp);
            continue;
        }
        if (k == 13) { /* go -- let the engine play this side */
            return TM_KEY_GO;
        }
        if (k < 0 || k > 7) continue; /* 15, or -1 timeout */
        if (file < 0) {
            file = k;
            disp[slot_offset] = (char)('A' + file);
            disp[slot_offset + 1] = '_';
        } else {
            rank = k;
            disp[slot_offset + 1] = (char)('1' + rank);
        }
        tm1638_display_string(disp);
    }
    return rank * 8 + file;
}

/* Prompts for from/to squares and resolves them against the actual legal
 * move list, retrying on anything that doesn't resolve to a legal move.
 * Builds and owns the shared 8-char persistent display for the whole
 * attempt: whichever color is actually to move (pos->side) gets the
 * entry cursor in its own slot (0-3 for White, 4-7 for Black -- a real
 * board's move-pair convention, not a human/engine one, see the
 * sentinel block's own comment above), the other color's slot stays
 * fixed on its own last completed move throughout -- both halves
 * visible the whole time, replacing the old "FrOM"/"tO" labels and the
 * static "YOUr MOu" splash chess_run() used to show first (design
 * agreed with the user 2026-08-12).
 *
 * Prompts for the promoting piece (J2's plan named this as J3's job
 * specifically -- a keypad-input concern, not outcome-detection) when
 * from/to only resolves to promotion moves, the same "1n2b3r4q" layout
 * console.c's own keypad picker uses (:651-677), then flashes the chosen
 * move against the piece name once before returning -- the same "keep
 * the move visible" idiom chess_run()'s own outcome announcements use
 * below, applied here too. Defaults to Queen on an ambiguous or aborted
 * choice rather than blocking on it a second time.
 *
 * Returns 0 -- otherwise never a real return value here, since a failed
 * match falls through to "bAd MOuE" and retries -- if the read was
 * aborted (Ctrl-C) or the options menu's EXIT item was used; the
 * sentinel chess_run() below checks for to exit cleanly instead of
 * treating it as an illegal move. STOP alone (TM_KEY_ABORT_STOPKEY) never
 * reaches here -- tm_read_square() already turns it into TM_KEY_RESTART,
 * since STOP only ever aborts input in progress, never the game. Returns
 * TM_MOVE_GO if key 13 ("go") was pressed instead -- chess_run() checks
 * for that distinctly from 0 (see TM_MOVE_GO's own comment above). */
static Move tm_read_move(Position *pos) {
    for (;;) {
        /* pos->side is stable for the whole attempt -- it only changes
         * via make_move(), which doesn't happen until chess_run() commits
         * whatever this function returns. Undo/redo/menu changes restart
         * this whole loop (TM_KEY_RESTART below), which recomputes this
         * fresh against whatever pos->side is after the change. */
        int active_off = (pos->side == WHITE) ? 0 : 4;
        char disp[9];
        for (int i = 0; i < 4; i++) disp[i] = g_tm_white_slot[i];
        for (int i = 0; i < 4; i++) disp[4 + i] = g_tm_black_slot[i];
        disp[8] = '\0';
        /* Blank the *whole* active half before entry starts, not just
         * the cursor position -- found live: tm_read_square() only ever
         * sets its own single cursor character, so without this the
         * other 3 chars of this side's own slot kept showing the tail
         * of that side's *previous* move (e.g. "_2E4" instead of "_   "
         * right after White's first move, since g_tm_white_slot still
         * held "E2E4" from before this new entry began). */
        for (int i = 0; i < 4; i++) disp[active_off + i] = ' ';

        int from = tm_read_square(pos, disp, active_off);
        if (from == TM_KEY_ABORT_CTRLC || from == TM_KEY_EXIT_GAME) return 0;
        if (from == TM_KEY_GO) return TM_MOVE_GO;
        if (from == TM_KEY_RESTART) continue;
        int to = tm_read_square(pos, disp, active_off + 2);
        if (to == TM_KEY_ABORT_CTRLC || to == TM_KEY_EXIT_GAME) return 0;
        if (to == TM_KEY_GO) return TM_MOVE_GO;
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
            if (choice == TM_KEY_ABORT_CTRLC) return 0;
            if (choice == TM_KEY_ABORT_STOPKEY) continue; /* abort this move
                entry, not the game -- back to FrOM, same as everywhere
                else STOP appears now. */
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
            if (only_promo) {
                static const char *promo_words[4] = { "KNIT", "bISH", "rOOK", "QUEN" };
                int idx = (promo_piece == KNIGHT) ? 0 : (promo_piece == BISHOP) ? 1
                          : (promo_piece == ROOK) ? 2 : 3;
                tm_show_move_annotation(disp, active_off / 4, promo_words[idx], 1, false);
            }
            return chosen;
        }
        tm1638_display_string("bAd MOuE");
        time_delay_us(700000);
    }
}

#if CONFIG_ENABLE_ST7735
/* Maps a terminal ChessOutcome to its 4-char tm_show_move_annotation()
 * word (design agreed with the user 2026-08-12) -- stalemate/repetition/
 * 50-move stay collapsed into one "drAU" word, matching J2's own already-
 * hardware-verified console/TM1638 behavior; this milestone didn't
 * revisit that collapse, only how the word gets displayed. Callers check
 * outcome != CHESS_ONGOING before calling this. */
static const char *tm_outcome_word(ChessOutcome outcome) {
    switch (outcome) {
        case CHESS_CHECKMATE_WHITE:
        case CHESS_CHECKMATE_BLACK:
            return "MATE";
        case CHESS_STALEMATE:
        case CHESS_DRAW_REPETITION:
        case CHESS_DRAW_50MOVE:
            return "drAU";
        default:
            return "    ";
    }
}

/* Game-over "wait for the human to notice" pause -- was a bare
 * `for (;;) time_delay_us(...)` at each of this function's three call
 * sites, with no abort check at all: an unescapable loop except by a
 * physical power cycle, found live on hardware playing exactly this out
 * (fool's mate, board correctly displayed "nAtE bL ", then neither the
 * STOP key nor Ctrl-C did anything). This is the same class of bug
 * [[standardized_interrupt_polling]] fixed for the search and the keypad
 * wait -- it just didn't reach these three sites, since none of them call
 * tm_wait_key() or chess_abort_requested() at all.
 *
 * Three outcomes, not two -- found worth adding live, the same session
 * STOP's exit meaning was removed: a human who just got mated may want to
 * "cheat" and take the last move back rather than either starting over or
 * leaving, so key 8 (Back, the same key that means undo everywhere else
 * in this file) takes back the move that ended the game and resumes play
 * from there. EXIT (Ctrl-C) and NEWGAME (STOP) are unchanged from before. */
typedef enum {
    TM_GAMEOVER_EXIT,
    TM_GAMEOVER_NEWGAME,
    TM_GAMEOVER_UNDO,
} TmGameOverChoice;

static TmGameOverChoice tm_wait_game_over(void) {
    for (;;) {
        int k = tm_wait_key();
        if (k == TM_KEY_ABORT_CTRLC) return TM_GAMEOVER_EXIT;
        if (k == TM_KEY_ABORT_STOPKEY) return TM_GAMEOVER_NEWGAME;
        if (k == 8) return TM_GAMEOVER_UNDO;
    }
}

/* Common exit sequence for every path that leaves chess_run() -- found
 * live to be inconsistent: only the top-level "waiting for a move" exit
 * showed "bYE" before leaving, the game-over-then-Ctrl-C path exited
 * silently, and neither cleared the displays afterward, so the last
 * game's board and status stayed on screen after the process controlling
 * them had already ended. One definition of "leaving" for all of them. */
static void tm_exit_chess(void) {
    tm1638_display_string("bYE     ");
    time_delay_us(700000);
    tm1638_display_string("        ");
    st7735_fill_screen(ST7735_BLACK);
    chess_session_end();
}

/* Runs tm_wait_game_over() and applies whichever choice comes back.
 * Returns true if play should continue (chess_run()'s main loop keeps
 * going, possibly against a changed position after an undo), false if
 * the caller should return (tm_exit_chess() already ran). Shared by
 * every "the game just ended" site in chess_run() below rather than
 * three near-identical copies of the same three-way dispatch. */
static bool tm_handle_game_over(Position *pos) {
    TmGameOverChoice choice = tm_wait_game_over();
    if (choice == TM_GAMEOVER_EXIT) {
        tm_exit_chess();
        return false;
    }
    if (choice == TM_GAMEOVER_UNDO) {
        if (pos->history_ply > 0) unmake_move(pos);
        g_console_max_history_ply = pos->history_ply;
        tm_sync_move_slots(pos);
        tm_redraw_if_display(pos);
    } else {
        tm_new_game(pos);
    }
    return true;
}

/* After a move (human-entered or engine-computed, either color) has just
 * been made for `mover_side`, checks game-outcome/check and displays
 * accordingly -- shared by every call site in chess_run() below rather
 * than repeated copies of the same outcome/check/game-over dispatch.
 * `disp` must already hold the post-move 8-char display (White+Black
 * slots); mutated in place by the annotation flash if one fires.
 *
 * Three outcomes, not two: TM_AFTER_EXIT (the game ended and the human
 * chose to leave -- caller returns), TM_AFTER_RESUMED (the game ended
 * but the human chose undo/new-game instead -- keep playing, but this
 * specific move-completion should NOT auto-chain into an engine reply,
 * since the position the human is now looking at didn't come from the
 * move that just happened), or TM_AFTER_ONGOING (the normal case --
 * keep playing, and this completion is eligible to auto-chain if the
 * caller wants that). */
typedef enum {
    TM_AFTER_EXIT,
    TM_AFTER_ONGOING,
    TM_AFTER_RESUMED,
} TmAfterMove;

static TmAfterMove tm_after_move(Position *pos, char *disp, int mover_side) {
    int half = (mover_side == WHITE) ? 0 : 1;
    ChessOutcome outcome = chess_game_outcome(pos);
    if (outcome != CHESS_ONGOING) {
        tm_show_move_annotation(disp, half, tm_outcome_word(outcome), 3, true);
        return tm_handle_game_over(pos) ? TM_AFTER_RESUMED : TM_AFTER_EXIT;
    }
    if (chess_in_check(pos)) {
        tm_show_move_annotation(disp, half, "CHk ", 1, false);
    } else {
        tm1638_display_string(disp);
    }
    return TM_AFTER_ONGOING;
}

void chess_run(void) {
    if (!chess_ensure_init()) return;
    /* 14b: resume the auto-saved game rather than always starting fresh --
     * this is the persona that boots straight into chess with no shell to
     * type `load` at, so it is where resuming matters most. Sets
     * g_console_max_history_ply itself (the undo/redo boundary shared with
     * the console REPL), whether it resumed or reset. */
    console_resume_or_new(&g_chess_pos);
    for (int i = 0; i < 4; i++) { g_tm_white_slot[i] = ' '; g_tm_black_slot[i] = ' '; }

    tm1638_display_string("LUgAL Ch");
    draw_chess_board(&g_chess_pos);
    draw_chess_status(&g_chess_pos, "", false);
    time_delay_us(1000000);

    for (;;) {
        int mover_side = g_chess_pos.side; /* before the move -- whoever
            tm_read_move() is about to prompt/play for. */
        Move mv = tm_read_move(&g_chess_pos);
        if (mv == 0) {
            /* Ctrl-C, or the options menu's EXIT item -- the software exit
             * path this persona lacked before J2: return cleanly to the
             * shell instead of needing a physical board reset. Revises
             * this function's own former "does not return" contract now
             * that there's a real way to ask it to. STOP alone never
             * reaches here (see TM_KEY_ABORT_STOPKEY's own comment
             * above) -- it aborts input, never the game. */
            tm_exit_chess();
            return;
        }

        char last_move_buf[6] = "";
        char *mover_slot = (mover_side == WHITE) ? g_tm_white_slot : g_tm_black_slot;
        bool go_pressed = (mv == TM_MOVE_GO);

        if (!go_pressed) {
            /* Named before it is played: SAN describes a move in terms of what
             * else could have been played instead, so the pre-move position is
             * the input, not a convenience (see pgn.h). */
            char mv_san[SAN_MAX];
            format_move_san(&g_chess_pos, mv, mv_san);

            if (!make_move(&g_chess_pos, mv)) {
                /* tm_read_move() only returns pseudo-legal moves from the
                 * real move list, so make_move() only fails here on a
                 * king-safety violation (a pin, moving into check) -- a
                 * real "no, that specific move is illegal", not a parse
                 * failure. */
                tm1638_display_string("ILLEGAL ");
                time_delay_us(700000);
                continue;
            }
            g_console_max_history_ply = g_chess_pos.history_ply; /* J3:
                new move played -- the redo boundary advances, same as
                J1's own console_execute_move()/console_engine_reply()
                do. */
            tm_format_move4(mv, mover_slot);
            tm_format_move4(mv, last_move_buf); /* uppercase on the TFT
                too now, matching tm_probe_pv()'s PV line -- consistent
                with the TM1638 slot right above rather than format_move()'s
                lowercase, which would only differ here and nowhere else
                any more. */

            char disp[9];
            for (int i = 0; i < 4; i++) { disp[i] = g_tm_white_slot[i]; disp[4 + i] = g_tm_black_slot[i]; }
            disp[8] = '\0';
            tm1638_display_string(disp);
            draw_chess_board(&g_chess_pos);
            draw_chess_status(&g_chess_pos, last_move_buf, false);
            /* And the terminal, which until now saw only tm_wait_key()'s raw
             * key codes while a game was played on the board (phase 16). The
             * TFT/7-segment writes above stay explicit rather than deferring
             * to chess_show(): this path has a real `last_move_buf` for the
             * status line, which chess_show() -- built for the state changes
             * that have no "move just made" -- deliberately does not take. */
            /* What was entered on the keypad, in the notation the rest of
             * the session and the PGN files use. The terminal has no other
             * way to know: the keys themselves are the board's business. */
            cprintf("Board plays: %s\n", mv_san);
            print_board(&g_chess_pos);
            console_autosave(&g_chess_pos);   /* 14b */

            TmAfterMove result = tm_after_move(&g_chess_pos, disp, mover_side);
            if (result == TM_AFTER_EXIT) return;
            if (result == TM_AFTER_RESUMED || !g_tm_auto_reply) continue; /* back
                to tm_read_move() for whoever's turn it is now -- human or
                "go", either color, nothing hardcoded. */
            /* Human's move stands, game is still ongoing, and auto-reply
             * is on (the default, design agreed with the user 2026-08-12,
             * after "go" alone turned out ambiguous about this): fall
             * through and let the engine reply immediately for whichever
             * side is now to move, no separate prompt or "go" needed --
             * the classic "you move, computer replies" experience. */
            mover_side = g_chess_pos.side;
            mover_slot = (mover_side == WHITE) ? g_tm_white_slot : g_tm_black_slot;
        } else {
            /* "go" (key 13, user request 2026-08-12) -- skip entering a
             * move, let the engine play whichever side is actually to
             * move. Blank this side's own slot rather than a move, since
             * there wasn't a human one this turn -- the ticker below
             * fills it back in live once the engine has something to
             * show. Never itself auto-chains afterward (see below) --
             * only a human-typed move can trigger the fall-through above. */
            for (int i = 0; i < 4; i++) mover_slot[i] = ' ';
            char disp[9];
            for (int i = 0; i < 4; i++) { disp[i] = g_tm_white_slot[i]; disp[4 + i] = g_tm_black_slot[i]; }
            disp[8] = '\0';
            tm1638_display_string(disp);
            draw_chess_board(&g_chess_pos);
            draw_chess_status(&g_chess_pos, last_move_buf, false);
        }

        /* Engine actually calculates now -- reached either via "go"
         * above, or via the auto-reply fall-through just above. Always
         * returns to prompting afterward (below), never chains a second
         * time -- otherwise "go" or an AUTO-on reply could set off the
         * engine playing both sides in an unstoppable loop with nothing
         * to break it. The side being calculated for (mover_side)
         * freezes the *other* slot into g_tm_frozen_slot: the ticker
         * (search_poll_stop_callback() -> tm_search_ticker_tick()) needs
         * to know which physical half is "the other one" so it can
         * redraw the full 8-char display once a second without
         * clobbering it. */
        g_tm_active_half = (mover_side == WHITE) ? 0 : 1;
        char *other_slot = (mover_side == WHITE) ? g_tm_black_slot : g_tm_white_slot;
        for (int i = 0; i < 4; i++) g_tm_frozen_slot[i] = other_slot[i];
        draw_chess_status(&g_chess_pos, last_move_buf, true);
        /* J3: the level J1's console REPL and this menu's key-12 level
         * select both set (g_console_search_level) -- was a hardcoded
         * 5000ms before this milestone gave chess_run() a level concept
         * at all. */
        Move engine_move = chess_think(&g_chess_pos, 64, level_times_ms[g_console_search_level - 1]);
        if (engine_move == 0) {
            /* Defensive only -- the outcome check above already returned
             * ongoing (so a legal reply exists) before this search ever
             * started; a real hit here would mean engine/outcome logic
             * disagree, not an expected game state. */
            tm1638_display_string("gAME OuEr");
            draw_chess_status(&g_chess_pos, "no moves", false);
            if (!tm_handle_game_over(&g_chess_pos)) return;
            continue;
        }
        char eng_san[SAN_MAX];
        format_move_san(&g_chess_pos, engine_move, eng_san);   /* before the move */

        make_move(&g_chess_pos, engine_move);
        g_console_max_history_ply = g_chess_pos.history_ply;
        tm_format_move4(engine_move, mover_slot);
        tm_format_move4(engine_move, last_move_buf);
        draw_chess_board(&g_chess_pos);
        draw_chess_status(&g_chess_pos, last_move_buf, false);
        /* The engine's own reply, on the terminal too (phase 16), and named
         * the way the console REPL names it so a session driven from both
         * devices reads as one transcript rather than two. */
        cprintf("Engine plays: %s (Score: %s%d)\n",
                eng_san, sign_prefix(g_search_score), g_search_score);
        print_board(&g_chess_pos);
        console_autosave(&g_chess_pos);   /* 14b */

        char disp2[9];
        for (int i = 0; i < 4; i++) { disp2[i] = g_tm_white_slot[i]; disp2[4 + i] = g_tm_black_slot[i]; }
        disp2[8] = '\0';
        if (tm_after_move(&g_chess_pos, disp2, mover_side) == TM_AFTER_EXIT) return;
    }
}
#endif /* CONFIG_ENABLE_ST7735 */
#endif /* CONFIG_BOARD_RP2350 && CONFIG_ENABLE_TM1638 */

/* See the forward declaration near the top of this file for why this exists.
 *
 * Defined out here, past every hardware guard, because it is the one function
 * that must exist on *every* build: the terminal is the only output a QEMU
 * target has, and it is precisely the output that was being skipped. The
 * display and keypad halves are guarded individually, so a board with a
 * keypad and no TFT (H3's three independent flags) gets exactly the outputs it
 * has. */
static void chess_show(const Position *pos) {
    print_board(pos);
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    tm_redraw_if_display(pos);
    tm_sync_move_slots(pos);
    {
        char disp[9];
        for (int i = 0; i < 4; i++) {
            disp[i] = g_tm_white_slot[i];
            disp[4 + i] = g_tm_black_slot[i];
        }
        disp[8] = '\0';
        tm1638_display_string(disp);
    }
#endif
}
