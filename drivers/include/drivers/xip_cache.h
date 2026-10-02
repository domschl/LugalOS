#ifndef LUGALOS_DRIVERS_XIP_CACHE_H
#define LUGALOS_DRIVERS_XIP_CACHE_H

/* The RP2350's XIP cache, maintained by address -- 38.4, plan/phase38_psram.md
 * H1. The one 16 KB cache sits in front of both QMI windows (flash and
 * PSRAM), and the same PSRAM byte is reachable through it (0x11000000) and
 * around it (0x15000000):
 *
 *   wrote through the cache, about to read or DMA around it:
 *       xip_cache_clean_range()       -- write the dirty lines back
 *   wrote around the cache, about to read through it:
 *       xip_cache_invalidate_range()  -- drop the stale lines (dirty ones
 *                                        are discarded, not written)
 *
 * Both take the *cached* address and a length, and cover every 8-byte line
 * the range touches. Per line, so ~1 ms a MB: fine for a page allocation,
 * not for a byte. Not by set/way: that would reach every line in the cache,
 * other owners' dirty data included.
 *
 * On every target without an XIP cache (QEMU, the ESP32-P4, whose caches are
 * a different mechanism) these are no-ops. */

#include "lugalos_config.h"

#include <stddef.h>

#if defined(CONFIG_BOARD_RP2350)
void xip_cache_clean_range(const void *cached_addr, size_t bytes);
void xip_cache_invalidate_range(const void *cached_addr, size_t bytes);
#else
static inline void xip_cache_clean_range(const void *cached_addr, size_t bytes) {
    (void)cached_addr; (void)bytes;
}
static inline void xip_cache_invalidate_range(const void *cached_addr, size_t bytes) {
    (void)cached_addr; (void)bytes;
}
#endif

#endif /* LUGALOS_DRIVERS_XIP_CACHE_H */
