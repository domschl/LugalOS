#ifndef LUGALOS_DRIVERS_CLOCKS_RP2350_H
#define LUGALOS_DRIVERS_CLOCKS_RP2350_H

/* 36.1, plan/phase36_rp2350_lcd7_terminal.md: clk_sys read back from PLL_SYS
 * and CLK_SYS_CTRL/DIV, and measured with mcycle against the crystal-driven
 * TIMER0. The `clocks` shell command on RP2350. */
void rp2350_clocks_report(void);

/* One boot-log line: measured MHz, and whether PLL_SYS agrees with
 * CONFIG_CLK_SYS_HZ. */
void rp2350_clocks_boot_check(void);

#endif /* LUGALOS_DRIVERS_CLOCKS_RP2350_H */
