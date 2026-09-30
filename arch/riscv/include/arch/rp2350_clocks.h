/*
 * arch/riscv/include/arch/rp2350_clocks.h -- the RP2350's clk_sys, as one
 * board fact instead of a literal in every driver.
 *
 * 36.1, plan/phase36_rp2350_lcd7_terminal.md §3.3. Until this existed,
 * `150000000` (or "150 MHz" in a comment beside a hand-computed divider) was
 * written into boot_header.S, uart_rp2350.c, uart1_link_rp2350.c,
 * gps_pps_rp2350.c, i2c_bus.c, spisd_rp2350.c and pico_clock_green_rp2350.c
 * separately. The RP2350-LCD-7 persona needs 144 MHz -- PIO-USB wants clk_sys
 * to be a multiple of 12 MHz, and the panel wants an integer 24 MHz PCLK --
 * and seven files agreeing on a number by coincidence is not a seam.
 *
 * clk_peri is attached to clk_sys on every persona (uart_init() does it), so
 * every UART/SPI/I2C/PWM divider derives from this one constant.
 *
 * Included from assembly (boot_header.S) as well as C: preprocessor defines
 * and integer arithmetic only.
 *
 * PLL provenance (RP2350 datasheet, RP-008373-DS-2, §8.6 "PLL"):
 *   FOUTPOSTDIV = (FREF / REFDIV) * FBDIV / (POSTDIV1 * POSTDIV2)
 *   VCO 750..1600 MHz; with the 12 MHz crystal and REFDIV = 1, FBDIV 63..133
 *   (the datasheet's own worked range). Max clk_sys from PLL_SYS: 150 MHz.
 *   CS: [5:0] REFDIV, [31] LOCK. FBDIV_INT: [11:0]. PRIM: [18:16] POSTDIV1,
 *   [14:12] POSTDIV2.
 */
#ifndef ARCH_RP2350_CLOCKS_H
#define ARCH_RP2350_CLOCKS_H

#include "lugalos_config.h"

/* The board fact. Only the persona that needs a different clock sets it;
 * everything else keeps the frequency this tree has always run at. */
#ifndef CONFIG_CLK_SYS_HZ
#define CONFIG_CLK_SYS_HZ 150000000
#endif

#define RP2350_XOSC_HZ          12000000
#define RP2350_PLL_SYS_REFDIV   1
#define RP2350_PLL_SYS_POSTDIV1 5
#define RP2350_PLL_SYS_POSTDIV2 2

/* VCO = clk_sys * 10 with the fixed post-dividers above, so FBDIV =
 * clk_sys * 10 / 12 MHz = clk_sys / 1.2 MHz. 150 MHz -> 125 (the value
 * boot_header.S always had), 144 MHz -> 120. */
#define RP2350_PLL_SYS_FBDIV    (CONFIG_CLK_SYS_HZ / 1200000)

/* The same checks, in the two languages this header is read by. The C ones
 * sit in drivers/clocks_rp2350.c; boot_header.S has the .if versions. */
#define RP2350_CLK_SYS_VALID \
    ((CONFIG_CLK_SYS_HZ % 1200000) == 0 && \
     RP2350_PLL_SYS_FBDIV >= 63 && RP2350_PLL_SYS_FBDIV <= 133 && \
     CONFIG_CLK_SYS_HZ <= 150000000)

#endif /* ARCH_RP2350_CLOCKS_H */
