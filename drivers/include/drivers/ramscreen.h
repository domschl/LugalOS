#ifndef LUGALOS_DRIVERS_RAMSCREEN_H
#define LUGALOS_DRIVERS_RAMSCREEN_H

#include "kernel/console.h"

/* 37.3b: a canvas-only screen in RAM for the targets without a panel, so that
 * Lisp's canvas runs and is tested on QEMU as on the RP2350-LCD-7. See
 * drivers/ramscreen.c. Bound at boot by kernel/main.c where there is no
 * other screen. */
const console_screen_t *ramscreen_console(void);

#endif /* LUGALOS_DRIVERS_RAMSCREEN_H */
