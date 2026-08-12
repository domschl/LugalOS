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

/* Generous relative to any depth perft is actually run at -- the deepest
 * table entry is 7, and perft's own node counts make depth 10+ take longer
 * than any reasonable timeout regardless of stack. Kept far below
 * MAX_SEARCH_PLYS-scale pool sizes since perft never needs search.c's own
 * ply range. */
#define PERFT_MAX_PLY 32

static MoveList *perft_movelists = NULL;
static uint32_t perft_movelists_pages = 0;
static int perft_ply = 0;

static bool perft_pools_init(void) {
    if (perft_movelists != NULL) {
        return true; /* already allocated -- idempotent, like search_pools_init(). */
    }
    uint32_t bytes = (uint32_t)(PERFT_MAX_PLY * sizeof(MoveList));
    perft_movelists_pages = (bytes + (uint32_t)PAGE_SIZE - 1) / (uint32_t)PAGE_SIZE;
    perft_movelists = (MoveList *)palloc_pages(perft_movelists_pages);
    if (perft_movelists == NULL) {
        perft_movelists_pages = 0;
        return false;
    }
    return true;
}

static void perft_pools_free(void) {
    if (perft_movelists == NULL) {
        return;
    }
    palloc_free(perft_movelists, perft_movelists_pages);
    perft_movelists = NULL;
    perft_movelists_pages = 0;
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

// Recursive perft runner
uint64_t run_perft(Position *pos, int depth) {
    if (depth == 0) {
        return 1ULL;
    }
    if (perft_movelists == NULL && !perft_pools_init()) {
        return 0ULL;
    }
    if (perft_ply >= PERFT_MAX_PLY) {
        return 0ULL; /* should never trigger -- see PERFT_MAX_PLY's comment. */
    }

    MoveList *list = &perft_movelists[perft_ply];
    generate_moves(pos, list);
    uint64_t nodes = 0ULL;

    perft_ply++;
    for (int i = 0; i < list->count; i++) {
        if (!make_move(pos, list->moves[i])) {
            continue;
        }
        nodes += run_perft(pos, depth - 1);
        unmake_move(pos);
    }
    perft_ply--;

    return nodes;
}

int run_perft_tests_depth(int max_depth) {
    int errors = 0;
    int passed = 0;

    if (max_depth <= 0) {
        max_depth = 5;
    }

    printf("Starting PERFT Verification Suite (Max Depth: %d)...\n", max_depth);
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

            uint64_t start_ms = time_get_ms();
            uint64_t actual = run_perft(&pos, d);
            uint64_t elapsed_ms = time_get_ms() - start_ms;

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

    printf("PERFT Results: %d passed depths, %d errors.\n", passed, errors);
    perft_pools_free();
    return errors;
}

int run_perft_tests(void) {
    return run_perft_tests_depth(5);
}
