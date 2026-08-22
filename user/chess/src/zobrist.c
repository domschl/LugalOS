/*
 * Vendored from ~/gith/domschl/LugalChess (engine/src/zobrist.c), H4,
 * plan/phase9_chess_computer.md.
 *
 * Now empty of everything it used to hold. The keys it filled at run time are
 * a mathematical constant -- a fixed xorshift, a fixed seed, 849 numbers that
 * cannot change without changing how positions hash -- so they are generated
 * once by tools/gen_zobrist.py and live in flash as const data
 * (user/chess/include/zobrist_tables.h). See section 3.2 of
 * plan/phase15_memory_reclamation.md, including the measurement showing that
 * reading them from flash costs nothing on this silicon.
 *
 * The file stays so the build's source list and this note have somewhere to
 * live; the translation unit is deliberately empty apart from that.
 */

#include "zobrist.h"

/* ISO C forbids an empty translation unit. */
typedef int zobrist_translation_unit_not_empty;
