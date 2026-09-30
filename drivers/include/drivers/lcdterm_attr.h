#ifndef LUGALOS_DRIVERS_LCDTERM_ATTR_H
#define LUGALOS_DRIVERS_LCDTERM_ATTR_H

#include "lugalos_config.h"

/* Where the screen terminal's code and constant data live -- 36.6a,
 * plan/phase36_rp2350_lcd7_terminal.md.
 *
 * On the RP2350-LCD-7 the terminal emulator runs as the U-mode `lcdterm`
 * task, and a U-mode task can execute and read only what its domain grants:
 * its own text section. So everything the task reaches -- vtterm's putc
 * path, fbtext, and the font tables -- is placed in `.lcdtermtext`
 * (linker/rp2350.ld), which the domain grants R/X, the shape every other
 * RP2350 driver task already has (.st7735text, .blktext, ...).
 * no_sanitize("undefined"): the UBSan handlers live in kernel text, which a
 * U-mode fault path could not reach.
 *
 * Everywhere else -- QEMU, the other personas -- the attributes are empty and
 * the same code is ordinary kernel code, which is what lets `vtselftest` run
 * on QEMU against exactly what the board executes. */
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)
#define LCDTERM_UTEXT   __attribute__((section(".lcdtermtext"))) __attribute__((no_sanitize("undefined")))
#define LCDTERM_URODATA __attribute__((section(".lcdtermrodata")))
#else
#define LCDTERM_UTEXT
#define LCDTERM_URODATA
#endif

#endif /* LUGALOS_DRIVERS_LCDTERM_ATTR_H */
