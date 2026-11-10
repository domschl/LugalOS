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
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "kernel/palloc.h"
#include "kernel/console.h"

#include <stddef.h>
#include <string.h>

/* The panel's size by default, so layouts and canvas sizes are the
 * RP2350-LCD-7's. 47.6: settable (`ramscreen W H`), so the suite also runs
 * them at the LCD-7B's 1024 x 600 with the widths D7 derives from it. */
/* accepted: `ramscreen W H` sets the size before a screen is built. */
static unsigned g_rs_w = 800u, g_rs_h = 480u;
#define RS_FB    (g_rs_w / 8u * g_rs_h)
#define RS_PAGES(bytes) (((bytes) + PAGE_SIZE - 1u) / PAGE_SIZE)

/* A screen that turns (`ramscreen turn`, 76.3) where the ribbon runs down a
 * portrait screen too: its terminals' shadows hold either shape. */
#define RS_ROTATES (RIBBON_AXIS_Y_SUPPORTED != 0)

/* guarded by: console_lock (rs_canvas()); `ramscreen W H` sets the size before a screen exists. */
static screen_t *g_rs;
static void     *g_rs_fb, *g_rs_store;
static unsigned  g_rs_built_w, g_rs_built_h;   /* guarded by: console_lock; the size g_rs was built at */

static bool rs_canvas_locked(const uint8_t *req, uint32_t n, uint8_t *reply) {
    if (!g_rs) {
        if (n > 0 && (req[0] == 'S' || req[0] == 0)) {
            if (reply) {
                memset(reply, 0, SCREEN_REPLY_LEN);
                reply[8] = SCREEN_LAYOUT_TEXT;
            }
            return true;
        }
        void *fb = palloc_pages(RS_PAGES(RS_FB));
        if (!fb) return false;
        screen_t *scr = (screen_t *)palloc_pages(RS_PAGES(SCREEN_BYTES_ROT(g_rs_w, g_rs_h, RS_ROTATES)));
        if (!scr) {
            palloc_free(fb, RS_PAGES(RS_FB));
            return false;
        }
        screen_init_rot(scr, fb, g_rs_w / 8u, g_rs_w, g_rs_h, RS_ROTATES);
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
    screen_report_windows(g_rs);
    return true;
}

/* Under console_lock, as the panel terminal's canvas: the screen is built on
 * the first request, and any task's canvas call can be that one. */
static bool rs_canvas(const uint8_t *req, uint32_t n, uint8_t *reply) {
    console_lock();
    bool ok = rs_canvas_locked(req, n, reply);
    console_unlock();
    return ok;
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
        palloc_free(g_rs, RS_PAGES(SCREEN_BYTES_ROT(g_rs_built_w, g_rs_built_h, RS_ROTATES)));
        if (g_rs_store) palloc_free(g_rs_store, RS_PAGES(SCREEN_STORE_BYTES(g_rs_built_w, g_rs_built_h)));
        g_rs = NULL;
        g_rs_fb = g_rs_store = NULL;
    }
    g_rs_w = w;
    g_rs_h = h;
    return 0;
}

int ramscreen_turn(void) {
    if (!RS_ROTATES || (g_rs_h % 32u) != 0) return -1;
    console_lock();
    unsigned w = g_rs_h, h = g_rs_w;
    g_rs_w = w;
    g_rs_h = h;
    if (g_rs) {
        /* The frame and the stores hold w x h bytes either way round. */
        screen_reorient(g_rs, g_rs_fb, w / 8u, w, h);
        g_rs_built_w = w;
        g_rs_built_h = h;
        screen_report_windows(g_rs);
    }
    console_unlock();
    return 0;
}

/* accepted: one byte read outside console_lock, as lcdterm_ribbon_down(). */
bool ramscreen_ribbon_down(void) {
    return g_rs && RIBBON_AXIS(&g_rs->ribbon) == RIBBON_AXIS_Y;
}

/* accepted: as ramscreen_ribbon_down(). */
bool ramscreen_fullscreen(void) {
    return g_rs && screen_fullscreen(g_rs);
}

unsigned ramscreen_handle_at(int x, int y) {
    return g_rs ? screen_handle_at(g_rs, x, y) : 0;
}

uint8_t ramscreen_fs_state(void) {
    return g_rs ? screen_fs_state(g_rs) : 0;
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

int ramscreen_get_canvas_cid_by_name(const char *name) {
    if (!g_rs || !name) return -1;
    for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
        if (g_rs->canvases[i].in_use && strcmp(g_rs->canvases[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool ramscreen_window_at(int x, int y, uint8_t *type, uint8_t *id, int *cx, int *cy) {
    return g_rs && screen_window_at(g_rs, x, y, type, id, cx, cy);
}

bool ramscreen_focused_window(uint8_t *type, uint8_t *id) {
    return g_rs && screen_focused_window(g_rs, type, id);
}

canvas1_t *ramscreen_get_reader_backing(uint8_t rid) {
    if (!g_rs || rid >= SCREEN_MAX_READERS || !g_rs->readers[rid].in_use) return NULL;
    return &g_rs->readers[rid].backing;
}
