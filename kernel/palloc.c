#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/lock.h"
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* See kernel/include/kernel/palloc.h for the rationale. */

static uintptr_t g_base;        /* page-aligned first managed address */
static uint32_t  g_num_pages;
static uint8_t   g_bitmap[(PALLOC_MAX_PAGES + 7) / 8];

/* Live page count and its high-water mark.
 *
 * g_used_pages duplicates what the bitmap already says, and only exists so
 * that g_peak_used can be maintained in O(1) at every allocation -- deriving
 * the peak would otherwise mean scanning the bitmap on each call, which is
 * the one place in this allocator where cost is paid repeatedly.
 *
 * The peak is the number worth having. Instantaneous free pages tell you
 * whether the last allocation fitted; the peak tells you whether the heap is
 * the right size, which on RP2350 (60 KB above _kernel_end) is a live
 * question rather than an academic one. Monotonic by construction: never
 * reset, so a transient spike between two reads of /proc/meminfo cannot hide
 * between them. */
static uint32_t  g_used_pages;
static uint32_t  g_peak_used;

/* Guards the bitmap and the two counters above (S4,
 * plan/phase22_smp_locking_foundation.md). One lock for the whole
 * allocator, not one per region: there is a single bitmap and a single
 * pair of counters, so there is nothing to divide. Zero-initialised, which
 * is a free spinlock_t -- and on RP2350 that also keeps it in .bss where
 * tools/sizereport.py can see it (see kernel/lock.h). */
static spinlock_t g_palloc_lock;

static inline bool bit_get(uint32_t i) {
    return (g_bitmap[i / 8] >> (i % 8)) & 1u;
}
static inline void bit_set(uint32_t i) {
    g_bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
}
static inline void bit_clear(uint32_t i) {
    g_bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
}

void palloc_init(uintptr_t start, uintptr_t end) {
    /* Round inward: never hand out a partial page at either edge. */
    uintptr_t s = (start + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1);
    uintptr_t e = end & ~((uintptr_t)PAGE_SIZE - 1);

    memset(g_bitmap, 0, sizeof(g_bitmap));
    g_used_pages = 0;
    g_peak_used = 0;

    if (e <= s) {
        g_base = s;
        g_num_pages = 0;
        printk("[PAlloc] No usable heap (start=0x%lx end=0x%lx)\n",
               (unsigned long)start, (unsigned long)end);
        return;
    }

    uintptr_t pages = (e - s) / PAGE_SIZE;
    if (pages > PALLOC_MAX_PAGES) {
        printk("[PAlloc] Heap has %lu pages; managing the first %d (PALLOC_MAX_PAGES)\n",
               (unsigned long)pages, PALLOC_MAX_PAGES);
        pages = PALLOC_MAX_PAGES;
    }

    g_base = s;
    g_num_pages = (uint32_t)pages;
    printk("[PAlloc] Page allocator: %u pages of %d bytes at 0x%lx (%u KB)\n",
           g_num_pages, PAGE_SIZE, (unsigned long)g_base,
           (unsigned int)((g_num_pages * (uint32_t)PAGE_SIZE) / 1024));
}

void *palloc_pages(uint32_t n) {
    return palloc_pages_aligned(n, 1);
}

void *palloc_pages_aligned(uint32_t n, uint32_t align_pages) {
    if (n == 0 || n > g_num_pages) return NULL;
    if (align_pages == 0 || (align_pages & (align_pages - 1)) != 0) return NULL;

    /* The alignment is relative to g_base, which palloc_init() rounded up to a
     * page boundary but not further. If the heap does not start on the
     * requested boundary, an index that is a multiple of align_pages is not an
     * address that is -- so the offset is folded in rather than assumed away. */
    uint32_t align_bytes = align_pages * (uint32_t)PAGE_SIZE;
    uint32_t skew = (uint32_t)(g_base & (align_bytes - 1)) / (uint32_t)PAGE_SIZE;
    uint32_t phase = skew ? (align_pages - skew) : 0;

    /* The scan-then-claim below is only correct if nothing else can claim a
     * page between finding a free run and marking it. Cooperative scheduling
     * made that true for free; preemption does not, and neither does a
     * second hart -- irq_save() masked only the hart that called it, so two
     * harts could find the same free run and both claim it (S3's endpoint
     * race, but over the page bitmap and with no refusal path to catch it).
     *
     * A spinlock_t rather than a ylock_t, and here that is the easy call:
     * this region is a bounded scan over a bitmap with no call out of it at
     * all -- no printk, no yield, nothing that can block. The one expensive
     * thing an allocation does, zeroing the pages, is deliberately outside
     * (see below). S3 had to split its claim in two precisely because it
     * could not say that; this one can. */
    uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);

    /* First fit. The page count here is small (128 on RP2350, 4096 on QEMU)
     * and allocation is rare -- task creation and page tables -- so a linear
     * scan is not worth improving on. */
    for (uint32_t i = phase; i + n <= g_num_pages; i++) {
        if (((i - phase) & (align_pages - 1)) != 0) continue;
        bool run_ok = true;
        for (uint32_t j = 0; j < n; j++) {
            if (bit_get(i + j)) { i += j; run_ok = false; break; }
        }
        if (!run_ok) continue;

        for (uint32_t j = 0; j < n; j++) bit_set(i + j);
        g_used_pages += n;
        if (g_used_pages > g_peak_used) g_peak_used = g_used_pages;
        spin_unlock_irqrestore(&g_palloc_lock, irqf);
        void *p = (void *)(g_base + (uintptr_t)i * PAGE_SIZE);
        memset(p, 0, (size_t)n * PAGE_SIZE); /* outside the critical section */
        return p;
    }
    spin_unlock_irqrestore(&g_palloc_lock, irqf);
    return NULL;
}

void palloc_free(void *p, uint32_t n) {
    if (!p || n == 0) return;
    uintptr_t addr = (uintptr_t)p;
    if (addr < g_base) return;

    uintptr_t off = addr - g_base;
    if (off % PAGE_SIZE) return;        /* not a page boundary: not ours */

    uint32_t idx = (uint32_t)(off / PAGE_SIZE);
    if (idx >= g_num_pages || idx + n > g_num_pages) return;

    uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);
    /* Only count down pages that were actually allocated. Clearing an already
     * clear bit is harmless to the bitmap, so a double free was previously a
     * no-op; an unguarded g_used_pages-- would turn it into a counter that
     * underflows and reports a peak larger than the heap. */
    for (uint32_t j = 0; j < n; j++) {
        if (bit_get(idx + j)) {
            bit_clear(idx + j);
            g_used_pages--;
        }
    }
    spin_unlock_irqrestore(&g_palloc_lock, irqf);
}

void palloc_stats(uint32_t *total_pages, uint32_t *free_pages) {
    if (total_pages) *total_pages = g_num_pages;
    if (free_pages) {
        uint32_t free_count = 0;
        for (uint32_t i = 0; i < g_num_pages; i++) {
            if (!bit_get(i)) free_count++;
        }
        *free_pages = free_count;
    }
}

void palloc_extra_stats(uint32_t *peak_used_pages, uint32_t *largest_free_run) {
    if (peak_used_pages) *peak_used_pages = g_peak_used;

    if (largest_free_run) {
        /* Free pages and *usable* free pages are not the same number once
         * anything asks for more than one page at a time. palloc_pages() is
         * first-fit over contiguous runs, and palloc_pages_aligned() wants a
         * self-aligned run on top of that (PMP needs NAPOT), so a heap with
         * plenty free but nothing contiguous fails allocations that the free
         * count says should succeed. This is the figure that distinguishes
         * "out of memory" from "too fragmented", which are different bugs.
         *
         * Reported without regard to alignment: it is the ceiling on what any
         * request could get, not a promise that an aligned one will. */
        uint32_t best = 0, run = 0;
        for (uint32_t i = 0; i < g_num_pages; i++) {
            if (bit_get(i)) {
                run = 0;
            } else if (++run > best) {
                best = run;
            }
        }
        *largest_free_run = best;
    }
}
