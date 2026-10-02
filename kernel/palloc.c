#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/lock.h"
#include "drivers/xip_cache.h"
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* See kernel/include/kernel/palloc.h for the rationale. */

/* A zone is one contiguous range of pages and its bitmap. There are two
 * (38.4, plan/phase38_psram.md §3.1): the fast zone, which is SRAM and
 * everything this allocator was before, and the bulk zone -- PSRAM on a
 * board with one, a stand-in range on QEMU, absent elsewhere. Each keeps its
 * own counters; one lock covers both, since nothing here is held long. */
typedef struct {
    uintptr_t base;             /* page-aligned first managed address */
    uint32_t  num_pages;
    uint8_t  *bitmap;
    uint32_t  max_pages;        /* the bitmap's capacity */
    /* Live page count and its high-water mark.
     *
     * used_pages duplicates what the bitmap already says, and only exists so
     * that peak_used can be maintained in O(1) at every allocation -- deriving
     * the peak would otherwise mean scanning the bitmap on each call, which is
     * the one place in this allocator where cost is paid repeatedly.
     *
     * The peak is the number worth having. Instantaneous free pages tell you
     * whether the last allocation fitted; the peak tells you whether the heap
     * is the right size, which on RP2350 is a live question rather than an
     * academic one. Monotonic by construction: never reset, so a transient
     * spike between two reads of /proc/meminfo cannot hide between them. */
    uint32_t  used_pages;
    uint32_t  peak_used;
    /* Bulk zone on RP2350: the same memory without the XIP cache, for
     * zeroing (H1). 0 where there is no such alias. */
    uintptr_t uncached_delta;
} zone_t;

static uint8_t g_fast_bitmap[(PALLOC_MAX_PAGES + 7) / 8];
static zone_t  g_fast = { .bitmap = g_fast_bitmap, .max_pages = PALLOC_MAX_PAGES };

#if defined(CONFIG_PALLOC_BULK_PAGES)
static uint8_t g_bulk_bitmap[(CONFIG_PALLOC_BULK_PAGES + 7) / 8];
static zone_t  g_bulk = { .bitmap = g_bulk_bitmap, .max_pages = CONFIG_PALLOC_BULK_PAGES };
#define BULK_ZONE (&g_bulk)
#else
#define BULK_ZONE ((zone_t *)NULL)
#endif

/* Bulk requests served from SRAM because the bulk zone could not: a number
 * that should stay 0, and /proc/meminfo says so when it does not. */
static uint32_t g_bulk_fallbacks;

/* Guards both bitmaps and their counters (S4,
 * plan/phase22_smp_locking_foundation.md). Zero-initialised, which is a free
 * spinlock_t -- and on RP2350 that also keeps it in .bss where
 * tools/sizereport.py can see it (see kernel/lock.h). */
static spinlock_t g_palloc_lock;

static inline bool bit_get(const zone_t *z, uint32_t i) {
    return (z->bitmap[i / 8] >> (i % 8)) & 1u;
}
static inline void bit_set(zone_t *z, uint32_t i) {
    z->bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
}
static inline void bit_clear(zone_t *z, uint32_t i) {
    z->bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
}

/* Round inward, never hand out a partial page at either edge, and clamp to
 * the bitmap. Returns the page count. */
static uint32_t zone_setup(zone_t *z, uintptr_t start, uintptr_t end, const char *what) {
    uintptr_t s = (start + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1);
    uintptr_t e = end & ~((uintptr_t)PAGE_SIZE - 1);

    memset(z->bitmap, 0, (z->max_pages + 7) / 8);
    z->used_pages = 0;
    z->peak_used = 0;
    z->base = s;
    z->num_pages = 0;
    if (e <= s) {
        printk("[PAlloc] No usable %s (start=0x%lx end=0x%lx)\n", what,
               (unsigned long)start, (unsigned long)end);
        return 0;
    }
    uintptr_t pages = (e - s) / PAGE_SIZE;
    if (pages > z->max_pages) {
        printk("[PAlloc] %s has %lu pages; managing the first %lu\n", what,
               (unsigned long)pages, (unsigned long)z->max_pages);
        pages = z->max_pages;
    }
    z->num_pages = (uint32_t)pages;
    return z->num_pages;
}

void palloc_init(uintptr_t start, uintptr_t end) {
    zone_setup(&g_fast, start, end, "heap");
    printk("[PAlloc] Page allocator: %u pages of %d bytes at 0x%lx (%u KB)\n",
           g_fast.num_pages, PAGE_SIZE, (unsigned long)g_fast.base,
           (unsigned int)((g_fast.num_pages * (uint32_t)PAGE_SIZE) / 1024));
#if defined(CONFIG_PALLOC_BULK_PAGES) && !defined(CONFIG_PSRAM_BYTES)
    /* QEMU: no PSRAM, but RAM well beyond what the fast zone manages. A
     * stand-in bulk zone right above it lets the suite run the zone code,
     * the by-address free and the fallback -- everything but the memory
     * type. Only where the RAM is there: a board whose heap the fast zone
     * already covers gets none. */
    uintptr_t fast_end = g_fast.base + (uintptr_t)g_fast.num_pages * PAGE_SIZE;
    uintptr_t want = fast_end + (uintptr_t)CONFIG_PALLOC_BULK_PAGES * PAGE_SIZE;
    if (want <= (end & ~((uintptr_t)PAGE_SIZE - 1))) palloc_init_bulk(fast_end, want, 0);
#endif
}

void palloc_init_bulk(uintptr_t start, uintptr_t end, uintptr_t uncached_delta) {
    zone_t *z = BULK_ZONE;
    if (!z) return;
    zone_setup(z, start, end, "bulk zone");
    z->uncached_delta = uncached_delta;
    printk("[PAlloc] Bulk zone: %u pages at 0x%lx (%u KB)\n", z->num_pages,
           (unsigned long)z->base, (unsigned int)((z->num_pages * (uint32_t)PAGE_SIZE) / 1024));
}

static zone_t *zone_of(uintptr_t addr) {
    if (addr >= g_fast.base && addr < g_fast.base + (uintptr_t)g_fast.num_pages * PAGE_SIZE) return &g_fast;
    zone_t *b = BULK_ZONE;
    if (b && b->num_pages && addr >= b->base && addr < b->base + (uintptr_t)b->num_pages * PAGE_SIZE) return b;
    return NULL;
}

/* Zeroes a freshly claimed run. In a zone with an uncached alias (PSRAM):
 * first drop whatever the XIP cache holds for the range -- possibly dirty
 * lines of whoever had it before, which must never be written back over the
 * zeros -- then write the zeros around the cache at the uncached rate (31
 * against 9 MB/s, [P§2]). Nothing can pull the lines back in between: the
 * run belongs to nobody until this returns. */
static void zone_zero(const zone_t *z, void *p, size_t bytes) {
    if (z->uncached_delta) {
        xip_cache_invalidate_range(p, bytes);
        memset((void *)((uintptr_t)p + z->uncached_delta), 0, bytes);
    } else {
        memset(p, 0, bytes);
    }
}

static void *zone_alloc(zone_t *z, uint32_t n, uint32_t align_pages, void *caller) {
    if (n == 0 || n > z->num_pages) return NULL;
    if (align_pages == 0 || (align_pages & (align_pages - 1)) != 0) return NULL;

    /* The alignment is relative to the zone's base, which zone_setup()
     * rounded up to a page boundary but not further. If the zone does not
     * start on the requested boundary, an index that is a multiple of
     * align_pages is not an address that is -- so the offset is folded in
     * rather than assumed away. NAPOT works the same in both zones: PMP
     * checks addresses, not memory types. */
    uint32_t align_bytes = align_pages * (uint32_t)PAGE_SIZE;
    uint32_t skew = (uint32_t)(z->base & (align_bytes - 1)) / (uint32_t)PAGE_SIZE;
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
     * all -- no yield, nothing that can block. The one expensive thing an
     * allocation does, zeroing the pages, is deliberately outside (see
     * below). S3 had to split its claim in two precisely because it could
     * not say that; this one can. */
    uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);

    /* First fit. The page counts here are small (128 + 2048 on the LCD-7,
     * 4096 on QEMU) and allocation is rare, so a linear scan is not worth
     * improving on. */
    for (uint32_t i = phase; i + n <= z->num_pages; i++) {
        if (((i - phase) & (align_pages - 1)) != 0) continue;
        bool run_ok = true;
        for (uint32_t j = 0; j < n; j++) {
            if (bit_get(z, i + j)) { i += j; run_ok = false; break; }
        }
        if (!run_ok) continue;

        for (uint32_t j = 0; j < n; j++) bit_set(z, i + j);
        z->used_pages += n;
        if (z->used_pages > z->peak_used) z->peak_used = z->used_pages;
        spin_unlock_irqrestore(&g_palloc_lock, irqf);
        void *p = (void *)(z->base + (uintptr_t)i * PAGE_SIZE);
        /* Before the zeroing, not after: the whole value of the check is that
         * it fires while the victim's data is still there to be looked at. */
        palloc_report_alloc(p, n, caller);
        zone_zero(z, p, (size_t)n * PAGE_SIZE);   /* outside the critical section */
        return p;
    }
    spin_unlock_irqrestore(&g_palloc_lock, irqf);
    return NULL;
}

void *palloc_pages(uint32_t n) {
    return zone_alloc(&g_fast, n, 1, __builtin_return_address(0));
}

void *palloc_pages_aligned(uint32_t n, uint32_t align_pages) {
    return zone_alloc(&g_fast, n, align_pages, __builtin_return_address(0));
}

void *palloc_pages_bulk_aligned(uint32_t n, uint32_t align_pages) {
    void *caller = __builtin_return_address(0);
    zone_t *b = BULK_ZONE;
    if (!b || b->num_pages == 0) return zone_alloc(&g_fast, n, align_pages, caller);
    void *p = zone_alloc(b, n, align_pages, caller);
    if (p) return p;
    p = zone_alloc(&g_fast, n, align_pages, caller);
    if (p) {
        uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);
        g_bulk_fallbacks++;
        spin_unlock_irqrestore(&g_palloc_lock, irqf);
    }
    return p;
}

void *palloc_pages_bulk(uint32_t n) {
    return palloc_pages_bulk_aligned(n, 1);
}

void palloc_free(void *p, uint32_t n) {
    if (!p || n == 0) return;
    uintptr_t addr = (uintptr_t)p;
    zone_t *z = zone_of(addr);
    if (!z) return;                     /* in neither zone: not ours */

    uintptr_t off = addr - z->base;
    if (off % PAGE_SIZE) return;        /* not a page boundary: not ours */

    uint32_t idx = (uint32_t)(off / PAGE_SIZE);
    if (idx + n > z->num_pages) return;

    uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);
    /* Only count down pages that were actually allocated. Clearing an already
     * clear bit is harmless to the bitmap, so a double free was previously a
     * no-op; an unguarded used_pages-- would turn it into a counter that
     * underflows and reports a peak larger than the heap. */
    for (uint32_t j = 0; j < n; j++) {
        if (bit_get(z, idx + j)) {
            bit_clear(z, idx + j);
            z->used_pages--;
        }
    }
    spin_unlock_irqrestore(&g_palloc_lock, irqf);
}

bool palloc_is_bulk(const void *p) {
    zone_t *b = BULK_ZONE;
    return b && zone_of((uintptr_t)p) == b;
}

/* Under the lock, like every other read of the bitmap (S5).
 *
 * These are diagnostics, and the tempting argument is that a diagnostic can
 * tolerate a figure that was true a moment ago. That argument is right about
 * *staleness* and wrong about this: an unlocked scan is not a stale answer,
 * it is a scan racing concurrent bit_set()/bit_clear() calls, which can count
 * a run that never existed as a whole. `/proc/meminfo`'s free-page count and
 * the largest-free-run figure are exactly what someone reads when they are
 * already suspicious about the heap, and a number that is wrong in a way the
 * allocator itself never was is worse than no number.
 *
 * Cheap to hold: the scan is bounded by the zone's page count (128 + 2048 on
 * the LCD-7, 4096 + 512 on QEMU) and calls nothing, which is the same reason palloc_pages() can use a
 * spinlock at all. */
static void zone_counts(const zone_t *z, uint32_t *free_pages, uint32_t *largest_run) {
    uint32_t free_count = 0, best = 0, run = 0;
    for (uint32_t i = 0; i < z->num_pages; i++) {
        if (bit_get(z, i)) {
            run = 0;
        } else {
            free_count++;
            if (++run > best) best = run;
        }
    }
    if (free_pages) *free_pages = free_count;
    if (largest_run) *largest_run = best;
}

void palloc_stats(uint32_t *total_pages, uint32_t *free_pages) {
    if (total_pages) *total_pages = g_fast.num_pages;
    if (free_pages) {
        uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);
        zone_counts(&g_fast, free_pages, NULL);
        spin_unlock_irqrestore(&g_palloc_lock, irqf);
    }
}

void palloc_extra_stats(uint32_t *peak_used_pages, uint32_t *largest_free_run) {
    if (peak_used_pages) *peak_used_pages = g_fast.peak_used;

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
        uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);
        zone_counts(&g_fast, NULL, largest_free_run);
        spin_unlock_irqrestore(&g_palloc_lock, irqf);
    }
}

bool palloc_bulk_stats(palloc_zone_stats_t *out) {
    zone_t *b = BULK_ZONE;
    if (!b || b->num_pages == 0 || !out) return false;
    uintptr_t irqf = spin_lock_irqsave(&g_palloc_lock);
    out->base = b->base;
    out->total_pages = b->num_pages;
    zone_counts(b, &out->free_pages, &out->largest_free_run);
    out->peak_used_pages = b->peak_used;
    out->fallbacks = g_bulk_fallbacks;
    spin_unlock_irqrestore(&g_palloc_lock, irqf);
    return true;
}
