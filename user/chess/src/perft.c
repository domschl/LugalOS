/*
 * Vendored from ~/gith/domschl/LugalChess (engine/src/perft.c), J4,
 * plan/phase10_chess_completion.md. run_perft() itself is unchanged --
 * pure generate_moves()/make_move()/unmake_move() recursion, no stdio, no
 * timing. run_perft_tests_depth() needed three platform edits:
 *
 *   - printf -> cprintf, via chess_platform.h's existing #define, the same
 *     as every other vendored file in this directory.
 *   - clock()/CLOCKS_PER_SEC/<time.h> (host-only, not available in this
 *     freestanding build) -> kernel/time.h's time_get_ms(), the same
 *     millisecond timer chess_ui.c's own chess_selftest() already uses.
 *   - %llu and %.1f aren't supported by cprintf (kernel/printk.c's own
 *     format parser only ever consumes a single 'l', never 'll', and has
 *     no floating-point case at all -- the same category of gap H4 already
 *     found and fixed once for search.c's own nps figure). Large node
 *     counts print through print_u64() below, manual decimal digits rather
 *     than relying on cprintf's integer width at all (uint64_t is wider
 *     than `long` on a 32-bit target, which is what cprintf's %d/%u/%x
 *     actually read regardless of an 'l' prefix); nodes/sec is plain
 *     integer nodes-per-second, matching search.c's own already-
 *     established fix, not floating-point KNPS. The upstream "%-18s"
 *     column alignment for the test-case name is also dropped -- this
 *     printf has no field-width support for %s at all (only %d/%u/%x pad),
 *     confirmed by reading vprintk_to() rather than assumed.
 *
 * Fourth platform edit, found only on real RP2350 hardware, not in QEMU
 * (falsify_on_hardware_not_qemu, again): run_perft()'s upstream body
 * declares `MoveList list;` as a plain stack local inside a *recursive*
 * function. Each MoveList is ~516 bytes (MAX_MOVES=256 Move slots + count);
 * a `(perft 7)` call -- or the "end games"/"start pos" table rows at their
 * own native depth -- recurses 7 levels deep, ~3.6 KB of stack for the
 * MoveList arrays alone, stacked on top of whatever the shell/lisp/console
 * call chain above it already holds. tests/hw/test_rp2350.py's memory-
 * margins check (3/4 of the 16 KB boot stack) failed on exactly this after
 * manual interactive perft testing. Fixed the same way J0 already fixed
 * search.c's own per-ply MoveList arrays: a heap-backed, ply-indexed pool
 * (perft_pools_init()/perft_pools_free() below), not a `static` -- a plain
 * static would alias every recursion level onto the same memory and corrupt
 * the parent's still-in-progress move list, which is exactly why upstream
 * used a stack local in the first place. Indexing by ply keeps each level's
 * storage distinct while moving it off the stack, and run_perft()'s public
 * signature is unchanged.
 */
#include "perft.h"
#include "movegen.h"
#include "move.h"
#include "kernel/time.h"
#include "kernel/palloc.h"
#include "kernel/sched.h"
#include "kernel/hart.h"

/* Generous relative to any depth perft is actually run at -- the deepest
 * table entry is 7, and perft's own node counts make depth 10+ take longer
 * than any reasonable timeout regardless of stack. Kept far below
 * MAX_SEARCH_PLYS-scale pool sizes since perft never needs search.c's own
 * ply range. */
#define PERFT_MAX_PLY 32

/* One worker per hart, no more: the split is over root moves and this kernel
 * has exactly MAX_HARTS of them. */
#define PERFT_MAX_WORKERS MAX_HARTS

/* One of these per worker (X8, plan/phase23_multicore_scheduling.md).
 *
 * `perft_movelists` and `perft_ply` used to be plain file-statics, which is
 * correct for one caller and fatal for two: the ply index selects which
 * MoveList a recursion level owns, so two cores sharing it would hand each
 * other's still-in-progress lists back and forth. Splitting them into a
 * context is the whole of what parallel perft needed from this file --
 * run_perft()'s own signature and behaviour are unchanged, and the
 * single-threaded path still uses exactly one context.
 *
 * Note what is NOT here, because it is what makes this cheap: the move
 * generator's magic-bitboard tables (user/chess/src/bitboard.c) are written
 * once by init_bitboards() and read-only afterwards, so every worker shares
 * them with no synchronisation at all. */
typedef struct {
    MoveList *lists;    /* PERFT_MAX_PLY entries */
    uint32_t  pages;    /* what to hand back to palloc */
    int       ply;
} perft_ctx_t;

static perft_ctx_t perft_main;   /* the single-threaded path's context */

static bool perft_ctx_init(perft_ctx_t *c) {
    if (c->lists != NULL) {
        return true; /* already allocated -- idempotent, like search_pools_init(). */
    }
    uint32_t bytes = (uint32_t)(PERFT_MAX_PLY * sizeof(MoveList));
    c->pages = (bytes + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    c->lists = (MoveList *)palloc_pages(c->pages);
    if (c->lists == NULL) {
        c->pages = 0;
        return false;
    }
    c->ply = 0;
    return true;
}

static void perft_ctx_free(perft_ctx_t *c) {
    if (c->lists == NULL) {
        return;
    }
    palloc_free(c->lists, c->pages);
    c->lists = NULL;
    c->pages = 0;
    c->ply = 0;
}

static void perft_pools_free(void) { perft_ctx_free(&perft_main); }

static uint64_t perft_rec(perft_ctx_t *c, Position *pos, int depth); /* below */

/* --- Root splitting across cores (X8) ------------------------------------
 *
 * Perft is the honest first use of a second core, and the reason is the test
 * table below: the node counts are exact and published, so a parallel run is
 * either right or wrong with no argument about it. Nothing else in this tree
 * verifies a concurrency change that cleanly.
 *
 * The split is over root moves. Each worker takes moves offset, offset+N,
 * offset+2N... from one shared read-only list, walks its own copy of the
 * position, and returns a count; the counts sum exactly because the subtrees
 * are disjoint. No transposition table, no move ordering, no feedback of any
 * kind between workers -- which is why this parallelises and the search does
 * not.
 *
 * Round-robin rather than contiguous blocks, deliberately: root moves have
 * wildly different subtree sizes (a queen move against a pawn push), so
 * handing worker 0 the first half would leave one core idle for most of the
 * run. Interleaving does not balance it perfectly -- nothing static does --
 * and the measured speedup is reported rather than claimed.
 */

typedef struct {
    Position     pos;        /* this worker's own board; make/unmake mutate it */
    perft_ctx_t  ctx;
    const Move  *moves;      /* the shared root list, read-only */
    int          nmoves;
    int          offset;     /* first root move index for this worker */
    int          stride;     /* how many workers there are */
    int          depth;
    volatile uint64_t nodes;
    volatile int done;
} perft_worker_t;

/* Pages per worker record, rounded up. A Position carries MAX_PLYS of undo
 * state, so this is ~8 KB and not something to put on a task stack. */
#define PERFT_WORKER_PAGES \
    ((uint32_t)((sizeof(perft_worker_t) + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE))

static uint64_t perft_worker_run(perft_worker_t *w) {
    uint64_t nodes = 0ULL;
    for (int i = w->offset; i < w->nmoves; i += w->stride) {
        if (!make_move(&w->pos, w->moves[i])) {
            continue;   /* illegal: the same one every worker would also skip */
        }
        nodes += perft_rec(&w->ctx, &w->pos, w->depth - 1);
        unmake_move(&w->pos);
    }
    return nodes;
}

static void perft_worker_body(void *arg) {
    perft_worker_t *w = (perft_worker_t *)arg;
    w->nodes = perft_worker_run(w);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    w->done = 1;
}

/* Splits `depth` across `workers` cores. Returns the node count, identical to
 * run_perft() for any worker count -- that identity is the test.
 *
 * Falls back to one worker whenever a second cannot be had (no SMP, only one
 * hart online, no memory, no free task slot) rather than failing: a caller
 * asking for two cores on a machine with one wants the answer, not an error.
 * How many actually ran is reported through `*used` so nobody has to infer it
 * from the timing. */
uint64_t run_perft_cores(Position *root, int depth, int workers, int *used) {
    if (used) *used = 1;
    if (depth <= 0) return 1ULL;

    unsigned online = smp_harts_online();
    if (workers < 1) workers = 1;
    if ((unsigned)workers > online) workers = (int)online;
    if (workers > PERFT_MAX_WORKERS) workers = PERFT_MAX_WORKERS;
    if (workers <= 1) return run_perft(root, depth);

    /* One shared root list. Generated once, never written again. */
    static MoveList root_list;
    generate_moves(root, &root_list);

    perft_worker_t *w = (perft_worker_t *)palloc_pages(PERFT_WORKER_PAGES * (uint32_t)workers);
    if (!w) return run_perft(root, depth);

    int made = 0;
    for (int i = 0; i < workers; i++) {
        w[i].pos    = *root;
        w[i].ctx.lists = NULL;
        w[i].ctx.pages = 0;
        w[i].ctx.ply   = 0;
        w[i].moves  = root_list.moves;
        w[i].nmoves = root_list.count;
        w[i].offset = i;
        w[i].stride = workers;
        w[i].depth  = depth;
        w[i].nodes  = 0;
        w[i].done   = 0;
        if (!perft_ctx_init(&w[i].ctx)) break;
        made++;
    }
    if (made < workers) {
        /* Could not equip everyone. Rather than run a split that would drop
         * whole root moves and report a wrong count, hand the pages back and
         * do it on one core. A wrong perft number is worse than a slow one. */
        for (int i = 0; i < made; i++) perft_ctx_free(&w[i].ctx);
        palloc_free(w, PERFT_WORKER_PAGES * (uint32_t)workers);
        return run_perft(root, depth);
    }

    /* Workers 1..n-1 on their own harts; worker 0 is this task, on this one.
     * Pinned at creation (task_create_pinned), because a task that is READY
     * before its affinity lands can be claimed by the wrong hart -- X5 found
     * that the hard way. */
    int spawned = 0;
    for (int i = 1; i < workers; i++) {
        if (task_create_pinned("perftw", perft_worker_body, &w[i], i) < 0) break;
        spawned++;
    }

    w[0].nodes = perft_worker_run(&w[0]);
    w[0].done  = 1;

    /* Anything that could not be spawned is run here, so no root move is
     * silently dropped. */
    for (int i = 1 + spawned; i < workers; i++) {
        w[i].nodes = perft_worker_run(&w[i]);
        w[i].done  = 1;
    }

    for (int i = 1; i <= spawned; i++) {
        while (!w[i].done) sched_yield();
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    uint64_t nodes = 0ULL;
    for (int i = 0; i < workers; i++) {
        nodes += w[i].nodes;
        perft_ctx_free(&w[i].ctx);
    }
    palloc_free(w, PERFT_WORKER_PAGES * (uint32_t)workers);
    if (used) *used = 1 + spawned;
    return nodes;
}

typedef struct {
    const char *name;
    const char *fen;
    uint64_t perftcnt[10];
    int depth_count;
} PerftTestCase;

static const PerftTestCase perft_cases[] = {
    // Reference: https://www.chessprogramming.org/Perft_Results
    {"end games", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -", {14, 191, 2812, 43238, 674624, 11030083, 178633661}, 7},
    {"strange bugs", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", {44, 1486, 62379, 2103487, 89941194}, 5},
    {"start pos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", {20, 400, 8902, 197281, 4865609, 119060324, 3195901860ULL}, 7},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", {48, 2039, 97862, 4085603, 193690690, 8031647685ULL}, 6},

    {"position-4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", {6, 264, 9467, 422333, 15833292, 706045033}, 6},
    {"position-4-mirror", "r2q1rk1/pP1p2pp/Q4n2/bbp1p3/Np6/1B3NBn/pPPP1PPP/R3K2R b KQ - 0 1", {6, 264, 9467, 422333, 15833292, 706045033}, 6},
    {"position-6", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", {46, 2079, 89890, 3894594, 164075551, 6923051137ULL}, 6},

    // Reference: https://gist.github.com/peterellisjones/8c46c28141c162d1d8a0f0badbc9cff9
    {"pej-1", "r6r/1b2k1bq/8/8/7B/8/8/R3K2R b QK - 3 2", {8}, 1},
    {"pej-2", "r1bqkbnr/pppppppp/n7/8/8/P7/1PPPPPPP/RNBQKBNR w QqKk - 2 2", {19}, 1},
    {"pej-3", "r3k2r/p1pp1pb1/bn2Qnp1/2qPN3/1p2P3/2N5/PPPBBPPP/R3K2R b QqKk - 3 2", {5}, 1},
    {"pej-4", "2kr3r/p1ppqpb1/bn2Qnp1/3PN3/1p2P3/2N5/PPPBBPPP/R3K2R b QK - 3 2", {44}, 1},
    {"pej-5", "rnb2k1r/pp1Pbppp/2p5/q7/2B5/8/PPPQNnPP/RNB1K2R w QK - 3 9", {39}, 1},
    {"pej-6", "2r5/3pk3/8/2P5/8/2K5/8/8 w - - 5 4", {9}, 1},
    {"pej-7", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", {44, 1486, 62379}, 3},
    {"pej-8", "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", {46, 2079, 89890}, 3},
    {"pej-9", "3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1", {18, 92, 1670, 10138, 185429, 1134888}, 6},
    {"pej-10", "8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1", {13, 102, 1266, 10276, 135655, 1015133}, 6},
    {"pej-11", "8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", {15, 126, 1928, 13931, 206379, 1440467}, 6},
    {"pej-12", "5k2/8/8/8/8/8/8/4K2R w K - 0 1", {15, 66, 1198, 6399, 120330, 661072}, 6},
    {"pej-13", "3k4/8/8/8/8/8/8/R3K3 w Q - 0 1", {16, 71, 1286, 7418, 141077, 803711}, 6},
    {"pej-14", "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", {26, 1141, 27826, 1274206}, 4},
    {"pej-15", "r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1", {44, 1494, 50509, 1720476}, 4},
    {"pej-16", "2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1", {11, 133, 1442, 19174, 266199, 3821001}, 6},
    {"pej-17", "8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1", {29, 165, 5160, 31961, 1004658}, 5},
    {"pej-18", "4k3/1P6/8/8/8/8/K7/8 w - - 0 1", {9, 40, 472, 2661, 38983, 217342}, 6},
    {"pej-19", "8/P1k5/K7/8/8/8/8/8 w - - 0 1", {6, 27, 273, 1329, 18135, 92683}, 6},
    {"pej-20", "K1k5/8/P7/8/8/8/8/8 w - - 0 1", {2, 6, 13, 63, 382, 2217}, 6},
    {"pej-21", "8/k1P5/8/1K6/8/8/8/8 w - - 0 1", {10, 25, 268, 926, 10857, 43261, 567584}, 7},
    {"pej-22", "8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1", {37, 183, 6559, 23527}, 4}
};

static const int perft_cases_count = sizeof(perft_cases) / sizeof(perft_cases[0]);

/* Manual decimal digits -- see this file's own header comment for why
 * cprintf's %u/%llu can't print a uint64_t safely on a 32-bit target. */
static void print_u64(uint64_t v) {
    char buf[21];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) {
        buf[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (n > 0) {
        cprintf("%c", buf[--n]);
    }
}

// Recursive perft runner, against one worker's own pools.
static uint64_t perft_rec(perft_ctx_t *c, Position *pos, int depth) {
    if (depth == 0) {
        return 1ULL;
    }
    if (c->ply >= PERFT_MAX_PLY) {
        return 0ULL; /* should never trigger -- see PERFT_MAX_PLY's comment. */
    }

    MoveList *list = &c->lists[c->ply];
    generate_moves(pos, list);
    uint64_t nodes = 0ULL;

    c->ply++;
    for (int i = 0; i < list->count; i++) {
        if (!make_move(pos, list->moves[i])) {
            continue;
        }
        nodes += perft_rec(c, pos, depth - 1);
        unmake_move(pos);
    }
    c->ply--;

    return nodes;
}

uint64_t run_perft(Position *pos, int depth) {
    if (depth == 0) {
        return 1ULL;
    }
    if (perft_main.lists == NULL && !perft_ctx_init(&perft_main)) {
        return 0ULL;
    }
    return perft_rec(&perft_main, pos, depth);
}

int run_perft_tests_depth(int max_depth) {
    return run_perft_tests_cores(max_depth, 1);
}

int run_perft_tests_cores(int max_depth, int workers) {
    int errors = 0;
    int passed = 0;
    int used_max = 1;

    if (max_depth <= 0) {
        max_depth = 5;
    }

    printf("Starting PERFT Verification Suite (Max Depth: %d, cores requested: %d)...\n",
           max_depth, workers);
    uint64_t suite_start_ms = time_get_ms();
    printf("========================================================================\n");

    for (int i = 0; i < perft_cases_count; i++) {
        const PerftTestCase *tc = &perft_cases[i];
        printf("Test Case: %s  FEN: %s\n", tc->name, tc->fen);

        Position pos;
        parse_fen(&pos, tc->fen);

        int max_d = tc->depth_count;
        if (max_d > max_depth) max_d = max_depth;

        printf("Depth: ");
        for (int d = 1; d <= max_d; d++) {
            // Check if test case has expected node count for this depth
            uint64_t expected = tc->perftcnt[d - 1];
            if (expected == 0) break;

            printf("%d ", d);

            int used = 1;
            uint64_t start_ms = time_get_ms();
            uint64_t actual = run_perft_cores(&pos, d, workers, &used);
            uint64_t elapsed_ms = time_get_ms() - start_ms;
            if (used > used_max) used_max = used;

            if (actual != expected) {
                printf("\n  -> ERROR at Depth %d: Expected ", d);
                print_u64(expected);
                printf(", Got ");
                print_u64(actual);
                printf("\n");
                errors++;
                break;
            } else {
                passed++;
                if (actual > 10000) {
                    printf("(");
                    print_u64(actual);
                    printf(" nodes");
                    if (elapsed_ms > 0) {
                        uint64_t nps = (actual * 1000) / elapsed_ms;
                        printf(", ");
                        print_u64(nps);
                        printf(" nps");
                    }
                    printf(") ");
                }
            }
        }
        printf("\n------------------------------------------------------------------------\n");
    }

    /* Total elapsed, in-guest. The per-depth nps figures above are already
     * measured here, but a caller comparing one core against two wants a
     * single number that is not their own terminal round-trip -- timing this
     * from the host measures the host's sleep, which is how the first X8
     * hardware run managed to report two cores as slower than one. */
    uint64_t suite_ms = time_get_ms() - suite_start_ms;
    printf("PERFT Results: %d passed depths, %d errors (cores used: %d, ",
           passed, errors, used_max);
    print_u64(suite_ms);
    printf(" ms).\n");
    perft_pools_free();
    return errors;
}

int run_perft_tests(void) {
    return run_perft_tests_depth(5);
}
