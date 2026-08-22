#ifndef LUGALOS_KERNEL_BALLOC_H
#define LUGALOS_KERNEL_BALLOC_H

#include <stdint.h>

/* Buddy allocator (M1, plan/phase12_microkernel_migration.md).
 *
 * `palloc.h` is deliberately page-granular -- correct for task stacks and
 * page tables, wrong for anything smaller, and the gap raw_ideas.md's "True
 * allocator" line and plan/redesign_eval.md both named. This is that
 * allocator, with one constraint neither of those asked for but that M4/M5
 * of the migration plan need: **every block this returns is power-of-two
 * sized and naturally aligned in absolute address space**, the same NAPOT
 * shape a PMP region requires (kernel/mem_domain.h). A generic first-fit
 * heap cannot promise that -- an arbitrary-offset block from a dense arena
 * can never be expressed as a single PMP region, and packs allocations tight
 * enough that an overrun corrupts a live neighbour instead of hitting a gap.
 * A buddy allocator only ever splits a power-of-two range in half, so every
 * block it hands out, at every level, is self-aligned by construction.
 * (Rule 6, plan/phase12_microkernel_migration.md §"Design rules".)
 *
 * ## Arena, not the whole heap
 *
 * balloc does not replace palloc_pages()/palloc_pages_aligned() or their
 * callers -- those are untouched by M1. Instead balloc_init() reserves a
 * fixed, self-aligned slice of the page heap (BALLOC_ARENA_PAGES, itself a
 * power of two so the slice's *base* address is aligned to its own size --
 * without that, "offset-aligned within the arena" would not imply
 * "address-aligned in absolute terms", and the whole NAPOT guarantee above
 * would be false) and buddy-manages only that slice. Deliberately modest for
 * M1 (64 KB): nothing in the tree depends on this allocator yet, so sizing
 * it generously now would just be memory nobody can use, not memory anyone
 * needs. M4/M5 raise it once real callers (driver task stacks, chan
 * endpoint buffers) exist to size it against.
 *
 * ## Floor
 *
 * 32 bytes, matching mem_domain_add()'s own floor -- deliberately the
 * stricter of RP2350's 32-byte and QEMU's 8-byte PMP granule, for the same
 * reason mem_domain.c gives: one rule that is legal on both builds rather
 * than one that is silently wider on the looser target.
 *
 * ## Mechanism
 *
 * The binary-tree-of-largest-free-block scheme (sometimes called "buddy2"):
 * one array, one entry per tree node, each entry holding the size (in
 * minimum-block units) of the largest free block available in that node's
 * subtree. No intrusive pointers written into free blocks (unlike a
 * freelist-based buddy allocator), so nothing about allocation state lives
 * in memory a caller might read before first use -- the whole allocator's
 * state is the one side array. Allocation and free are both O(log N) tree
 * walks; free needs no size argument because the walk that finds the node
 * (see kernel/balloc.c) is exactly the walk that already knows it.
 */

/* Every block returned by balloc_alloc() is at least this large and this
 * aligned, whatever `size` asked for. Matches MEM_DOMAIN's floor
 * (kernel/mem_domain.h) so a request this allocator satisfies is always
 * legal as a PMP region on its own, with no separate rounding step. */
#define BALLOC_MIN_BLOCK 32u

/* Total arena size, in pages. Power of two, so the arena's base address
 * (reserved via palloc_pages_aligned(BALLOC_ARENA_PAGES, BALLOC_ARENA_PAGES))
 * is itself naturally aligned to the arena's full byte size -- see the file
 * comment above for why that is load-bearing rather than tidy.
 *
 * Per-board, from cmake/board-*.cmake, for the reason palloc.h's own
 * CONFIG_PALLOC_MAX_PAGES gives: which board gets which value is a board
 * fact, not something an #ifdef here should decide. 16 pages (64 KB) on the
 * QEMU targets, 4 (16 KB) on RP2350 -- see §1.1 of
 * plan/phase15_memory_reclamation.md, and the two costs below.
 *
 * ## This constant is paid twice, and only one half is lazy
 *
 *   - The arena itself, BALLOC_ARENA_PAGES pages of heap, is reserved on
 *     the first balloc_alloc() and not before (see balloc_init()). A board
 *     that never calls this allocator pays nothing for it.
 *   - g_longest, the tree (kernel/balloc.c), is
 *     `(2 * pages * PAGE_SIZE / BALLOC_MIN_BLOCK - 1) * sizeof(uint16_t)`
 *     bytes of permanent .bss, sized at compile time, present from boot
 *     **whether or not the arena is ever reserved**: 8190 bytes at 16
 *     pages, 2046 at 4. On RP2350 .bss and the heap are the same budget
 *     (palloc_init() starts the heap at _kernel_end), so raising this
 *     constant costs heap even on a boot that never allocates from it.
 *     That is why it is 4 there and not 16. */
#include "lugalos_config.h"
#define BALLOC_ARENA_PAGES ((uint32_t)CONFIG_BALLOC_ARENA_PAGES)

/* Marks the allocator ready to reserve. Must run after palloc_init().
 *
 * Deliberately does NOT reserve the arena (M1 did; §1.1 of
 * plan/phase15_memory_reclamation.md is why it stopped). M1 sized the arena
 * for the callers M4/M5 were expected to bring -- driver task stacks, chan
 * endpoint buffers -- and those callers never arrived: as of this writing
 * the only balloc_alloc() in the tree is kernel/shell.c's `ballocdemo`, and
 * an eager reservation was handing 64 KB of RP2350's 212 KB heap to a
 * command nobody runs outside the QEMU test suite. Reserving on first use
 * instead makes an unused allocator genuinely free rather than nearly free,
 * and means the same thing cannot recur if a future arena is sized for
 * callers that again do not materialise.
 *
 * Call exactly once, from kernel/main.c, the way palloc_init() is: a second
 * call after something has allocated would abandon a live arena. */
void balloc_init(void);

/* Smallest power-of-two block >= max(size, BALLOC_MIN_BLOCK), naturally
 * aligned, zeroed. NULL if `size` exceeds the arena, or no free block of the
 * needed order remains (query balloc_stats() to tell those apart, the same
 * way palloc_extra_stats()'s largest_free_run distinguishes "full" from
 * "fragmented" for the page allocator). */
void *balloc_alloc(uint32_t size);

/* Frees a block returned by balloc_alloc(), coalescing with its buddy (and
 * that buddy's buddy, and so on) wherever the sibling is also fully free.
 * No size parameter: the tree walk that locates the block's owning node is
 * the same walk that would need one to double-check it, so asking for it
 * separately would only be one more way for a caller to get free() wrong. */
void balloc_free(void *ptr);

/* `arena_bytes`: the fixed total, BALLOC_ARENA_PAGES * PAGE_SIZE.
 * `largest_free_bytes`: the biggest single request balloc_alloc() could
 * satisfy right now -- the coalescing counterpart to palloc's
 * largest_free_run, and what a fragmentation-churn test asserts against.
 * Either pointer may be NULL. */
void balloc_stats(uint32_t *arena_bytes, uint32_t *largest_free_bytes);

#endif /* LUGALOS_KERNEL_BALLOC_H */
