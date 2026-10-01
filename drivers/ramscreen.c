/*
 * A screen in RAM, for the targets without one -- 37.3b,
 * plan/phase37_screen_layouts_and_apps.md §2.
 *
 * It exists so that Lisp's canvas -- and the canvas protocol, and the
 * layouts behind it -- run, and are tested, on QEMU exactly as on the
 * RP2350-LCD-7: the same drivers/screen.c, the same requests, the same
 * replies, only no panel scans the pixels out. `(canvas-get x y)` reads them
 * back, which is what the runner's tests do.
 *
 * Only the canvas: it offers no size and no title, so console_size() still
 * says "unknown" on a serial line, and nothing a program prints goes into
 * it. It costs nothing until the first canvas request, which allocates the
 * 800 x 480 frame (12 pages) and the screen's state (2) -- the panel's size,
 * so that layouts and canvas sizes are the panel's too.
 */

#include "drivers/ramscreen.h"
#include "drivers/screen.h"
#include "kernel/palloc.h"

#define RS_W     800u
#define RS_H     480u
#define RS_FB    (RS_W / 8u * RS_H)
#define RS_PAGES(bytes) (((bytes) + PAGE_SIZE - 1u) / PAGE_SIZE)

static screen_t *g_rs;

static bool rs_canvas(const uint8_t *req, uint32_t n, uint8_t *reply) {
    if (!g_rs) {
        void *fb = palloc_pages(RS_PAGES(RS_FB));
        if (!fb) return false;
        screen_t *scr = (screen_t *)palloc_pages(RS_PAGES(SCREEN_BYTES(RS_W, RS_H)));
        if (!scr) {
            palloc_free(fb, RS_PAGES(RS_FB));
            return false;
        }
        screen_init(scr, fb, RS_W / 8u, RS_W, RS_H);
        g_rs = scr;
    }
    screen_canvas(g_rs, req, n, reply);
    return true;
}

static const console_screen_t g_ramscreen = { .canvas = rs_canvas };

const console_screen_t *ramscreen_console(void) {
    return &g_ramscreen;
}
