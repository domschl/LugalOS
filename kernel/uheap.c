/* See kernel/include/kernel/uheap.h. 45.3b, plan/phase45_esp32c6.md.
 *
 * Block layout (all sizes in bytes, multiples of 8, offsets from h->base):
 *
 *   +0  uint32_t size     total block size including this 8-byte header;
 *                         bit 0 = this block is in use
 *   +4  uint32_t prev     size of the block physically before this one
 *                         (0 for the first)
 *   +8  payload...        or, for a *free* block, two uint32_t list links:
 *   +8  uint32_t next     offset+1 of the next free block in the class (0 = end)
 *   +12 uint32_t prevfree offset+1 of the previous one (0 = head)
 *
 * The heap ends with a zero-sized, in-use sentinel block, so the last real
 * block never looks for a successor beyond the arena and "is the next block
 * free?" needs no bounds test. Nothing in this file calls anything: it has no
 * libc dependency, because it is compiled into U-mode text that may not. */

#include "kernel/uheap.h"
#include <stddef.h>

#define HDR       8u
#define MIN_BLOCK 16u
#define USED      1u

typedef struct { uint32_t size, prev; } blk_t;
typedef struct { uint32_t next, prevfree; } flink_t;

static inline blk_t   *at(const uheap_t *h, uint32_t off) { return (blk_t *)(h->base + off); }
static inline uint32_t off_of(const uheap_t *h, const blk_t *b) { return (uint32_t)((const uint8_t *)b - h->base); }
static inline uint32_t bsize(const blk_t *b) { return b->size & ~USED; }
static inline bool     bused(const blk_t *b) { return (b->size & USED) != 0; }
static inline flink_t *links(blk_t *b) { return (flink_t *)((uint8_t *)b + HDR); }
static inline blk_t   *next_blk(const uheap_t *h, const blk_t *b) { return at(h, off_of(h, b) + bsize(b)); }

/* floor(log2(n)), n > 0. The class of a block of that size. */
static uint32_t cls(uint32_t n) {
    uint32_t c = 0;
    while (n >>= 1) c++;
    return c;
}

static void list_insert(uheap_t *h, blk_t *b) {
    uint32_t c = cls(bsize(b));
    flink_t *l = links(b);
    l->next = h->free_head[c];
    l->prevfree = 0;
    if (h->free_head[c]) links(at(h, h->free_head[c] - 1))->prevfree = off_of(h, b) + 1;
    h->free_head[c] = off_of(h, b) + 1;
}

static void list_remove(uheap_t *h, blk_t *b) {
    uint32_t c = cls(bsize(b));
    flink_t *l = links(b);
    if (l->prevfree) links(at(h, l->prevfree - 1))->next = l->next;
    else             h->free_head[c] = l->next;
    if (l->next) links(at(h, l->next - 1))->prevfree = l->prevfree;
}

bool uheap_init(uheap_t *h, void *arena, uint32_t size) {
    if (!h || !arena || ((uintptr_t)arena & 7u) || size < 64) return false;
    size &= ~7u;
    for (int i = 0; i < UHEAP_CLASSES; i++) h->free_head[i] = 0;
    h->base = (uint8_t *)arena;
    h->size = size;
    h->used_bytes = h->used_blocks = h->high_water = h->fail_count = 0;

    uint32_t first = size - HDR;                    /* everything but the sentinel */
    blk_t *b = at(h, 0);
    b->size = first;                                /* free */
    b->prev = 0;
    blk_t *s = at(h, first);
    s->size = USED;                                 /* the sentinel: size 0, in use */
    s->prev = first;
    list_insert(h, b);
    return true;
}

/* A block big enough for `n` payload bytes. */
static uint32_t need(uint32_t n) {
    uint32_t t = (n + HDR + 7u) & ~7u;
    return t < MIN_BLOCK ? MIN_BLOCK : t;
}

static blk_t *find_fit(uheap_t *h, uint32_t want) {
    /* The class `want` falls in may hold smaller blocks: scan it. Every class
     * above holds only blocks at least as large as its lower bound, which is
     * at least `want`, so its head fits. */
    uint32_t c = cls(want);
    for (uint32_t o = h->free_head[c]; o; o = links(at(h, o - 1))->next) {
        blk_t *b = at(h, o - 1);
        if (bsize(b) >= want) return b;
    }
    for (c++; c < UHEAP_CLASSES; c++)
        if (h->free_head[c]) return at(h, h->free_head[c] - 1);
    return NULL;
}

void *uheap_alloc(uheap_t *h, uint32_t n) {
    if (n == 0 || n > h->size) { if (n) h->fail_count++; return NULL; }
    uint32_t want = need(n);
    blk_t *b = find_fit(h, want);
    if (!b) { h->fail_count++; return NULL; }
    list_remove(h, b);
    uint32_t have = bsize(b);
    if (have - want >= MIN_BLOCK) {                 /* split: the tail stays free */
        blk_t *t = at(h, off_of(h, b) + want);
        t->size = have - want;
        t->prev = want;
        next_blk(h, t)->prev = t->size;
        b->size = want;
        list_insert(h, t);
    }
    b->size |= USED;
    h->used_bytes += bsize(b) - HDR;
    h->used_blocks++;
    if (h->used_bytes > h->high_water) h->high_water = h->used_bytes;
    return (uint8_t *)b + HDR;
}

static bool owns(const uheap_t *h, const void *p) {
    const uint8_t *q = (const uint8_t *)p;
    return q >= h->base + HDR && q < h->base + h->size && (((uintptr_t)q - (uintptr_t)h->base) & 7u) == 0;
}

void uheap_free(uheap_t *h, void *p) {
    if (!p || !owns(h, p)) return;
    blk_t *b = (blk_t *)((uint8_t *)p - HDR);
    if (!bused(b)) return;                          /* a double free is ignored, not acted on */
    h->used_bytes -= bsize(b) - HDR;
    h->used_blocks--;
    b->size &= ~USED;

    blk_t *n = next_blk(h, b);
    if (!bused(n)) {                                /* merge with the successor */
        list_remove(h, n);
        b->size += bsize(n);
        next_blk(h, b)->prev = b->size;
    }
    if (b->prev) {                                  /* merge with the predecessor */
        blk_t *pv = at(h, off_of(h, b) - b->prev);
        if (!bused(pv)) {
            list_remove(h, pv);
            pv->size += bsize(b);
            next_blk(h, pv)->prev = pv->size;
            b = pv;
        }
    }
    list_insert(h, b);
}

void *uheap_calloc(uheap_t *h, uint32_t n, uint32_t size) {
    if (n != 0 && size > 0xffffffffu / n) { h->fail_count++; return NULL; }
    uint32_t total = n * size;
    uint8_t *p = (uint8_t *)uheap_alloc(h, total);
    if (p) for (uint32_t i = 0; i < total; i++) p[i] = 0;
    return p;
}

void *uheap_realloc(uheap_t *h, void *p, uint32_t n) {
    if (!p) return uheap_alloc(h, n);
    if (n == 0) { uheap_free(h, p); return NULL; }
    if (!owns(h, p)) return NULL;
    blk_t *b = (blk_t *)((uint8_t *)p - HDR);
    if (!bused(b)) return NULL;
    uint32_t cur = bsize(b) - HDR;
    uint32_t want = need(n);
    if (want <= bsize(b)) return p;                 /* already big enough: keep it (no shrink-split) */

    /* Grow in place into a free successor when that is enough. */
    blk_t *nx = next_blk(h, b);
    if (!bused(nx) && bsize(b) + bsize(nx) >= want) {
        list_remove(h, nx);
        uint32_t total = bsize(b) + bsize(nx);
        h->used_bytes += total - bsize(b);
        if (total - want >= MIN_BLOCK) {
            blk_t *t = at(h, off_of(h, b) + want);
            t->size = total - want;
            t->prev = want;
            next_blk(h, t)->prev = t->size;
            list_insert(h, t);
            h->used_bytes -= t->size;
            total = want;
        }
        b->size = total | USED;
        next_blk(h, b)->prev = total;
        if (h->used_bytes > h->high_water) h->high_water = h->used_bytes;
        return p;
    }

    uint8_t *q = (uint8_t *)uheap_alloc(h, n);
    if (!q) return NULL;                            /* the original is untouched, as realloc promises */
    for (uint32_t i = 0; i < cur; i++) q[i] = ((uint8_t *)p)[i];
    uheap_free(h, p);
    return q;
}

uint32_t uheap_largest_free(const uheap_t *h) {
    uint32_t best = 0;
    for (int c = 0; c < UHEAP_CLASSES; c++)
        for (uint32_t o = h->free_head[c]; o; o = links(at(h, o - 1))->next) {
            uint32_t s = bsize(at(h, o - 1));
            if (s > best) best = s;
            if (o >= h->size) break;                /* a corrupt link must not loop us forever */
        }
    return best > HDR ? best - HDR : 0;
}

uint32_t uheap_free_bytes(const uheap_t *h) {
    uint32_t total = 0;
    uint32_t off = 0;
    while (off + HDR <= h->size) {
        const blk_t *b = at(h, off);
        uint32_t s = bsize(b);
        if (s == 0) break;
        if (!bused(b)) total += s - HDR;
        off += s;
    }
    return total;
}

uint32_t uheap_check(const uheap_t *h) {
    uint32_t errors = 0;
    uint32_t off = 0, prev = 0, free_in_walk = 0;
    uint32_t used_blocks = 0, used_bytes = 0;
    while (true) {
        if (off + HDR > h->size) { errors++; break; }
        const blk_t *b = at(h, off);
        uint32_t s = bsize(b);
        if (b->prev != prev) errors++;
        if (s == 0) { if (off != h->size - HDR || !bused(b)) errors++; break; }
        if ((s & 7u) || s < MIN_BLOCK || off + s > h->size - HDR) { errors++; break; }
        if (bused(b)) { used_blocks++; used_bytes += s - HDR; }
        else {
            free_in_walk++;
            const blk_t *nx = at(h, off + s);
            if (!bused(nx) && bsize(nx)) errors++;          /* two adjacent free blocks: not coalesced */
        }
        prev = s;
        off += s;
    }
    /* Every free block in the walk must be on exactly one list, in the right class. */
    uint32_t in_lists = 0;
    for (int c = 0; c < UHEAP_CLASSES; c++) {
        uint32_t last = 0, guard = 0;
        for (uint32_t o = h->free_head[c]; o; ) {
            if (o > h->size || ++guard > h->size / MIN_BLOCK + 1) { errors++; break; }
            const blk_t *b = at(h, o - 1);
            if (bused(b) || cls(bsize(b)) != (uint32_t)c) errors++;
            if (links(( blk_t *)b)->prevfree != last) errors++;
            last = o; in_lists++;
            o = links((blk_t *)b)->next;
        }
    }
    if (in_lists != free_in_walk) errors++;
    if (used_blocks != h->used_blocks || used_bytes != h->used_bytes) errors++;
    return errors;
}
