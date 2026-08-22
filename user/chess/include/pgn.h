/*
 * PGN save/load for LugalOS chess (14b, plan/phase14_networking_and_host_tooling.md;
 * built in phase 16).
 *
 * Not vendored from LugalChess -- upstream has no PGN support; this is
 * LugalOS's own, written against the vendored engine's Position/Move types.
 *
 * ## Why SAN, and why it is the bulk of this
 *
 * PGN's movetext is Standard Algebraic Notation ("Nf3", "exd5", "O-O",
 * "Qxe7+"), not the long algebraic ("g1f3") the engine formats everywhere
 * else. SAN is *position-dependent* in a way long algebraic is not: naming a
 * move requires knowing which other pieces of the same type could also reach
 * that square, so it cannot be derived from the Move word alone. That is what
 * makes a file another program will actually open, which is the entire point
 * of choosing PGN over a private format.
 */

#ifndef LUGALOS_CHESS_PGN_H
#define LUGALOS_CHESS_PGN_H

#include "defs.h"
#include "position.h"
#include "move.h"

/* Longest SAN string plus terminator: "Qa1xb2+" style with both
 * disambiguators and a promotion is 8 characters ("bxa8=Q#"), and 12 leaves
 * room without anyone having to be clever. */
#define SAN_MAX 12

/* Writes `m` in SAN, as played from `pos`.
 *
 * `pos` must be the position *before* the move -- SAN names a move in terms of
 * what else could have been played instead, so the pre-move position is not a
 * convenience here, it is the input. `m` must be legal in `pos`.
 *
 * `pos` is taken non-const because determining the check/mate suffix means
 * playing the move and looking; it is restored exactly before returning.
 */
void format_move_san(Position *pos, Move m, char out[SAN_MAX]);

/* Resolves a SAN string back to a move legal in `pos`, or 0.
 *
 * Implemented by formatting every legal move and comparing, rather than by
 * parsing SAN's grammar: the two directions can then never disagree about
 * disambiguation or suffixes, which is exactly where a hand-written parser and
 * a hand-written formatter drift apart. Costs a move generation per call,
 * which at load time is nothing.
 */
Move parse_move_san(Position *pos, const char *san);

/* Writes `pos`'s complete game to `path` as PGN, replaying its own move
 * history. Returns 0 on success.
 *
 * `result` is the PGN result tag ("1-0", "0-1", "1/2-1/2", "*").
 * Emits [SetUp]/[FEN] tags when the game did not start from the standard
 * position, so a game begun from a `fen` command reloads exactly.
 */
int pgn_save(const Position *pos, const char *path, const char *result);

/* Loads a PGN written by pgn_save() (or any PGN whose movetext this engine
 * can resolve) into `pos`. Returns 0 on success. */
int pgn_load(Position *pos, const char *path);

/* Round-trips SAN over a set of positions chosen for the cases that break
 * naive implementations (disambiguation by file and by rank, castling,
 * capture-promotion, mate suffix). Returns the failure count, 0 when clean. */
int pgn_selftest(void);

#endif /* LUGALOS_CHESS_PGN_H */
