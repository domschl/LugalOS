/*
 * Vendored from ~/gith/domschl/LugalChess (engine/include/search.h), H4,
 * plan/phase9_chess_computer.md. search_pools_init()/search_pools_free()
 * are LugalOS-only additions, J0/J1 (plan/phase10_chess_completion.md) --
 * see search.c's header comment.
 */

#ifndef SEARCH_H
#define SEARCH_H

#include "defs.h"
#include "position.h"

#define MATE_VALUE 29000
#define INFINITY_VALUE 30000

// Search controls
extern int max_search_depth;
extern long max_search_time_ms;
extern long start_search_time_ms;
extern bool stop_search;
extern long nodes_searched;

/* Allocates the PV/quiescence move-list scratch space from the page
 * allocator on first call; a no-op returning true on every call after the
 * first. Must be called (and must succeed) before search_position() or
 * get_book_move() run -- chess_ui.c's chess_ensure_init() is the one call
 * site. Returns false only on genuine page-allocator exhaustion. */
bool search_pools_init(void);

/* Releases what search_pools_init() allocated, back to the page allocator.
 * Safe to call whether or not search_pools_init() ever ran (a no-op if
 * not). chess_ui.c's chess_session_end() is the one call site. */
void search_pools_free(void);

void search_position(Position *pos, int depth, int time_limit_ms);
int quiescence(Position *pos, int ply, int alpha, int beta);
int pv_search(Position *pos, int depth, int ply, int alpha, int beta, bool null_move_allowed);
const char *get_book_line_name(const Position *pos);

#endif // SEARCH_H
