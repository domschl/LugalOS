/*
 * Vendored from ~/gith/domschl/LugalChess (engine/include/bitboard.h), H4,
 * plan/phase9_chess_computer.md, with one real change: on RISC-V without
 * the Zbb extension (every LugalOS target -- RP2350's march is
 * rv32imac_zicsr_zbs, Zbs only, no hardware ctz/clz/popcount), GCC lowers
 * __builtin_ctz/__builtin_popcount to a libgcc call (__ctzsi2, which
 * itself promotes to the 64-bit __ctzdi2 on this backend). That function's
 * reference to libgcc's __clz_tab failed to relocate in the full kernel
 * link ("relocation truncated to fit: R_RISCV_HI20") on RV64_MMU -- a
 * genuine toolchain/multilib defect, confirmed with a two-line reproduction
 * outside this codebase entirely, not something introduced by how this
 * function is written. The `#elif defined(__riscv)` branch below forces the
 * already-present software fallback (zero library calls, confirmed via the
 * same reproduction) instead of the GCC-builtin branch on every RISC-V
 * target, sidestepping the defect uniformly rather than special-casing one
 * board.
 */

#ifndef BITBOARD_H
#define BITBOARD_H

#include "defs.h"

// Macro/Inline function definitions for bit manipulations
#define get_bit(bb, square) (((bb) >> (square)) & 1ULL)
#define set_bit(bb, square) ((bb) |= (1ULL << (square)))
#define clear_bit(bb, square) ((bb) &= ~(1ULL << (square)))

// Cross-platform compiler built-ins for trailing zero count (LSB) and population count
#if defined(__riscv)
    // Forced to the plain software fallback (see this file's header
    // comment) -- checked ahead of __GNUC__ since GCC is also how this gets
    // built on every LugalOS target.
    static inline int count_bits(uint64_t bb) {
        int count = 0;
        while (bb) {
            count++;
            bb &= bb - 1; // clear the least significant bit
        }
        return count;
    }
    static inline int get_lsb(uint64_t bb) {
        if (bb == 0) return NO_SQ;
        int index = 0;
        if ((bb & 0xFFFFFFFFULL) == 0) { bb >>= 32; index += 32; }
        if ((bb & 0xFFFFULL) == 0) { bb >>= 16; index += 16; }
        if ((bb & 0xFFULL) == 0) { bb >>= 8; index += 8; }
        if ((bb & 0xFULL) == 0) { bb >>= 4; index += 4; }
        if ((bb & 0x3ULL) == 0) { bb >>= 2; index += 2; }
        if ((bb & 0x1ULL) == 0) { index += 1; }
        return index;
    }
#elif defined(__GNUC__) || defined(__clang__)
    static inline int count_bits(uint64_t bb) {
        uint32_t low = (uint32_t)bb;
        uint32_t high = (uint32_t)(bb >> 32);
        return __builtin_popcount(low) + __builtin_popcount(high);
    }
    static inline int get_lsb(uint64_t bb) {
        if (bb == 0ULL) return NO_SQ;
        uint32_t low = (uint32_t)bb;
        if (low != 0U) {
            return __builtin_ctz(low);
        }
        uint32_t high = (uint32_t)(bb >> 32);
        if (high != 0U) {
            return __builtin_ctz(high) + 32;
        }
        return NO_SQ;
    }
#elif defined(_MSC_VER)
    #include <intrin.h>
    static inline int count_bits(uint64_t bb) {
        return (int)__popcnt64(bb);
    }
    static inline int get_lsb(uint64_t bb) {
        if (bb == 0) return NO_SQ;
        unsigned long index;
        _BitScanForward64(&index, bb);
        return (int)index;
    }
#else
    // Software fallback
    static inline int count_bits(uint64_t bb) {
        int count = 0;
        while (bb) {
            count++;
            bb &= bb - 1; // clear the least significant bit
        }
        return count;
    }
    static inline int get_lsb(uint64_t bb) {
        if (bb == 0) return 64;
        int index = 0;
        if ((bb & 0xFFFFFFFFULL) == 0) { bb >>= 32; index += 32; }
        if ((bb & 0xFFFFULL) == 0) { bb >>= 16; index += 16; }
        if ((bb & 0xFFULL) == 0) { bb >>= 8; index += 8; }
        if ((bb & 0xFULL) == 0) { bb >>= 4; index += 4; }
        if ((bb & 0x3ULL) == 0) { bb >>= 2; index += 2; }
        if ((bb & 0x1ULL) == 0) { index += 1; }
        return index;
    }
#endif

// Pop least significant bit and return its index
static inline int pop_lsb(uint64_t *bb) {
    int lsb = get_lsb(*bb);
    *bb &= *bb - 1;
    return lsb;
}

// Global precomputed attack tables
extern uint64_t pawn_attacks[2][64];
extern uint64_t knight_attacks[64];
extern uint64_t king_attacks[64];

// Board file & rank masks
extern uint64_t file_masks[8];
extern uint64_t rank_masks[8];

// Clear file masks (for pawn/knight jumps and moves)
extern const uint64_t not_a_file;
extern const uint64_t not_h_file;
extern const uint64_t not_ab_file;
extern const uint64_t not_gh_file;

// Initialization
void init_bitboards(void);

// Attack getters
uint64_t get_pawn_attacks(int color, int square);
uint64_t get_knight_attacks(int square);
uint64_t get_king_attacks(int square);
uint64_t get_bishop_attacks(int square, uint64_t occupied);
uint64_t get_rook_attacks(int square, uint64_t occupied);
uint64_t get_queen_attacks(int square, uint64_t occupied);

// Print utilities
void print_bitboard(uint64_t bb);

// Reference on-the-fly sliding attacks (used directly if USE_MAGIC_BITBOARDS is 0, or during Magic verification)
uint64_t bishop_attacks_on_the_fly(int square, uint64_t occupied);
uint64_t rook_attacks_on_the_fly(int square, uint64_t occupied);

#endif // BITBOARD_H
