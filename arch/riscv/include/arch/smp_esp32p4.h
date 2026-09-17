/* The ESP32-P4's second HP core. 34.9,
 * plan/phase34_esp32p4_pll_bringup.md.
 *
 * At 34.9 core 1's entire program is a counter, which is the point -- see
 * the .c file for why proving that separately from everything above it is
 * worth its own milestone.
 */
#ifndef ARCH_SMP_ESP32P4_H
#define ARCH_SMP_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* Incremented by core 1, read by core 0. Lives in .bss, so a non-zero value
 * can only have been written by the other core. */
extern volatile uint32_t g_p4_core1_ticks;

/* Unstall core 1, give it a clock, release its reset and hand it a boot
 * address. Returns false if it would not come out of the stall. */
bool esp32p4_core1_launch_probe(void);

/* The `smpstart` shell command's body: launch, wait 100 ms, and say whether
 * the counter moved. */
void esp32p4_core1_probe_report(void);

#endif /* CONFIG_BOARD_ESP32P4 */
#endif /* ARCH_SMP_ESP32P4_H */
