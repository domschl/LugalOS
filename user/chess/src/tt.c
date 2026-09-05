/*
 * Vendored from ~/gith/domschl/LugalChess (engine/src/tt.c), H4,
 * plan/phase9_chess_computer.md. Only change from upstream: malloc()/free()
 * don't exist on LugalOS -- there is no general heap, only palloc_pages()'s
 * page-granular allocator (see kernel/palloc.h). LUGALCHESS_EMBEDDED always
 * requests a fixed 32 KB table here (exactly 8 pages at PAGE_SIZE=4096), so
 * a single palloc_pages(8)/palloc_free(ptr, 8) pair replaces malloc/free
 * directly -- no general allocator needed for this one call site.
 */

#include "tt.h"
#include "kernel/palloc.h"

TTEntry *tt_table = NULL;
int tt_size_entries = 0;
uint8_t tt_current_age = 0;

static uint32_t tt_pages = 0;

/* See init_tt(). 32 KB is the pre-X8b value and the default. */
uint32_t tt_embedded_bytes = 32u * 1024u;

void init_tt(int size_mb) {
    if (tt_table != NULL) {
        free_tt();
    }

    uint64_t bytes;
#if defined(LUGALCHESS_EMBEDDED)
    (void)size_mb;
    /* On microcontrollers, a fixed table rather than size_mb -- 32 KB, i.e.
     * 2048 entries, which is what RP2350's heap can spare.
     *
     * Settable since X8b (plan/phase23_multicore_scheduling.md), because it
     * is the variable that decides whether Lazy SMP is worth anything: the
     * helper's only contribution is TT entries the primary then finds
     * already filled, so a table two searchers can thrash caps the gain at
     * zero no matter how well the threading works. Measured rather than
     * assumed -- see the milestone. Default unchanged. */
    bytes = (uint64_t)tt_embedded_bytes;
#else
    // Default to 16MB if size is 0 or negative
    if (size_mb <= 0) {
        size_mb = 16;
    }
    bytes = (uint64_t)size_mb * 1024ULL * 1024ULL;
#endif

    tt_size_entries = (int)(bytes / sizeof(TTEntry));

    // Allocate memory
    tt_pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    tt_table = (TTEntry *)palloc_pages(tt_pages);
    if (tt_table == NULL) {
        tt_size_entries = 0;
        tt_pages = 0;
        return;
    }

    clear_tt();
}


void free_tt(void) {
    if (tt_table != NULL) {
        palloc_free(tt_table, tt_pages);
        tt_table = NULL;
        tt_pages = 0;
    }
    tt_size_entries = 0;
}

/* --- Lockless safety for a shared table (X8b) ----------------------------
 *
 * A TTEntry is a 64-bit key plus move, score, depth, flags and age -- six
 * fields, written and read without a lock. On one core that is fine. On two
 * it is not: core A can store its key, be interleaved by core B storing a
 * different position's key and fields, and then store its own fields over
 * them. The slot then holds B's key with A's move, and the engine happily
 * fetches "the best move for this position" that belongs to another position
 * entirely. `make_move()` rejects an outright illegal move, but a move that
 * is legal here and simply wrong is not rejected by anything -- it is just
 * played.
 *
 * Hyatt's lockless scheme, which is the standard answer: store the key
 * XOR-ed with the payload, and check it by re-deriving. A torn entry fails
 * the comparison and reads as a miss, which costs one lookup and never a
 * wrong move. No lock, no atomics, and nothing on the probe path but an XOR.
 *
 * The payload mix must include every field a reader trusts, or a tear in the
 * omitted one would go undetected. */
static inline uint64_t tt_payload(const TTEntry *e) {
    return ((uint64_t)e->best_move)
         | ((uint64_t)(uint16_t)e->score << 16)
         | ((uint64_t)(uint8_t)e->depth  << 32)
         | ((uint64_t)e->flags           << 40)
         | ((uint64_t)e->age             << 48);
}

/* The key this entry actually holds, or garbage if it was torn -- in which
 * case it will not match what any caller is looking for. */
static inline uint64_t tt_key(const TTEntry *e) {
    return e->hash_key ^ tt_payload(e);
}

static inline void tt_store(TTEntry *e, uint64_t key) {
    /* Payload first, key last, with a barrier between: the key is what
     * validates the payload, so it must not become visible before the
     * payload it vouches for. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e->hash_key = key ^ tt_payload(e);
}

void clear_tt(void) {
    if (tt_table != NULL) {
        memset(tt_table, 0, tt_size_entries * sizeof(TTEntry));
    }
    tt_current_age = 0;
}

void increment_tt_age(void) {
    tt_current_age++;
}

bool read_tt(uint64_t hash_key, int depth, int ply, int alpha, int beta, int *score, Move *best_move) {
    if (tt_table == NULL || tt_size_entries == 0) {
        return false;
    }

    int idx = (int)(hash_key % (uint64_t)tt_size_entries);
    TTEntry *entry = &tt_table[idx];

    if (tt_key(entry) == hash_key) {
        if (best_move) *best_move = entry->best_move;

        // Only use the score if the depth is sufficient
        if (entry->depth >= depth) {
            int tt_score = score_from_tt((int)entry->score, ply);

            if (entry->flags == TT_EXACT) {
                if (score) *score = tt_score;
                return true;
            }
            if (entry->flags == TT_ALPHA && tt_score <= alpha) {
                if (score) *score = tt_score;
                return true;
            }
            if (entry->flags == TT_BETA && tt_score >= beta) {
                if (score) *score = tt_score;
                return true;
            }
        }
    } else {
        if (best_move) *best_move = 0;
    }

    return false;
}

bool probe_tt_entry(uint64_t hash_key, int *score, Move *best_move) {
    if (tt_table == NULL || tt_size_entries == 0) {
        return false;
    }

    int idx = (int)(hash_key % (uint64_t)tt_size_entries);
    TTEntry *entry = &tt_table[idx];

    if (tt_key(entry) == hash_key) {
        if (best_move) *best_move = entry->best_move;
        if (score) *score = score_from_tt((int)entry->score, 0);
        return true;
    }

    return false;
}

void write_tt(uint64_t hash_key, Move best_move, int score, int depth, uint8_t flags) {
    if (tt_table == NULL || tt_size_entries == 0) {
        return;
    }

    int idx = (int)(hash_key % (uint64_t)tt_size_entries);
    TTEntry *entry = &tt_table[idx];

    // Replacement strategy: Replace if empty, if it's the same position,
    // if new depth >= old depth, or if old entry belongs to a previous search age.
    uint64_t held = tt_key(entry);
    bool replace = (held == 0) ||
                   (held == hash_key) ||
                   (depth >= entry->depth) ||
                   (entry->age != tt_current_age);

    if (replace) {
        // Keep the old best move if the new one is null (e.g. during fails-low)
        entry->best_move = (best_move != 0) ? best_move : entry->best_move;
        entry->score = (int16_t)score;
        entry->depth = (int8_t)depth;
        entry->flags = flags;
        entry->age = tt_current_age;
        tt_store(entry, hash_key);   /* key last: see tt_payload() above */
    }
}
