/*
 * The RP2350's XIP cache maintenance by address -- 38.4, plan/phase38_psram.md
 * H1. See drivers/include/drivers/xip_cache.h for when to use which.
 *
 * pico-sdk's hardware_xip_cache (xip_cache.c), reduced to what is used: a
 * byte write to 0x18000000 + (address - 0x10000000) + op performs `op` on the
 * line holding that address -- op 2 invalidate by address, op 3 clean by
 * address. Not RAM-resident: neither op disturbs instruction fetch, and
 * neither is called from inside the flash write's critical section (that
 * one cleans by set/way, drivers/flash_rp2350.c).
 */

#include "drivers/xip_cache.h"

#include <stdint.h>

#if defined(CONFIG_BOARD_RP2350)

#define XIP_BASE            0x10000000UL
#define XIP_MAINT_BASE      0x18000000UL
#define XIP_CACHE_LINE      8u
#define OP_INVALIDATE_BY_ADDRESS 2u
#define OP_CLEAN_BY_ADDRESS      3u

static void maintain(const void *cached_addr, size_t bytes, uintptr_t op) {
    if (bytes == 0) return;
    uintptr_t a = (uintptr_t)cached_addr & ~(uintptr_t)(XIP_CACHE_LINE - 1);
    uintptr_t end = (uintptr_t)cached_addr + bytes;
    if (a < XIP_BASE || end > XIP_BASE + 0x04000000UL) return;   /* not XIP space */
    __asm__ __volatile__("fence" ::: "memory");     /* earlier stores reach the cache first */
    for (; a < end; a += XIP_CACHE_LINE) {
        *(volatile uint8_t *)(XIP_MAINT_BASE + (a - XIP_BASE) + op) = 0;
    }
    __asm__ __volatile__("fence" ::: "memory");
}

void xip_cache_clean_range(const void *cached_addr, size_t bytes) {
    maintain(cached_addr, bytes, OP_CLEAN_BY_ADDRESS);
}

void xip_cache_invalidate_range(const void *cached_addr, size_t bytes) {
    maintain(cached_addr, bytes, OP_INVALIDATE_BY_ADDRESS);
}

#endif
