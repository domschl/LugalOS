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
/* 76.3: turns the screen a quarter (w x h becomes h x w), its windows kept,
 * as the Tab5's panel turns. -1 where the ribbon has no vertical axis, or
 * the new width is not a multiple of 32. */
int ramscreen_turn(void);
/* Whether its ribbon runs down it now (after a turn to portrait). */
bool ramscreen_ribbon_down(void);
/* Whether a window covers it (full screen). */
bool ramscreen_fullscreen(void);
uint8_t ramscreen_fs_state(void);
canvas1_t *ramscreen_get_canvas_backing(uint8_t cid);
int        ramscreen_get_canvas_cid_by_name(const char *name);
canvas1_t *ramscreen_get_reader_backing(uint8_t rid);
/* The RAM screen's windows, as lcdterm_window_at() / _focused_window() are
 * the panel's: so pointer routing runs on QEMU. */
bool       ramscreen_window_at(int x, int y, uint8_t *type, uint8_t *id, int *cx, int *cy);
bool       ramscreen_focused_window(uint8_t *type, uint8_t *id);

#endif /* LUGALOS_DRIVERS_RAMSCREEN_H */
