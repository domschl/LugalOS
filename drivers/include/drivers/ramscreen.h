#ifndef LUGALOS_DRIVERS_RAMSCREEN_H
#define LUGALOS_DRIVERS_RAMSCREEN_H

#include "kernel/console.h"
#include "drivers/canvas1.h"

/* 37.3b: a canvas-only screen in RAM for the targets without a panel, so that
 * Lisp's canvas runs and is tested on QEMU as on the RP2350-LCD-7. See
 * drivers/ramscreen.c. Bound at boot by kernel/main.c where there is no
 * other screen. */
const console_screen_t *ramscreen_console(void);

/* 47.6: the RAM screen's size, 800 x 480 by default. Setting it frees the
 * screen built so far (its canvases with it); the next canvas request builds
 * one at the new size. w a multiple of 32, 320..1600; h 240..1200. -1 if
 * refused. */
int ramscreen_set_size(unsigned w, unsigned h);
void ramscreen_size(unsigned *w, unsigned *h);
canvas1_t *ramscreen_get_canvas_backing(uint8_t cid);
canvas1_t *ramscreen_get_reader_backing(uint8_t rid);

#endif /* LUGALOS_DRIVERS_RAMSCREEN_H */
