/*
 * Vendored from ~/gith/domschl/LugalChess (engine/src/bitboard.c), H4,
 * plan/phase9_chess_computer.md. Two changes from upstream:
 *   - dropped the unused `#include <time.h>` (nothing in this file calls
 *     time()/clock(); LugalOS has no <time.h> at all)
 *   - print_bitboard()'s hash line reformatted: cprintf has no %llx (a
 *     single 'l' length modifier is skipped, not two, so it would read a
 *     64-bit argument as 32-bit and desync everything after it -- see
 *     chess_platform.h), so the hash is split into two 32-bit halves.
 * The USE_MAGIC_BITBOARDS==0 half of this file (magic-number search, whose
 * only printf lived there) never compiles in at all now that defs.h
 * hardcodes that off (H0: magic bitboards need ~840 KB, far past an RP2350
 * board's budget) -- nothing to adapt there.
 */

#include "bitboard.h"

// Global precomputed attack tables
uint64_t pawn_attacks[2][64];
uint64_t knight_attacks[64];
uint64_t king_attacks[64];

// Board file & rank masks
uint64_t file_masks[8];
uint64_t rank_masks[8];

// Clear file masks
const uint64_t not_a_file  = 0xfefefefefefefefeULL;
const uint64_t not_h_file  = 0x7f7f7f7f7f7f7f7fULL;
const uint64_t not_ab_file = 0xfcfcfcfcfcfcfcfcULL;
const uint64_t not_gh_file = 0x3f3f3f3f3f3f3f3fULL;

// Helper to mask pawn attacks (without shifting out of bounds)
static uint64_t mask_pawn_attacks(int color, int square) {
    uint64_t attacks = 0ULL;
    uint64_t bit = (1ULL << square);
    if (color == WHITE) {
        if (square < 56) {
            if ((bit << 7) & not_h_file) attacks |= (bit << 7);
            if ((bit << 9) & not_a_file) attacks |= (bit << 9);
        }
    } else {
        if (square > 7) {
            if ((bit >> 7) & not_a_file) attacks |= (bit >> 7);
            if ((bit >> 9) & not_h_file) attacks |= (bit >> 9);
        }
    }
    return attacks;
}

// Helper to mask knight attacks
static uint64_t mask_knight_attacks(int square) {
    uint64_t attacks = 0ULL;
    uint64_t bit = (1ULL << square);

    if (square + 17 < 64 && ((bit << 17) & not_a_file)) attacks |= (bit << 17);
    if (square + 15 < 64 && ((bit << 15) & not_h_file)) attacks |= (bit << 15);
    if (square + 10 < 64 && ((bit << 10) & not_ab_file)) attacks |= (bit << 10);
    if (square + 6 < 64 && ((bit << 6) & not_gh_file)) attacks |= (bit << 6);

    if (square >= 17 && ((bit >> 17) & not_h_file)) attacks |= (bit >> 17);
    if (square >= 15 && ((bit >> 15) & not_a_file)) attacks |= (bit >> 15);
    if (square >= 10 && ((bit >> 10) & not_gh_file)) attacks |= (bit >> 10);
    if (square >= 6 && ((bit >> 6) & not_ab_file)) attacks |= (bit >> 6);

    return attacks;
}

// Helper to mask king attacks
static uint64_t mask_king_attacks(int square) {
    uint64_t attacks = 0ULL;
    uint64_t bit = (1ULL << square);

    if (square + 8 < 64) attacks |= (bit << 8);
    if (square + 9 < 64 && ((bit << 9) & not_a_file)) attacks |= (bit << 9);
    if (square + 1 < 64 && ((bit << 1) & not_a_file)) attacks |= (bit << 1);
    if (square >= 7 && ((bit >> 7) & not_a_file)) attacks |= (bit >> 7);

    if (square >= 8) attacks |= (bit >> 8);
    if (square >= 9 && ((bit >> 9) & not_h_file)) attacks |= (bit >> 9);
    if (square >= 1 && ((bit >> 1) & not_h_file)) attacks |= (bit >> 1);
    if (square + 7 < 64 && ((bit << 7) & not_h_file)) attacks |= (bit << 7);

    return attacks;
}

// Precomputed on-the-fly sliding attacks
uint64_t bishop_attacks_on_the_fly(int square, uint64_t occupied) {
    uint64_t attacks = 0ULL;
    int r, f;
    int tr = square / 8;
    int tf = square % 8;

    for (r = tr + 1, f = tf + 1; r <= 7 && f <= 7; r++, f++) {
        attacks |= (1ULL << (r * 8 + f));
        if (occupied & (1ULL << (r * 8 + f))) break;
    }
    for (r = tr + 1, f = tf - 1; r <= 7 && f >= 0; r++, f--) {
        attacks |= (1ULL << (r * 8 + f));
        if (occupied & (1ULL << (r * 8 + f))) break;
    }
    for (r = tr - 1, f = tf + 1; r >= 0 && f <= 7; r--, f++) {
        attacks |= (1ULL << (r * 8 + f));
        if (occupied & (1ULL << (r * 8 + f))) break;
    }
    for (r = tr - 1, f = tf - 1; r >= 0 && f >= 0; r--, f--) {
        attacks |= (1ULL << (r * 8 + f));
        if (occupied & (1ULL << (r * 8 + f))) break;
    }
    return attacks;
}

uint64_t rook_attacks_on_the_fly(int square, uint64_t occupied) {
    uint64_t attacks = 0ULL;
    int r, f;
    int tr = square / 8;
    int tf = square % 8;

    for (r = tr + 1; r <= 7; r++) {
        attacks |= (1ULL << (r * 8 + tf));
        if (occupied & (1ULL << (r * 8 + tf))) break;
    }
    for (r = tr - 1; r >= 0; r--) {
        attacks |= (1ULL << (r * 8 + tf));
        if (occupied & (1ULL << (r * 8 + tf))) break;
    }
    for (f = tf + 1; f <= 7; f++) {
        attacks |= (1ULL << (tr * 8 + f));
        if (occupied & (1ULL << (tr * 8 + f))) break;
    }
    for (f = tf - 1; f >= 0; f--) {
        attacks |= (1ULL << (tr * 8 + f));
        if (occupied & (1ULL << (tr * 8 + f))) break;
    }
    return attacks;
}

#if USE_MAGIC_BITBOARDS

// Relevant occupancy bits for sliding pieces
static const int bishop_relevant_bits[64] = {
    6, 5, 5, 5, 5, 5, 5, 6,
    5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 7, 7, 7, 7, 5, 5,
    5, 5, 7, 9, 9, 7, 5, 5,
    5, 5, 7, 9, 9, 7, 5, 5,
    5, 5, 7, 7, 7, 7, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5,
    6, 5, 5, 5, 5, 5, 5, 6
};

static const int rook_relevant_bits[64] = {
    12, 11, 11, 11, 11, 11, 11, 12,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    11, 10, 10, 10, 10, 10, 10, 11,
    12, 11, 11, 11, 11, 11, 11, 12
};

// Found magic numbers will be stored here
static uint64_t bishop_magics[64];
static uint64_t rook_magics[64];

// Masks for occupancies
static uint64_t bishop_masks[64];
static uint64_t rook_masks[64];

// Flat arrays containing the attack lookup entries
static uint64_t bishop_attacks_table[5248];
static uint64_t rook_attacks_table[102400];

// Pointers to the start of each square's lookup block
static uint64_t *bishop_attacks[64];
static uint64_t *rook_attacks[64];

// Helper to mask relevant bishop occupancy squares (excluding board edges)
static uint64_t mask_bishop_occupancy(int square) {
    uint64_t attacks = 0ULL;
    int r, f;
    int tr = square / 8;
    int tf = square % 8;
    for (r = tr + 1, f = tf + 1; r < 7 && f < 7; r++, f++) attacks |= (1ULL << (r * 8 + f));
    for (r = tr + 1, f = tf - 1; r < 7 && f > 0; r++, f--) attacks |= (1ULL << (r * 8 + f));
    for (r = tr - 1, f = tf + 1; r > 0 && f < 7; r--, f++) attacks |= (1ULL << (r * 8 + f));
    for (r = tr - 1, f = tf - 1; r > 0 && f > 0; r--, f--) attacks |= (1ULL << (r * 8 + f));
    return attacks;
}

// Helper to mask relevant rook occupancy squares (excluding board edges)
static uint64_t mask_rook_occupancy(int square) {
    uint64_t attacks = 0ULL;
    int r, f;
    int tr = square / 8;
    int tf = square % 8;
    for (r = tr + 1; r < 7; r++) attacks |= (1ULL << (r * 8 + tf));
    for (r = tr - 1; r > 0; r--) attacks |= (1ULL << (r * 8 + tf));
    for (f = tf + 1; f < 7; f++) attacks |= (1ULL << (tr * 8 + f));
    for (f = tf - 1; f > 0; f--) attacks |= (1ULL << (tr * 8 + f));
    return attacks;
}

// Generates the index for a given occupancy setting
static uint64_t set_occupancy(int index, int bits_in_mask, uint64_t attack_mask) {
    uint64_t occupancy = 0ULL;
    for (int i = 0; i < bits_in_mask; i++) {
        int sq = pop_lsb(&attack_mask);
        if (index & (1 << i)) {
            occupancy |= (1ULL << sq);
        }
    }
    return occupancy;
}

// PRNG for magic search
static uint64_t magic_prng_state = 1802ULL;
static uint64_t magic_prng_next(void) {
    magic_prng_state ^= magic_prng_state >> 12;
    magic_prng_state ^= magic_prng_state << 25;
    magic_prng_state ^= magic_prng_state >> 27;
    return magic_prng_state * 2685821657736338717ULL;
}

static uint64_t generate_sparse_magic(void) {
    return magic_prng_next() & magic_prng_next() & magic_prng_next();
}

// Dynamically find a magic number for a square
static uint64_t find_magic_number(int square, int relevant_bits, bool is_bishop) {
    static uint64_t occupancies[4096];
    static uint64_t attacks[4096];
    static uint64_t used_attacks[4096];
    static bool used_flag[4096];

    uint64_t mask = is_bishop ? mask_bishop_occupancy(square) : mask_rook_occupancy(square);
    int cnt = 1 << relevant_bits;

    for (int i = 0; i < cnt; i++) {
        occupancies[i] = set_occupancy(i, relevant_bits, mask);
        attacks[i] = is_bishop ? bishop_attacks_on_the_fly(square, occupancies[i])
                               : rook_attacks_on_the_fly(square, occupancies[i]);
    }

    // Try finding magic
    for (int iter = 0; iter < 10000000; iter++) {
        uint64_t magic = generate_sparse_magic();

        memset(used_flag, 0, sizeof(bool) * cnt);
        bool fail = false;

        for (int i = 0; i < cnt; i++) {
            int idx = (int)((occupancies[i] * magic) >> (64 - relevant_bits));
            if (!used_flag[idx]) {
                used_flag[idx] = true;
                used_attacks[idx] = attacks[i];
            } else if (used_attacks[idx] != attacks[i]) {
                fail = true;
                break;
            }
        }
        if (!fail) {
            return magic;
        }
    }
    printf("CRITICAL ERROR: Failed to find magic for square %d (is_bishop: %d)\n", square, is_bishop);
    return 0;
}


static void init_magic_bitboards(void) {
    int bishop_offset = 0;
    int rook_offset = 0;

    for (int sq = 0; sq < 64; sq++) {
        // Bishop Setup
        bishop_masks[sq] = mask_bishop_occupancy(sq);
        bishop_attacks[sq] = &bishop_attacks_table[bishop_offset];
        int bishop_bits = bishop_relevant_bits[sq];
        int bishop_cnt = 1 << bishop_bits;

        bishop_magics[sq] = find_magic_number(sq, bishop_bits, true);

        for (int i = 0; i < bishop_cnt; i++) {
            uint64_t occ = set_occupancy(i, bishop_bits, bishop_masks[sq]);
            uint64_t magic_idx = (occ * bishop_magics[sq]) >> (64 - bishop_bits);
            bishop_attacks[sq][magic_idx] = bishop_attacks_on_the_fly(sq, occ);
        }
        bishop_offset += bishop_cnt;

        // Rook Setup
        rook_masks[sq] = mask_rook_occupancy(sq);
        rook_attacks[sq] = &rook_attacks_table[rook_offset];
        int rook_bits = rook_relevant_bits[sq];
        int rook_cnt = 1 << rook_bits;

        rook_magics[sq] = find_magic_number(sq, rook_bits, false);

        for (int i = 0; i < rook_cnt; i++) {
            uint64_t occ = set_occupancy(i, rook_bits, rook_masks[sq]);
            uint64_t magic_idx = (occ * rook_magics[sq]) >> (64 - rook_bits);
            rook_attacks[sq][magic_idx] = rook_attacks_on_the_fly(sq, occ);
        }
        rook_offset += rook_cnt;
    }
}
#endif // USE_MAGIC_BITBOARDS

// Sliding attack getters
uint64_t get_bishop_attacks(int square, uint64_t occupied) {
#if USE_MAGIC_BITBOARDS
    uint64_t occ = occupied & bishop_masks[square];
    uint64_t idx = (occ * bishop_magics[square]) >> (64 - bishop_relevant_bits[square]);
    return bishop_attacks[square][idx];
#else
    return bishop_attacks_on_the_fly(square, occupied);
#endif
}

uint64_t get_rook_attacks(int square, uint64_t occupied) {
#if USE_MAGIC_BITBOARDS
    uint64_t occ = occupied & rook_masks[square];
    uint64_t idx = (occ * rook_magics[square]) >> (64 - rook_relevant_bits[square]);
    return rook_attacks[square][idx];
#else
    return rook_attacks_on_the_fly(square, occupied);
#endif
}

uint64_t get_queen_attacks(int square, uint64_t occupied) {
    return get_bishop_attacks(square, occupied) | get_rook_attacks(square, occupied);
}

// Non-sliding attack getters
uint64_t get_pawn_attacks(int color, int square) {
    return pawn_attacks[color][square];
}

uint64_t get_knight_attacks(int square) {
    return knight_attacks[square];
}

uint64_t get_king_attacks(int square) {
    return king_attacks[square];
}

// Global initialization
void init_bitboards(void) {
    // 1. Initialize file & rank masks
    for (int f = 0; f < 8; f++) {
        file_masks[f] = 0ULL;
        for (int r = 0; r < 8; r++) {
            file_masks[f] |= (1ULL << (r * 8 + f));
        }
    }
    for (int r = 0; r < 8; r++) {
        rank_masks[r] = 0ULL;
        for (int f = 0; f < 8; f++) {
            rank_masks[r] |= (1ULL << (r * 8 + f));
        }
    }

    // 2. Precompute simple piece attacks
    for (int sq = 0; sq < 64; sq++) {
        pawn_attacks[WHITE][sq] = mask_pawn_attacks(WHITE, sq);
        pawn_attacks[BLACK][sq] = mask_pawn_attacks(BLACK, sq);
        knight_attacks[sq] = mask_knight_attacks(sq);
        king_attacks[sq] = mask_king_attacks(sq);
    }

    // 3. Initialize slider magics if configured
#if USE_MAGIC_BITBOARDS
    init_magic_bitboards();
#endif
}

// Debug utility to print a visual board of the bitboard
void print_bitboard(uint64_t bb) {
    printf("\n");
    for (int r = 7; r >= 0; r--) {
        printf("%d  ", r + 1);
        for (int f = 0; f < 8; f++) {
            int sq = r * 8 + f;
            printf("%d ", get_bit(bb, sq) ? 1 : 0);
        }
        printf("\n");
    }
    printf("   a b c d e f g h\n\n");
    printf("Bitboard: 0x%x%08x\n\n", (unsigned int)(bb >> 32), (unsigned int)bb);
}
