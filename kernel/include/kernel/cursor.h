#ifndef LUGALOS_KERNEL_CURSOR_H
#define LUGALOS_KERNEL_CURSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "kernel/cursor_shape.h"

/* The mouse cursor on an RGB565 scan-out the CPU composes (the VisionFive
 * 2's DC8200, kernel/cursor.c): drawn where the console's pointer is and
 * while it shows (console_pointer_shown()), over the frame before it is
 * flipped, and taken off it again before the frame is drawn into. Up to two
 * buffers (a front and a back), each remembering where it holds the cursor. */
#define CURSOR_W CURSOR_SHAPE_PX
#define CURSOR_H CURSOR_SHAPE_PX

void cursor_init(void);

/* The cursor onto fb (fb_w x fb_h RGB565), first taken off where fb had it. */
void cursor_render(uint16_t *fb, unsigned fb_w, unsigned fb_h);
/* The cursor taken off fb, what was under it put back. */
void cursor_restore(uint16_t *fb, unsigned fb_w, unsigned fb_h);

/* What puts back fb's pixels under a cursor at (x, y), when the backend can
 * redraw them from its own source (the 1-bpp terminal frame); without one,
 * the pixels saved when it was drawn. */
typedef void (*cursor_restore_fn)(uint16_t *fb, unsigned fb_w, unsigned fb_h, int x, int y);
void cursor_set_restore_cb(cursor_restore_fn cb);

#endif /* LUGALOS_KERNEL_CURSOR_H */
