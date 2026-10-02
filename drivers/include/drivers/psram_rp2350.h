#ifndef LUGALOS_DRIVERS_PSRAM_RP2350_H
#define LUGALOS_DRIVERS_PSRAM_RP2350_H

/* The RP2350's QSPI PSRAM on QMI window 1 -- 38.2, plan/phase38_psram.md.
 *
 * Built where the board file names one (CONFIG_PSRAM_CS_GPIO and
 * CONFIG_PSRAM_BYTES: today only the RP2350-LCD-7). On every other build
 * these calls do not exist, and callers say so with the same #if. */

#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PSRAM_BYTES)

#define PSRAM_CACHED_BASE    0x11000000u   /* through the 16 KB XIP cache */
#define PSRAM_UNCACHED_BASE  0x15000000u   /* the same chip, no cache */

/* Early boot, before palloc_init(), interrupts still off: GP0 to XIP_CS1, the
 * chip's ID, reset, QPI, window 1's timing from CONFIG_CLK_SYS_HZ, writes
 * enabled, size by aliasing checked against CONFIG_PSRAM_BYTES. Logs one
 * line either way. Never halts: the console that would carry the reason does
 * not exist yet -- psram_require() does that. */
void psram_init(void);

bool     psram_is_up(void);
uint32_t psram_bytes(void);        /* 0 when not up */

/* Sign-off S1: a persona built for PSRAM that has none does not run. Called
 * once the console, USB and the panel are up; returns at once when PSRAM is
 * up, and otherwise never returns -- it repeats the reason every few seconds
 * on the console and the panel, and leaves USB running so the board can still
 * be reflashed with the 1200-baud touch. */
void psram_require(void);

/* `psram`, `psram test`, `psram bench`. */
void psram_command(const char *args);

/* One line for /proc/meminfo; returns the bytes written. */
int psram_meminfo(char *buf, uint32_t cap);

#endif

#endif /* LUGALOS_DRIVERS_PSRAM_RP2350_H */
