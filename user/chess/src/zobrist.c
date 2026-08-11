/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/src/zobrist.c),
 * H4, plan/phase9_chess_computer.md. Self-contained xorshift PRNG, no
 * platform dependency at all.
 */

#include "zobrist.h"

uint64_t zobrist_pieces[6][2][64];
uint64_t zobrist_castling[16];
uint64_t zobrist_en_passant[64];
uint64_t zobrist_side;

static uint64_t prng_state = 1802ULL;

static uint64_t prng_next(void) {
    prng_state ^= prng_state >> 12;
    prng_state ^= prng_state << 25;
    prng_state ^= prng_state >> 27;
    return prng_state * 2685821657736338717ULL;
}

void init_zobrist(void) {
    // Deterministic key generation using Xorshift PRNG
    for (int p = 0; p < 6; p++) {
        for (int c = 0; c < 2; c++) {
            for (int sq = 0; sq < 64; sq++) {
                zobrist_pieces[p][c][sq] = prng_next();
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        zobrist_castling[i] = prng_next();
    }

    for (int sq = 0; sq < 64; sq++) {
        zobrist_en_passant[sq] = prng_next();
    }

    zobrist_side = prng_next();
}
