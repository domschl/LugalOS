#ifndef ARCH_JH7110_CACHE_H
#define ARCH_JH7110_CACHE_H

#include <stdint.h>

#define SIFIVE_L2_BASE              0x02010000UL
#define SIFIVE_L2_FLUSH64           0x200UL

static inline void jh7110_cache_mb(void) {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

/* SiFive L2 Cache Controller Flush64 register: flushes dirty lines to DDR and
 * invalidates L1/L2 caches across all U74 harts for non-coherent DMA masters.
 * TileLink strictly orders successive writes to the same MMIO register; issuing
 * a single fence before and after the burst achieves full bus pipeline throughput. */
static inline void jh7110_flush_l2_range(uintptr_t pa_start, uintptr_t pa_end) {
    volatile uint64_t *flush64 = (volatile uint64_t *)(SIFIVE_L2_BASE + SIFIVE_L2_FLUSH64);
    jh7110_cache_mb();
    uintptr_t start = pa_start & ~(64UL - 1UL);
    uintptr_t line = start;
    while (line + 256UL <= pa_end) {
        *flush64 = line;
        *flush64 = line + 64UL;
        *flush64 = line + 128UL;
        *flush64 = line + 192UL;
        line += 256UL;
    }
    while (line < pa_end) {
        *flush64 = line;
        line += 64UL;
    }
    jh7110_cache_mb();
}

#endif /* ARCH_JH7110_CACHE_H */
