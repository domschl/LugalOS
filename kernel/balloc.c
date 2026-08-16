#include "kernel/balloc.h"
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
    /* Self-aligned: BALLOC_ARENA_PAGES is a power of two, so this call asks
     * palloc for a run whose *address*, not just its offset, is a multiple
     * of ARENA_BYTES -- the property every block handed out below relies on
     * (see the header's "Arena, not the whole heap" section). */
    void *arena = palloc_pages_aligned(BALLOC_ARENA_PAGES, BALLOC_ARENA_PAGES);
    if (!arena) {
        printk("[BAlloc] Could not reserve %u-page arena; buddy allocator offline\n",
               BALLOC_ARENA_PAGES);
        g_ready = false;
        return;
    }
    g_arena_base = (uintptr_t)arena;

    uint32_t node_size = NUM_LEAVES * 2u;
    for (uint32_t i = 0; i < NODE_COUNT; i++) {
        if (is_pow2(i + 1u)) node_size /= 2u;
        g_longest[i] = (uint16_t)node_size;
    }
    g_ready = true;
    printk("[BAlloc] Buddy allocator: %u KB arena at %p (floor %u B)\n",
           ARENA_BYTES / 1024, arena, BALLOC_MIN_BLOCK);
}

void *balloc_alloc(uint32_t size) {
    if (!g_ready || size == 0 || size > ARENA_BYTES) return NULL;
    if (size < BALLOC_MIN_BLOCK) size = BALLOC_MIN_BLOCK;

    /* Safe from next_pow2()'s wraparound on a huge `size`: the ARENA_BYTES
     * check above already rejected anything that could overflow here. */
    uint32_t leaves_needed = next_pow2(size) / BALLOC_MIN_BLOCK;

    if (g_longest[0] < leaves_needed) return NULL; /* not enough space anywhere */

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

    void *ptr = (void *)(g_arena_base + (uintptr_t)offset_leaves * BALLOC_MIN_BLOCK);
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
    uint32_t index = offset_leaves + NUM_LEAVES - 1u;
    uint32_t node_size;
    for (node_size = 1u; g_longest[index] != 0; index = node_parent(index)) {
        node_size *= 2u;
        /* Reached the root without ever finding a zero: this address was
         * never allocated (bad pointer, or a double free). Refuse rather
         * than wrap parent(0) into a huge index. */
        if (index == 0) return;
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
}

void balloc_stats(uint32_t *arena_bytes, uint32_t *largest_free_bytes) {
    if (arena_bytes) *arena_bytes = ARENA_BYTES;
    if (largest_free_bytes) {
        *largest_free_bytes = g_ready ? (uint32_t)g_longest[0] * BALLOC_MIN_BLOCK : 0;
    }
}
