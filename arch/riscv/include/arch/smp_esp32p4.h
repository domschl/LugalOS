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
 * address. Returns false if it would not come out of the stall.
 *
 * `into_kernel` selects what it is pointed at: false is 34.9's counter, true
 * is `_start` and entry.S's secondary path. The counter stays reachable for
 * the reason kernel/smp.c gives about RP2350's mode dispatch -- it is the
 * only state of this path ever proven on silicon, and a bring-up whose
 * fallback is `git revert` is one done blind. */
bool esp32p4_core1_launch(bool into_kernel);
bool esp32p4_core1_launch_probe(void);

/* 34.10: per-hart state core 1 must set for itself -- the branch predictor,
 * which is a CSR and therefore cannot be inherited. Called from
 * secondary_main() before anything else. */
void esp32p4_core1_early_init(void);

/* What core 1's MHCR read on arrival and after esp32p4_core1_early_init()
 * set it. Written by core 1, read by core 0 -- free on this chip, the L1
 * data cache being shared. Both zero until core 1 has entered the kernel. */
extern volatile uint32_t g_p4_core1_mhcr_before;
extern volatile uint32_t g_p4_core1_mhcr_after;
uint32_t esp32p4_mhcr_read(void);

/* The `smpstart` shell command's body: launch, wait 100 ms, and say whether
 * the counter moved. */
void esp32p4_core1_probe_report(void);

#endif /* CONFIG_BOARD_ESP32P4 */
#endif /* ARCH_SMP_ESP32P4_H */
