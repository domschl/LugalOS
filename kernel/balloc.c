#include "kernel/balloc.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include <string.h>
#include <stdbool.h>

/* See kernel/include/kernel/balloc.h for the rationale.
 *
 * The tree: node 0 is the root (the whole arena); node i's children are
 * 2i+1 and 2i+2. g_longest[i] holds the size, in BALLOC_MIN_BLOCK units, of
 * the largest free block anywhere in node i's subtree -- 0 means "nothing
 * free in this subtree at all". Leaves are the last NUM_LEAVES entries. This
 * is the standard "buddy2" scheme: no freelist, no pointers written into
 * free memory, allocation and free are both an O(log N) walk of this one
 * array. */

#define NUM_LEAVES ((BALLOC_ARENA_PAGES * (uint32_t)PAGE_SIZE) / BALLOC_MIN_BLOCK)
#define NODE_COUNT (2u * NUM_LEAVES - 1u)
#define ARENA_BYTES (NUM_LEAVES * BALLOC_MIN_BLOCK)

_Static_assert((NUM_LEAVES & (NUM_LEAVES - 1u)) == 0,
              "BALLOC_ARENA_PAGES * PAGE_SIZE / BALLOC_MIN_BLOCK must be a "
              "power of two, or the tree below is not a real binary buddy tree");

static uint16_t  g_longest[NODE_COUNT];
static uintptr_t g_arena_base;
static bool      g_ready;

/* Guards g_longest[] -- the buddy tree -- and nothing else (S5).
 *
 * Converted despite having no runtime caller today outside ballocdemo:
 * that is exactly the moment to do it. This is a general allocator handing
 * out self-aligned, PMP-usable blocks, so the next caller is a driver or a
 * loader, and it will not think about locking any more than the three
 * subsystems that already had to have it retrofitted did. */
static spinlock_t g_balloc_lock;

static inline uint32_t node_left(uint32_t i)   { return 2u * i + 1u; }
static inline uint32_t node_right(uint32_t i)  { return 2u * i + 2u; }
static inline uint32_t node_parent(uint32_t i) { return (i - 1u) / 2u; }

static inline bool is_pow2(uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }

static uint32_t next_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

void balloc_init(void) {
    /* No reservation here -- see the header. This only establishes the
     * "nothing reserved yet" state that balloc_alloc() reserves out of, so
     * that the ordering contract with palloc_init() stays where it was and
     * the boot log still says the allocator exists. */
    g_ready = false;
    g_arena_base = 0;
    printk("[BAlloc] Buddy allocator: %u KB arena on first use (floor %u B)\n",
           ARENA_BYTES / 1024, BALLOC_MIN_BLOCK);
}

/* Claims the arena and builds the tree. Called from balloc_alloc() on the
 * first allocation and never again while that arena is held. */
static bool balloc_reserve(void) {
    /* Self-aligned: BALLOC_ARENA_PAGES is a power of two, so this call asks
     * palloc for a run whose *address*, not just its offset, is a multiple
     * of ARENA_BYTES -- the property every block handed out below relies on
     * (see the header's "Arena, not the whole heap" section). */
    void *arena = palloc_pages_aligned(BALLOC_ARENA_PAGES, BALLOC_ARENA_PAGES);
    if (!arena) {
        /* Reachable now in a way it was not when this ran at boot: the heap
         * is no longer empty by construction at this point, so this can mean
         * "too fragmented to place an ARENA_BYTES-aligned run" as well as
         * "full". Left as a failed allocation rather than a retry -- the
         * next balloc_alloc() will try again on its own. */
        printk("[BAlloc] Could not reserve %u-page arena; buddy allocator offline\n",
               BALLOC_ARENA_PAGES);
        return false;
    }
    g_arena_base = (uintptr_t)arena;

    uint32_t node_size = NUM_LEAVES * 2u;
    for (uint32_t i = 0; i < NODE_COUNT; i++) {
        if (is_pow2(i + 1u)) node_size /= 2u;
        g_longest[i] = (uint16_t)node_size;
    }
    g_ready = true;
    printk("[BAlloc] Reserved %u KB arena at %p (floor %u B)\n",
           ARENA_BYTES / 1024, arena, BALLOC_MIN_BLOCK);
    return true;
}

void *balloc_alloc(uint32_t size) {
    /* Size checked before reserving, so a request this allocator could never
     * satisfy does not drag an arena out of the heap on its way to NULL. */
    if (size == 0 || size > ARENA_BYTES) return NULL;
    if (!g_ready && !balloc_reserve()) return NULL;
    if (size < BALLOC_MIN_BLOCK) size = BALLOC_MIN_BLOCK;

    /* Safe from next_pow2()'s wraparound on a huge `size`: the ARENA_BYTES
     * check above already rejected anything that could overflow here. */
    uint32_t leaves_needed = next_pow2(size) / BALLOC_MIN_BLOCK;

    /* The lock starts here, not at the top of the function (S5,
     * plan/phase22_smp_locking_foundation.md). Everything above it either
     * touches nothing shared or is balloc_reserve(), which calls
     * palloc_pages_aligned() -- itself lock-taking since S4 -- and printk(),
     * which can block. A spinlock_t across either would be the deadlock
     * kernel/lock.h warns about; across both it would be two.
     *
     * From here down is pure tree arithmetic over g_longest[] with no call
     * out of it, which is the same shape that made palloc's own conversion
     * straightforward. The check and the descent must be one indivisible
     * step: g_longest[0] saying there is room, and the descent that acts on
     * it, are exactly the read-then-act pair a second hart can invalidate
     * between. */
    uintptr_t bflags = spin_lock_irqsave(&g_balloc_lock);

    if (g_longest[0] < leaves_needed) {
        spin_unlock_irqrestore(&g_balloc_lock, bflags);
        return NULL; /* not enough space anywhere */
    }

    /* Descend from the root, always toward a child with enough room, until
     * the subtree size matches the request exactly -- that node IS the
     * block. */
    uint32_t index = 0;
    uint32_t node_size;
    for (node_size = NUM_LEAVES; node_size != leaves_needed; node_size /= 2u) {
        uint32_t l = node_left(index);
        index = (g_longest[l] >= leaves_needed) ? l : node_right(index);
    }
    g_longest[index] = 0; /* this subtree is now fully allocated */
    uint32_t offset_leaves = (index + 1u) * node_size - NUM_LEAVES;

    /* Propagate the reduced availability back up to the root. */
    while (index != 0) {
        index = node_parent(index);
        uint32_t l = g_longest[node_left(index)];
        uint32_t r = g_longest[node_right(index)];
        g_longest[index] = (uint16_t)(l > r ? l : r);
    }

    spin_unlock_irqrestore(&g_balloc_lock, bflags);

    void *ptr = (void *)(g_arena_base + (uintptr_t)offset_leaves * BALLOC_MIN_BLOCK);
    /* Zeroed outside the lock, for palloc_pages()'s reason: the block is
     * already claimed, so nobody else can be looking at it, and holding a
     * spinlock across a memset of up to ARENA_BYTES would be the longest
     * critical section in the kernel by a wide margin. */
    memset(ptr, 0, (size_t)node_size * BALLOC_MIN_BLOCK);
    return ptr;
}

void balloc_free(void *ptr) {
    if (!g_ready || !ptr) return;
    uintptr_t addr = (uintptr_t)ptr;
    if (addr < g_arena_base) return;
    uintptr_t off = addr - g_arena_base;
    if (off % BALLOC_MIN_BLOCK) return; /* not a block boundary: not ours */
    uint32_t offset_leaves = (uint32_t)(off / BALLOC_MIN_BLOCK);
    if (offset_leaves >= NUM_LEAVES) return; /* outside the arena: not ours */

    /* Walk up from the leaf until a zeroed node is found -- that is the
     * exact node balloc_alloc() allocated, whatever its order. Everything
     * below it (this leaf included) was never touched by that allocation,
     * so it still holds its original nonzero init value; that is what lets
     * this loop recognise "not yet at the allocated node" without being
     * told the block's size. */
    /* The whole walk-up-and-merge is one critical section: the "find the
     * zeroed node" search and the merge that acts on it read and write the
     * same tree, and a concurrent alloc between them would be freeing
     * against a shape that no longer exists. */
    uintptr_t bflags = spin_lock_irqsave(&g_balloc_lock);

    uint32_t index = offset_leaves + NUM_LEAVES - 1u;
    uint32_t node_size;
    for (node_size = 1u; g_longest[index] != 0; index = node_parent(index)) {
        node_size *= 2u;
        /* Reached the root without ever finding a zero: this address was
         * never allocated (bad pointer, or a double free). Refuse rather
         * than wrap parent(0) into a huge index. */
        if (index == 0) { spin_unlock_irqrestore(&g_balloc_lock, bflags); return; }
    }
    g_longest[index] = (uint16_t)node_size;

    /* Merge upward wherever both children are now fully free. */
    while (index != 0) {
        index = node_parent(index);
        node_size *= 2u;
        uint32_t l = g_longest[node_left(index)];
        uint32_t r = g_longest[node_right(index)];
        g_longest[index] = (uint16_t)((l + r == node_size) ? node_size
                                                            : (l > r ? l : r));
    }
    spin_unlock_irqrestore(&g_balloc_lock, bflags);
}

void balloc_stats(uint32_t *arena_bytes, uint32_t *largest_free_bytes) {
    if (arena_bytes) *arena_bytes = ARENA_BYTES;
    if (largest_free_bytes) {
        /* Before the arena is reserved the answer is ARENA_BYTES, not 0: the
         * question this reports on is "the biggest single request
         * balloc_alloc() could satisfy right now", and an unreserved
         * allocator would reserve a whole empty arena to serve one. Zero
         * would describe a full allocator, which is the opposite state.
         *
         * It is a forecast rather than a fact in exactly one case -- the
         * reservation itself can still fail if the heap cannot place an
         * ARENA_BYTES-aligned run -- and balloc_alloc() says so on the spot
         * when that happens. */
        /* Deliberately not under g_balloc_lock, and the distinction from
         * palloc_stats() -- which S5 *did* lock -- is the point. That one
         * walks the whole bitmap, so an unlocked read can compose a figure
         * for a heap state that never existed. This reads one aligned
         * 16-bit word, which cannot tear, so the only thing missing exclusion
         * costs here is freshness: the answer was true when it was read.
         * Locking it would trade a real (if small) cost on a diagnostic for
         * no property anyone can name. */
        if (!g_ready) *largest_free_bytes = ARENA_BYTES;
        else *largest_free_bytes = (uint32_t)g_longest[0] * BALLOC_MIN_BLOCK;
    }
}
