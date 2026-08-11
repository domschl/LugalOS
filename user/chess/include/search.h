/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/search.h),
 * H4, plan/phase9_chess_computer.md.
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

void search_position(Position *pos, int depth, int time_limit_ms);
int quiescence(Position *pos, int ply, int alpha, int beta);
int pv_search(Position *pos, int depth, int ply, int alpha, int beta, bool null_move_allowed);
const char *get_book_line_name(const Position *pos);

#endif // SEARCH_H
