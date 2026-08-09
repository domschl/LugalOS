#include "kernel/palloc.h"
#include "kernel/printk.h"
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

/* See kernel/include/kernel/palloc.h for the rationale. */

static uintptr_t g_base;        /* page-aligned first managed address */
static uint32_t  g_num_pages;
static uint8_t   g_bitmap[(PALLOC_MAX_PAGES + 7) / 8];

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
    if (n == 0 || n > g_num_pages) return NULL;

    /* First fit. The page count here is small (128 on RP2350, 4096 on QEMU)
     * and allocation is rare -- task creation and page tables -- so a linear
     * scan is not worth improving on. */
    for (uint32_t i = 0; i + n <= g_num_pages; i++) {
        bool run_ok = true;
        for (uint32_t j = 0; j < n; j++) {
            if (bit_get(i + j)) { i += j; run_ok = false; break; }
        }
        if (!run_ok) continue;

        for (uint32_t j = 0; j < n; j++) bit_set(i + j);
        void *p = (void *)(g_base + (uintptr_t)i * PAGE_SIZE);
        memset(p, 0, (size_t)n * PAGE_SIZE);
        return p;
    }
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

    for (uint32_t j = 0; j < n; j++) bit_clear(idx + j);
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
