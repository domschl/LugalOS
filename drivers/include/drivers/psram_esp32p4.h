#ifndef LUGALOS_DRIVERS_PSRAM_ESP32P4_H
#define LUGALOS_DRIVERS_PSRAM_ESP32P4_H

/* The ESP32-P4's in-package PSRAM (AP hex-mode, 32 MB on the P4NRW32), 47.3,
 * plan/phase47_esp32p4_lcd7b_ribbon.md. The same interface as
 * drivers/psram_rp2350.h, so kernel/main.c, the shell, Lisp and /proc use one
 * set of calls; drivers/include/drivers/psram.h picks the board's. */

#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_PSRAM_BYTES)

/* SOC_EXTRAM_LOW: where the PSRAM MMU maps it, through L1D and L2. The
 * non-cacheable alias (SOC_NON_CACHEABLE_OFFSET_PSRAM) is not used: the bulk
 * zone zeroes through the cache, and DMA buffers are written back with
 * esp32p4_extmem_writeback() instead. */
#define PSRAM_CACHED_BASE    0x48000000u
#define PSRAM_UNCACHED_BASE  PSRAM_CACHED_BASE   /* no alias in use: delta 0 */

/* Power, clock, controller, device, MMU. Reports; psram_require() decides. */
void psram_init(void);

bool     psram_is_up(void);
uint32_t psram_bytes(void);        /* 0 when not up */

/* Halts with the reason if a board that names a PSRAM does not have it. */
void psram_require(void);

/* `psram`, `psram test [MB]`, `psram bench`. */
void psram_command(const char *args);

/* Appended to /proc/meminfo. */
int psram_meminfo(char *buf, uint32_t cap);

#endif

#endif /* LUGALOS_DRIVERS_PSRAM_ESP32P4_H */
