/*
 * A screen in RAM, for the targets without one -- 37.3b,
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
 * frame (800 x 480: 12 pages) and the screen's state (2) -- the panel's
 * size by default, so that layouts and canvas sizes are the panel's too --
 * and, from the bulk zone, the canvas stores (47 pages, 38.8).
 */

#include "drivers/ramscreen.h"
#include "drivers/screen.h"
#include "kernel/palloc.h"

#include <stddef.h>

/* The panel's size by default, so layouts and canvas sizes are the
 * RP2350-LCD-7's. 47.6: settable (`ramscreen W H`), so the suite also runs
 * them at the LCD-7B's 1024 x 600 with the widths D7 derives from it. */
static unsigned g_rs_w = 800u, g_rs_h = 480u;
#define RS_FB    (g_rs_w / 8u * g_rs_h)
#define RS_PAGES(bytes) (((bytes) + PAGE_SIZE - 1u) / PAGE_SIZE)

static screen_t *g_rs;
static void     *g_rs_fb, *g_rs_store;
static unsigned  g_rs_built_w, g_rs_built_h;   /* the size g_rs was built at */

static bool rs_canvas(const uint8_t *req, uint32_t n, uint8_t *reply) {
    if (!g_rs) {
        void *fb = palloc_pages(RS_PAGES(RS_FB));
        if (!fb) return false;
        screen_t *scr = (screen_t *)palloc_pages(RS_PAGES(SCREEN_BYTES(g_rs_w, g_rs_h)));
        if (!scr) {
            palloc_free(fb, RS_PAGES(RS_FB));
            return false;
        }
        screen_init(scr, fb, g_rs_w / 8u, g_rs_w, g_rs_h);
        /* 38.8: the canvas stores, from the bulk zone -- QEMU's stand-in --
         * so the suite runs them; without one, the canvas is not kept. */
        void *st = palloc_pages_bulk(RS_PAGES(SCREEN_STORE_BYTES(g_rs_w, g_rs_h)));
        if (st) screen_set_store(scr, st, RS_PAGES(SCREEN_STORE_BYTES(g_rs_w, g_rs_h)) * PAGE_SIZE);
        g_rs = scr;
        g_rs_fb = fb;
        g_rs_store = st;
        g_rs_built_w = g_rs_w;
        g_rs_built_h = g_rs_h;
    }
    screen_canvas(g_rs, req, n, reply);
    return true;
}

/* 37.5a: what a screenshot saves -- once something has drawn. */
static bool rs_pixels(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride) {
    if (!g_rs) return false;
    *fb = g_rs->cv.fb;
    *w = g_rs_built_w;
    *h = g_rs_built_h;
    *stride = g_rs_built_w / 8u;
    return true;
}

int ramscreen_set_size(unsigned w, unsigned h) {
    if (w < 320u || h < 240u || w > 1600u || h > 1200u || (w % 32u) != 0) return -1;
    if (g_rs) {
        palloc_free(g_rs_fb, RS_PAGES(g_rs_built_w / 8u * g_rs_built_h));
        palloc_free(g_rs, RS_PAGES(SCREEN_BYTES(g_rs_built_w, g_rs_built_h)));
        if (g_rs_store) palloc_free(g_rs_store, RS_PAGES(SCREEN_STORE_BYTES(g_rs_built_w, g_rs_built_h)));
        g_rs = NULL;
        g_rs_fb = g_rs_store = NULL;
    }
    g_rs_w = w;
    g_rs_h = h;
    return 0;
}

void ramscreen_size(unsigned *w, unsigned *h) {
    *w = g_rs_w;
    *h = g_rs_h;
}

static const console_screen_t g_ramscreen = { .canvas = rs_canvas, .pixels = rs_pixels };

const console_screen_t *ramscreen_console(void) {
    return &g_ramscreen;
}

canvas1_t *ramscreen_get_canvas_backing(uint8_t cid) {
    if (!g_rs || cid >= SCREEN_MAX_CANVASES || !g_rs->canvases[cid].in_use) return NULL;
    return &g_rs->canvases[cid].backing;
}

canvas1_t *ramscreen_get_reader_backing(uint8_t rid) {
    if (!g_rs || rid >= SCREEN_MAX_READERS || !g_rs->readers[rid].in_use) return NULL;
    return &g_rs->readers[rid].backing;
}
