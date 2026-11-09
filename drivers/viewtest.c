/* drivers/viewtest.c -- `viewtest`: a view with one button and a count of
 * what it was told, for checking that a view needs nothing of its own in the
 * screen (drivers/screen.c) or the P4's compositor (drivers/dsi_esp32p4.c):
 * its window type, these ops and view_for_window()'s entry are all of it. */

#include "drivers/viewtest.h"
#include "drivers/view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/ugui.h"
#include "drivers/ramscreen.h"
#include "drivers/lcdterm.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "lugalos_config.h"

#if CONFIG_DEV && !(defined(CONFIG_BOARD_ESP32C6) || (defined(CONFIG_BOARD_RP2350) && !defined(CONFIG_LCD_PCLK_GPIO)))

#define VT_ID_BUTTON 1

/* accepted: written on the views task only (view_call() for the shell's open); the
 * shell's status reads it after view_sync(), a count one behind at worst. */
static struct {
    bool       active;
    uint8_t    slot;
    int        w, h;
    unsigned   taps, button, keys, resizes, focus, other;
    uint32_t   seq;
    ugui_ctx_t ui;
} s_vt BULK_BSS;

static canvas1_t *vt_backing(void) {
#if defined(LCDTERM_PRESENT)
    canvas1_t *back = lcdterm_get_reader_backing(s_vt.slot);
    if (back) return back;
#endif
    return ramscreen_get_reader_backing(s_vt.slot);
}

static void vt_draw(void) {
    canvas1_t *back = vt_backing();
    if (!s_vt.active || !back || s_vt.w <= 0 || s_vt.h <= 0) return;
    ugui_surface_t surf = { .c1 = back, .w = s_vt.w, .h = s_vt.h, .scale = CHROME_SCALE };
    ugui_begin(&s_vt.ui);
    ugui_fill(&surf, (ugui_rect_t){ 0, 0, (int16_t)s_vt.w, (int16_t)s_vt.h }, UGUI_GRAY_PAPER, CANVAS1_WHITE);
    ugui_button(&surf, &s_vt.ui, (ugui_rect_t){ 16, 16, 160, 40 }, "Press", VT_ID_BUTTON, false);
    ugui_end(&surf, &s_vt.ui);
    s_vt.seq++;
    uint8_t req[2] = { 'W', s_vt.slot }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);
}

static bool vt_active(uint8_t inst) { return s_vt.active && inst == s_vt.slot; }

static void vt_event(uint8_t inst, const win_event_t *ev) {
    if (!vt_active(inst)) return;
    switch (ev->type) {
    case WIN_EV_POINTER_UP:
        if (ev->flags & WIN_EVF_TAP) {
            s_vt.taps++;
            int16_t idx;
            if (ugui_hit_test(&s_vt.ui, ev->x, ev->y, &idx) == VT_ID_BUTTON) s_vt.button++;
        }
        return;
    case WIN_EV_KEY_DOWN:
        s_vt.keys++;
        return;
    case WIN_EV_FOCUS_GAINED:
    case WIN_EV_FOCUS_LOST:
        s_vt.focus++;
        return;
    default:
        s_vt.other++;
        return;
    }
}

static void vt_resize(uint8_t inst, int w, int h) {
    if (!vt_active(inst)) return;
    s_vt.resizes++;
    if (w == s_vt.w && h == s_vt.h) return;
    s_vt.w = w;
    s_vt.h = h;
    vt_draw();
}

static void vt_close(uint8_t inst) {
    if (vt_active(inst)) s_vt.active = false;
}

static uint8_t *vt_surface(uint8_t inst, uint32_t *stride, int *w, int *h) {
    (void)inst; (void)stride; (void)w; (void)h;
    return NULL;                                         /* 1-bpp only: the compositor converts it */
}

static uint32_t vt_seq(uint8_t inst) {
    (void)inst;
    return s_vt.seq;
}

const view_ops_t g_viewtest_view = {
    .name = "viewtest",
    .active = vt_active,
    .event = vt_event,
    .resize = vt_resize,
    .close = vt_close,
    .surface = vt_surface,
    .seq = vt_seq,
};

typedef struct { int op; bool ok; } vt_call_t;

static void vt_call(void *arg) {
    vt_call_t *c = arg;
    if (c->op == 0) {                                    /* open */
        if (s_vt.active) { c->ok = true; return; }
        uint8_t req[3 + 9] = { 'v', RIBBON_WIN_VIEWTEST, SCREEN_VIEW_SINGLETON,
                               'V', 'i', 'e', 'w', 't', 'e', 's', 't', '\0' };
        uint8_t reply[SCREEN_REPLY_LEN];
        if (!console_canvas(req, sizeof req, reply) || reply[0] != 0) { c->ok = false; return; }
        s_vt.active = true;
        s_vt.slot = reply[1];
        s_vt.w = (int)(reply[2] | (reply[3] << 8));
        s_vt.h = (int)(reply[4] | (reply[5] << 8));
        s_vt.taps = s_vt.button = s_vt.keys = s_vt.resizes = s_vt.focus = s_vt.other = 0;
        vt_draw();
        c->ok = true;
    }
}

void viewtest_cmd(const char *arg) {
    while (arg && *arg == ' ') arg++;
    if (arg && arg[0] == 'o') {
        vt_call_t c = { 0, false };
        view_call(vt_call, &c);
        cprintf(c.ok ? "viewtest: open\n" : "viewtest: no window\n");
        return;
    }
    view_sync();
    cprintf("viewtest: %s, slot %u, %d x %d; taps %u (on the button %u), keys %u, "
            "resizes %u, focus %u, other %u\n",
            s_vt.active ? "open" : "closed", (unsigned)s_vt.slot, s_vt.w, s_vt.h,
            s_vt.taps, s_vt.button, s_vt.keys, s_vt.resizes, s_vt.focus, s_vt.other);
}

#else

void viewtest_cmd(const char *arg) {
    (void)arg;
    cprintf("viewtest: no views on this board\n");
}

#endif
