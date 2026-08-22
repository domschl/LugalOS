/*
 * Vendored from ~/gith/domschl/LugalChess (engine/include/zobrist.h), H4,
 * plan/phase9_chess_computer.md. Changed from upstream (section 3.2,
 * plan/phase15_memory_reclamation.md): the keys are const data in flash
 * instead of .bss filled at run time, and init_zobrist() is gone with
 * nothing left for it to do.
 */

#ifndef ZOBRIST_H
#define ZOBRIST_H

#include "defs.h"

/* Zobrist keys. Precomputed (tools/gen_zobrist.py) and const, so they live in
 * flash rather than costing 6.8 KB of RAM from boot -- which on RP2350 is 6.8
 * KB of heap, since .bss and the heap are the same budget there.
 *
 * The values are identical to what init_zobrist() used to compute: the
 * generator replicates that function's PRNG and draw order exactly, verified
 * against the running board before the switch.
 *
 * Reached through the original names, so every call site in position.c is
 * untouched. Macros rather than `static const uint64_t (*zobrist_pieces)[2][64]
 * = ...` aliases because the array *types* must survive -- the engine indexes
 * these as three-dimensional arrays, and an alias would silently decay. */
#include "zobrist_tables.h"

#define zobrist_pieces     zobrist_pieces_flash
#define zobrist_castling   zobrist_castling_flash
#define zobrist_en_passant zobrist_en_passant_flash
#define zobrist_side       zobrist_side_flash

#endif // ZOBRIST_H
