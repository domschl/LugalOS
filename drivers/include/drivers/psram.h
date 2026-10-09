#ifndef LUGALOS_DRIVERS_PSRAM_H
#define LUGALOS_DRIVERS_PSRAM_H

/* The board's PSRAM driver, whichever it is (47.3,
 * plan/phase47_esp32p4_lcd7b_ribbon.md). Both implement the interface phase
 * 38 defined -- psram_init/is_up/bytes/require/command/meminfo and the
 * PSRAM_CACHED_BASE / PSRAM_UNCACHED_BASE pair -- and both are compiled only
 * where the board file names a PSRAM (CONFIG_PSRAM_BYTES), so callers test
 * that and nothing else. */
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_RP2350)
#include "drivers/psram_rp2350.h"
#elif defined(CONFIG_BOARD_ESP32P4)
#include "drivers/psram_esp32p4.h"
#endif

#endif /* LUGALOS_DRIVERS_PSRAM_H */
