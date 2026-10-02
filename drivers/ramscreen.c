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
 * so that layouts and canvas sizes are the panel's too -- and, from the bulk
 * zone, the canvas stores (47 pages, 38.8).
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
        /* 38.8: the canvas stores, from the bulk zone -- QEMU's stand-in --
         * so the suite runs them; without one, the canvas is not kept. */
        void *st = palloc_pages_bulk(RS_PAGES(SCREEN_STORE_BYTES(RS_W, RS_H)));
        if (st) screen_set_store(scr, st, RS_PAGES(SCREEN_STORE_BYTES(RS_W, RS_H)) * PAGE_SIZE);
        g_rs = scr;
    }
    screen_canvas(g_rs, req, n, reply);
    return true;
}

/* 37.5a: what a screenshot saves -- once something has drawn. */
static bool rs_pixels(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride) {
    if (!g_rs) return false;
    *fb = g_rs->cv.fb;
    *w = RS_W;
    *h = RS_H;
    *stride = RS_W / 8u;
    return true;
}

static const console_screen_t g_ramscreen = { .canvas = rs_canvas, .pixels = rs_pixels };

const console_screen_t *ramscreen_console(void) {
    return &g_ramscreen;
}
