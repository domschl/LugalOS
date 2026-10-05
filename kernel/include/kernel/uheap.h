#ifndef LUGALOS_KERNEL_UHEAP_H
#define LUGALOS_KERNEL_UHEAP_H

#include <stdint.h>
#include <stdbool.h>

/* A heap over one caller-supplied arena (45.3b, plan/phase45_esp32c6.md).
 *
 * The Wi-Fi blob allocates constantly -- 59 call sites of its `_free`, 21
 * of `_wifi_malloc`, buffers from 100 bytes to ~1.7 KB -- through the
 * `malloc`/`free`/`calloc`/`realloc` entries of its OS table. Those cannot
 * be syscalls (the cost) and must not be kernel memory (the isolation), so
 * they run in the radio's own domain, over a slice of its own RAM, in this
 * allocator. Nothing here is specific to the radio: any confined runtime can
 * use it; it has no dependencies at all, not even libc, because it is linked
 * into U-mode text that may call nothing outside its own pages.
 *
 * ## What it is
 *
 * Boundary tags with segregated free lists and immediate coalescing: every
 * block carries its size and its predecessor's size, so freeing merges with
 * both neighbours in O(1), and free blocks sit in one of 32 power-of-two
 * classes so an allocation finds a fit without walking the whole heap.
 * Allocations are 8-byte aligned; the smallest block is 16 bytes.
 * Everything is stored as 32-bit *offsets* from the arena, so a block is the
 * same size on rv32, rv64 and the host the tests run on.
 *
 * ## What it is not
 *
 * Safe against its own client. The metadata lives in the arena, next to the
 * data, and a runtime that overruns a buffer overwrites the next block's
 * header. uheap_check() walks the heap and reports every inconsistency it can
 * see (a size that runs off the end, a back-pointer that disagrees, a free
 * list that does not match the blocks), so a radio task that trashed its heap
 * is *reported* as that, not diagnosed from a crash two minutes later. The
 * damage stays inside the radio's domain, which is the point of putting the
 * heap there.
 *
 * Not thread-safe: the owner serialises. In the radio domain that is the
 * domain's critical section (kobj.h, KOBJ_OP_CRIT_ENTER) around each call. */

#define UHEAP_CLASSES 32

typedef struct {
    uint8_t  *base;
    uint32_t  size;                       /* arena bytes, multiple of 8 */
    uint32_t  free_head[UHEAP_CLASSES];   /* offset+1 of the first free block in each class; 0 = none */
    /* Accounting: what a caller reads to tune pool sizes, and what a test
     * compares against its own model. */
    uint32_t  used_bytes;                 /* payload bytes handed out and not returned */
    uint32_t  used_blocks;
    uint32_t  high_water;                 /* the most used_bytes has ever been */
    uint32_t  fail_count;                 /* allocations that returned NULL */
} uheap_t;

/* `arena` must be 8-byte aligned and at least 64 bytes. Returns false if not;
 * the arena's size is rounded down to a multiple of 8. */
bool  uheap_init(uheap_t *h, void *arena, uint32_t size);

void *uheap_alloc(uheap_t *h, uint32_t n);          /* NULL if it does not fit; n == 0 returns NULL */
void *uheap_calloc(uheap_t *h, uint32_t n, uint32_t size);   /* overflow-checked, zeroed */
void *uheap_realloc(uheap_t *h, void *p, uint32_t n);        /* p == NULL allocates; n == 0 frees and returns NULL */
void  uheap_free(uheap_t *h, void *p);              /* NULL is fine; a pointer not from this heap is ignored */

/* Largest single allocation that would succeed now, and total free bytes. */
uint32_t uheap_largest_free(const uheap_t *h);
uint32_t uheap_free_bytes(const uheap_t *h);

/* Walks every block and every free list. Returns the number of inconsistencies
 * found (0 = sound). Does not modify the heap. */
uint32_t uheap_check(const uheap_t *h);

#endif /* LUGALOS_KERNEL_UHEAP_H */
