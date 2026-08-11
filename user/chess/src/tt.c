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

void init_tt(int size_mb) {
    if (tt_table != NULL) {
        free_tt();
    }

    uint64_t bytes;
#if defined(LUGALCHESS_EMBEDDED)
    (void)size_mb;
    // On microcontrollers, ignore size_mb and allocate a safe 32KB table (2048 entries)
    bytes = 32 * 1024ULL;
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

    if (entry->hash_key == hash_key) {
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

    if (entry->hash_key == hash_key) {
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
    bool replace = (entry->hash_key == 0) ||
                   (entry->hash_key == hash_key) ||
                   (depth >= entry->depth) ||
                   (entry->age != tt_current_age);

    if (replace) {
        entry->hash_key = hash_key;
        // Keep the old best move if the new one is null (e.g. during fails-low)
        entry->best_move = (best_move != 0) ? best_move : entry->best_move;
        entry->score = (int16_t)score;
        entry->depth = (int8_t)depth;
        entry->flags = flags;
        entry->age = tt_current_age;
    }
}
