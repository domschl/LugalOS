/* The ESP32-P4's clock tree, read back from its own registers.
 * 34.1, plan/phase34_esp32p4_pll_bringup.md.
 *
 * Read-only at 34.1. This header is where the PLL switch will land in
 * 34.3-34.5, which is why the file exists now rather than the reporting
 * living in kernel/shell.c: the milestone that changes these clocks and the
 * milestone that reads them back should not be in different places.
 */
#ifndef ARCH_CLK_ESP32P4_H
#define ARCH_CLK_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* Everything the four root clocks are made of, as read this instant.
 *
 * The frequencies are *derived* from the dividers and the source select, not
 * measured -- this chip has no register that reports a frequency. What makes
 * them trustworthy is that the derivation is checkable against two things
 * that are independent of it: the ROM's own belief (bookkeeping) and the
 * ticker's measured CLINT rate (a real measurement, against the crystal).
 * esp32p4_clocks_report() prints all three for exactly that reason. */
typedef struct {
    uint8_t  src;          /* raw LP_CLKRST HP_ROOT_CLK_SRC_SEL, 0..3 */
    uint32_t root_hz;      /* HP_ROOT_CLK, from `src` */

    /* The dividers are a *cascade*, not four taps off the root. See the
     * .c file: CPU = ROOT/cpu, MEM = CPU/mem, SYS = MEM/sys, APB = SYS/apb. */
    uint32_t cpu_div, cpu_num, cpu_den;   /* CPU's divider is fractional */
    uint32_t mem_div, sys_div, apb_div;

    uint32_t cpu_hz, mem_hz, sys_hz, apb_hz;
} esp32p4_clocks_t;

/* Reads the registers. Touches nothing else and writes nothing. */
void esp32p4_clocks_read(esp32p4_clocks_t *c);

/* What the boot ROM believes the CPU frequency is, in MHz.
 *
 * Bookkeeping, not a measurement: the ROM keeps this figure so its own
 * microsecond delay loops are calibrated, and it only changes when somebody
 * calls ets_update_cpu_frequency(). Before 34.4 nobody ever has, so this is
 * also a check that the ROM was handed a board it understands. */
uint32_t esp32p4_rom_cpu_freq_mhz(void);

/* The CLINT's tick rate, measured over `window_us` against the systimer --
 * which runs from the crystal, so this does not move when the CPU does.
 *
 * The only figure on this board that is measured rather than derived from a
 * divider, and therefore the independent check on everything above. */
uint32_t esp32p4_clint_measure_hz(uint32_t window_us);

/* 34.2: CPLL's configuration, what the flash is clocked from, and the state
 * the boot ROM left the HP regulator in. Printed as part of `clocks`.
 *
 * Reads only, in the sense that matters -- no clock, divider or power state
 * is changed. It does drive the analog I2C master to interrogate CPLL, which
 * necessarily writes that master's own selector and command word; see the .c
 * file for why that is not a write to the thing being measured. */
void esp32p4_clock_sources_report(void);

/* The CPU clock rate, measured with `mcycle` against the crystal-referenced
 * systimer over `window_us`. Direct: it counts the clock this phase changes,
 * rather than deriving it from a divider whose encoding 34.2 found untrusted.
 */
uint32_t esp32p4_cpu_measure_hz(uint32_t window_us);

/* 34.3: apply this chip's own eFuse voltage trim to the HP_ACTIVE regulator,
 * before any milestone raises a clock against it.
 *
 * Returns true if it changed anything. `from` and `to` receive the *control*
 * field (HP_ACTIVE_HP_REGULATOR_DBIAS) before and after; `indicated` receives
 * the separate RO field the regulator reports its current voltage in. Those
 * two disagree on this board and the .c file says so at length -- both are
 * reported rather than one being chosen.
 *
 * Never lowers the setting and never invents one: an unburnt eFuse leaves
 * the register alone, because IDF's fallback for that case is the value
 * already there. See the .c file for why the DCDC is deliberately untouched.
 */
bool esp32p4_regulator_apply_efuse_dbias(uint32_t *from, uint32_t *to,
                                         uint32_t *indicated);

/* 34.4: point HP_ROOT_CLK at CPLL and set the four dividers for `mhz`.
 *
 * Accepts only 40 (the crystal, a no-op) and the three CPLL-derived steps
 * ESP-IDF enumerates for this silicon -- 90, 180, 360 -- and returns false
 * for anything else rather than computing dividers, because a combination
 * outside that table can be silently corrected by hardware without the
 * registers reflecting it. Returns true if it switched.
 *
 * `mhz` names an entry in ESP-IDF's divider table, whose nominal assumes
 * CPLL is 360 MHz. This board's ROM configures CPLL to 320, so the entry
 * called "90" yields 80 -- which is why `measured_hz` exists and why the ROM
 * is told the measured figure rather than the requested one.
 */
bool esp32p4_cpu_freq_set(uint32_t mhz, uint32_t *measured_hz);

/* The `clocks` shell command's body. */
void esp32p4_clocks_report(void);

#endif /* CONFIG_BOARD_ESP32P4 */
#endif /* ARCH_CLK_ESP32P4_H */
