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
 * comment above for why that is load-bearing rather than tidy. 16 pages =
 * 64 KB on every target; see the file comment for why this is deliberately
 * small for M1. */
#define BALLOC_ARENA_PAGES 16u

/* Reserves the arena from the page allocator and initializes the buddy tree.
 * Must run after palloc_init(). Idempotent only in the sense that calling it
 * twice reserves two arenas and leaks the tracking state of the first --
 * call it exactly once, from kernel/main.c, the way palloc_init() itself is. */
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
