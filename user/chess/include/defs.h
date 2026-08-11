/*
 * Vendored from ~/gith/domschl/LugalChess (engine/include/defs.h), H4,
 * plan/phase9_chess_computer.md. <stdio.h>/<stdlib.h> don't exist on
 * LugalOS's freestanding build -- chess_platform.h stands in for both, and
 * <string.h> is LugalOS's own (libc/include/string.h). USE_MAGIC_BITBOARDS
 * hardcoded off rather than left as a compile-time override: magic
 * bitboards need ~840 KB for the attack tables, far past what an RP2350
 * board can spare (see H0, plan/phase9_chess_computer.md).
 */

#ifndef DEFS_H
#define DEFS_H

#include <stdint.h>
#include <stdbool.h>
#include "chess_platform.h"
#include "version.h"

#define USE_MAGIC_BITBOARDS 0

// Board square mapping (Little-Endian Rank-File mapping: A1=0 ... H8=63)
enum {
    A1, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8, NO_SQ = 64
};

// Files
enum {
    FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H
};

// Ranks
enum {
    RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8
};


// Colors
enum {
    WHITE, BLACK, BOTH
};

// Pieces
enum {
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE
};

// Castling rights (binary representation)
// WK = 1 (0001), WQ = 2 (0010), BK = 4 (0100), BQ = 8 (1000)
enum {
    WK_CASTLE = 1,
    WQ_CASTLE = 2,
    BK_CASTLE = 4,
    BQ_CASTLE = 8
};

// Move encoding representation (16-bit integer)
// 0000 0000 0011 1111 : source square (0 to 63)
// 0000 1111 1100 0000 : destination square (0 to 63)
// 0011 0000 0000 0000 : promotion piece (Knight=0, Bishop=1, Rook=2, Queen=3)
// 0100 0000 0000 0000 : capture flag
// 1000 0000 0000 0000 : special moves flags (double pawn push, en-passant, castling, promotion flag)
// We will define specific bit masks and functions for move handling in move.h.

typedef uint16_t Move;

#if defined(LUGALCHESS_EMBEDDED)
#define MAX_PLYS 256
#else
#define MAX_PLYS 2048
#endif
#define MAX_MOVES 256

#define MATE_VALUE 29000
#define INFINITY_VALUE 30000


#endif // DEFS_H
