/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/tt.h),
 * H4, plan/phase9_chess_computer.md.
 */

#ifndef TT_H
#define TT_H

#include "defs.h"

#define TT_EXACT 0
#define TT_ALPHA 1
#define TT_BETA  2

typedef struct {
    uint64_t hash_key;
    Move best_move;
    int16_t score;
    int8_t depth;
    uint8_t flags;
    uint8_t age;
} TTEntry;

extern TTEntry *tt_table;
extern int tt_size_entries;
extern uint8_t tt_current_age;

// Convert mate scores to/from transposition table representation
static inline int score_to_tt(int score, int ply) {
    if (score > MATE_VALUE) return score + ply;
    if (score < -MATE_VALUE) return score - ply;
    return score;
}

static inline int score_from_tt(int score, int ply) {
    if (score > MATE_VALUE) return score - ply;
    if (score < -MATE_VALUE) return score + ply;
    return score;
}

/* Size of the embedded transposition table, in bytes (X8b). 32 KB by
 * default, which is what RP2350's heap can spare and what every measurement
 * before X8b used. It is exposed because it, and not the threading, is what
 * bounds Lazy SMP's usefulness here. Takes effect at the next init_tt(). */
extern uint32_t tt_embedded_bytes;

void init_tt(int size_mb);
void free_tt(void);
void clear_tt(void);
void increment_tt_age(void);

// Read from TT
bool read_tt(uint64_t hash_key, int depth, int ply, int alpha, int beta, int *score, Move *best_move);
bool probe_tt_entry(uint64_t hash_key, int *score, Move *best_move);

// Write to TT
void write_tt(uint64_t hash_key, Move best_move, int score, int depth, uint8_t flags);

#endif // TT_H
