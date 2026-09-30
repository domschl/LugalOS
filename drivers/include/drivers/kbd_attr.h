#ifndef LUGALOS_DRIVERS_KBD_ATTR_H
#define LUGALOS_DRIVERS_KBD_ATTR_H

#include "lugalos_config.h"

/* Where the USB keyboard task's code and constant data live -- 36.8,
 * plan/phase36_rp2350_lcd7_terminal.md. The same arrangement as
 * drivers/lcdterm_attr.h: on the RP2350-LCD-7 the `kbd` task runs in U-mode
 * and can execute and read only its own section, `.kbdtext`
 * (linker/rp2350.ld); everywhere else the attributes are empty and the
 * portable half (drivers/usbkbd.c) is ordinary kernel code, which is what
 * lets `usbkbdselftest` run on QEMU against what the board executes. */
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)
#define KBD_UTEXT   __attribute__((section(".kbdtext"))) __attribute__((no_sanitize("undefined")))
#define KBD_URODATA __attribute__((section(".kbdrodata")))
#else
#define KBD_UTEXT
#define KBD_URODATA
#endif

#endif /* LUGALOS_DRIVERS_KBD_ATTR_H */
