#ifndef LUGALOS_KERNEL_SCRATCH_H
#define LUGALOS_KERNEL_SCRATCH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* On-demand working memory (§3.1, plan/phase15_memory_reclamation.md).
 *
 * The pattern this generalises had been reinvented four times before it was
 * named: user/chibicc/pools.c's compile-time arena, user/chess/src/search.c's
 * move-list pools, chess_ui.c's scratch Position, and user/lisp/lisp.c's file
 * buffers. Each time the reasoning was the same, and each time it was written
 * out again from scratch.
 *
 * ## The rule
 *
 *   A static buffer over ~1 KB that is **not live at boot** and **not on a
 *   hot path** belongs here rather than in .bss.
 *
 * Both halves matter. "Not live at boot" is what makes it reclaimable at all.
 * "Not on a hot path" is the limit: this costs a bitmap scan in
 * palloc_pages(), which is nothing next to filesystem I/O or entering U-mode,
 * and quite a lot next to servicing a 9P frame. Buffers that a request-serving
 * loop touches per message stay static on purpose -- see fs/p9_link.c, where
 * the client-side one-shot buffers moved here and the server-side ones
 * deliberately did not.
 *
 * The reason the rule pays on this project specifically: on RP2350 .bss and
 * the heap are the same budget (palloc_init() starts the heap at _kernel_end),
 * so a kilobyte of .bss that serves an idle subsystem is a kilobyte no *other*
 * subsystem can use. Since chess, cc, ed, networking and Lisp-heavy work are
 * mutually exclusive in practice, moving them here hands the memory to
 * whichever one is actually running.
 *
 * ## Shape
 *
 * The page count is stored, not recomputed at release. That is the one thing
 * this exists to make impossible to get wrong: tt.c and search.c both carry a
 * comment explaining that they keep their own page count for exactly this
 * reason, having each arrived at it independently.
 *
 * Allocate per call rather than sharing one buffer between call sites. It
 * costs nothing extra and makes re-entrancy correct by construction -- see
 * §2.5 of the plan, where a single shared buffer would have let a nested
 * `load` overwrite the text its caller was still evaluating.
 */

typedef struct {
    void    *base;   /* NULL when nothing is held */
    uint32_t pages;
} scratch_t;

/* Rounds `bytes` up to whole pages and claims them, zeroed. Returns false and
 * leaves `s` empty if the heap cannot place the run; every caller must handle
 * that -- on the board it is a real outcome, not a formality. Safe to call on
 * a scratch_t that is already zeroed; not safe to call twice without an
 * intervening release (the first block would be leaked). */
bool scratch_acquire(scratch_t *s, uint32_t bytes);

/* Returns the pages and clears `s`. A no-op on an empty scratch_t, so an
 * error path may call it unconditionally. */
void scratch_release(scratch_t *s);

#endif /* LUGALOS_KERNEL_SCRATCH_H */
