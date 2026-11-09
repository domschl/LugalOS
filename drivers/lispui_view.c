/* drivers/lispui_view.c -- `ugui` for Lisp programs (76.2): the view that
 * draws a Lisp program's display list (drivers/include/drivers/lispui.h).
 *
 * Coordinates: a program lays out in points, which the view multiplies by
 * CHROME_SCALE (two pixels a point on the Tab5's HiDPI panel, one elsewhere),
 * so one layout fits every panel; sizes and event positions come back in
 * points too. */

#include "drivers/lispui.h"

#if defined(LISPUI_PRESENT)

#include "drivers/view.h"
#include "drivers/ribbon.h"
#include "drivers/screen.h"
#include "drivers/ugui.h"
#include "drivers/ramscreen.h"
#include "drivers/lcdterm.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"

#include <stdarg.h>
#include <string.h>

#define LU_LIST_BYTES   6144u   /* one picture's display list */
#define LU_TEXTS        4       /* text fields a window keeps */
#define LU_LISTS        4       /* scrolling lists */
#define LU_SLIDERS      8
#define LU_RING         32      /* events waiting for the program */

typedef struct {
    uint16_t   id;
    bool       used;
    ugui_text_t t;
    char       buf[LISPUI_TEXT_MAX];
} lu_text_t;

typedef struct {
    uint16_t    id;
    bool        used;
    int         scroll;         /* px, kept across pictures */
    ugui_rect_t rect;           /* where the last picture put it */
    int         count, row_h;
} lu_list_t;

typedef struct {
    uint16_t    id;
    ugui_rect_t rect;
    int         value, min, max;
} lu_slider_t;

/* One record of the display list, parsed. */
typedef struct {
    uint8_t     op;
    int         nnums, nstrs;
    int16_t     num[LISPUI_MAX_NUMS];
    const char *str[LISPUI_MAX_STRS];
} lu_rec_t;

/* single task: views -- the window, its kept picture and what the picture's
 * widgets keep (text, scroll); the Lisp task reaches it only through
 * view_call(). */
static struct {
    bool        active;
    uint8_t     slot;
    int         w, h;           /* px */
    uint32_t    seq;
    ugui_ctx_t  ui;
    uint8_t     list[LU_LIST_BYTES];
    uint32_t    len;
    lu_text_t   text[LU_TEXTS];
    lu_list_t   lists[LU_LISTS];
    lu_slider_t sliders[LU_SLIDERS];
    int         nsliders;
    int         press_x, press_y;
    bool        osk_want;
    lu_rec_t    rec;            /* the record being drawn */
} s_lu BULK_BSS;

/* single task: Lisp (under g_lisp_lock) -- the list being built, and whether
 * the program's window is open as far as the program knows. */
static struct {
    uint8_t  buf[LU_LIST_BYTES];
    uint32_t len;
    bool     overflow;
    bool     open;
} s_build BULK_BSS;

/* accepted: one producer and one consumer -- the views task alone writes
 * `head` (after the slot it fills), the Lisp task alone writes `tail` (after
 * the slot it read); each publishes with a release store and reads the
 * other's index with an acquire load. No lock: it would be the only static
 * this needs in SRAM. */
static struct {
    lispui_ev_t ev[LU_RING];
    uint32_t    head, tail;
    uint32_t    dropped;        /* single task: views */
} s_ring BULK_BSS;

#define LU_S CHROME_SCALE

static void lu_push(uint8_t type, uint16_t id, int16_t index, int x, int y, int32_t value) {
    lispui_ev_t e = { type, id, index, (int16_t)(x / LU_S), (int16_t)(y / LU_S), value };
    uint32_t head = s_ring.head;
    uint32_t next = (head + 1u) % LU_RING;
    if (next == __atomic_load_n(&s_ring.tail, __ATOMIC_ACQUIRE)) {
        s_ring.dropped++;
        return;
    }
    s_ring.ev[head] = e;
    __atomic_store_n(&s_ring.head, next, __ATOMIC_RELEASE);
}

/* --- The display list -------------------------------------------------------
 * A record: op, the count of numbers, the count of strings (one byte each),
 * the numbers (int16, little-endian), then the strings, each with its NUL. */

static uint32_t lu_parse(const uint8_t *b, uint32_t len, uint32_t at, lu_rec_t *r) {
    if (at + 3u > len) return 0;
    r->op = b[at];
    r->nnums = b[at + 1];
    r->nstrs = b[at + 2];
    at += 3u;
    if (r->nnums > LISPUI_MAX_NUMS || r->nstrs > LISPUI_MAX_STRS || at + 2u * (uint32_t)r->nnums > len) return 0;
    for (int i = 0; i < r->nnums; i++, at += 2u) r->num[i] = (int16_t)(b[at] | (b[at + 1] << 8));
    for (int i = 0; i < r->nstrs; i++) {
        const char *s = (const char *)b + at;
        while (at < len && b[at]) at++;
        if (at >= len) return 0;
        r->str[i] = s;
        at++;
    }
    for (int i = r->nnums; i < LISPUI_MAX_NUMS; i++) r->num[i] = 0;
    return at;
}

static lu_text_t *lu_text_find(uint16_t id) {
    for (int i = 0; i < LU_TEXTS; i++)
        if (s_lu.text[i].used && s_lu.text[i].id == id) return &s_lu.text[i];
    return NULL;
}

static lu_text_t *lu_text_make(uint16_t id) {
    lu_text_t *t = lu_text_find(id);
    if (t) return t;
    for (int i = 0; i < LU_TEXTS; i++) {
        if (s_lu.text[i].used) continue;
        t = &s_lu.text[i];
        t->used = true;
        t->id = id;
        t->buf[0] = '\0';
        ugui_text_init(&t->t, t->buf, (uint16_t)sizeof t->buf);
        return t;
    }
    return NULL;
}

static lu_list_t *lu_list_make(uint16_t id) {
    lu_list_t *free_one = NULL;
    for (int i = 0; i < LU_LISTS; i++) {
        if (s_lu.lists[i].used && s_lu.lists[i].id == id) return &s_lu.lists[i];
        if (!s_lu.lists[i].used && !free_one) free_one = &s_lu.lists[i];
    }
    if (free_one) {
        memset(free_one, 0, sizeof *free_one);
        free_one->used = true;
        free_one->id = id;
    }
    return free_one;
}

static lu_slider_t *lu_slider_find(uint16_t id) {
    if (!id) return NULL;
    for (int i = 0; i < s_lu.nsliders; i++)
        if (s_lu.sliders[i].id == id) return &s_lu.sliders[i];
    return NULL;
}

static ugui_rect_t lu_rect(const int16_t *n) {
    return (ugui_rect_t){ (int16_t)(n[0] * LU_S), (int16_t)(n[1] * LU_S), (int16_t)(n[2] * LU_S), (int16_t)(n[3] * LU_S) };
}

static void lu_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t row, int index, void *user) {
    const lu_rec_t *r = user;
    if (index < 0 || index >= r->nstrs) return;
    ugui_list_row(s, ctx, row, NULL, r->str[index], NULL, (uint16_t)r->num[0], (int16_t)index, index == r->num[5]);
}

static void lu_draw_rec(const ugui_surface_t *s, ugui_ctx_t *ctx, lu_rec_t *r) {
    const int16_t *n = r->num;
    const char *s0 = r->nstrs > 0 ? r->str[0] : "";
    const char *s1 = r->nstrs > 1 ? r->str[1] : "";
    switch (r->op) {
    case LISPUI_OP_FILL:
        ugui_fill(s, lu_rect(n), (uint8_t)n[4], n[4] >= 128 ? CANVAS1_WHITE : CANVAS1_BLACK);
        break;
    case LISPUI_OP_LABEL:
        ugui_text_serif(s, n[0] * LU_S, n[1] * LU_S, s0, (uint32_t)strlen(s0), n[2] != 0, n[3] != 0, false);
        break;
    case LISPUI_OP_BUTTON:
        ugui_button(s, ctx, lu_rect(n + 1), s0, (uint16_t)n[0], n[5] != 0);
        break;
    case LISPUI_OP_CARD:
        ugui_card(s, ctx, lu_rect(n + 1), NULL, s0, s1, (uint16_t)n[0], n[5] != 0);
        break;
    case LISPUI_OP_CHECKBOX:
        ugui_checkbox(s, ctx, lu_rect(n + 1), s0, n[5] != 0, (uint16_t)n[0]);
        break;
    case LISPUI_OP_TOGGLE:
        ugui_toggle(s, ctx, lu_rect(n + 1), s0, n[5] != 0, (uint16_t)n[0]);
        break;
    case LISPUI_OP_SLIDER: {
        ugui_rect_t rr = lu_rect(n + 1);
        ugui_slider(s, ctx, rr, n[5], n[6], n[7], (uint16_t)n[0]);
        if (s_lu.nsliders < LU_SLIDERS)
            s_lu.sliders[s_lu.nsliders++] = (lu_slider_t){ (uint16_t)n[0], rr, n[5], n[6], n[7] };
        break;
    }
    case LISPUI_OP_PROGRESS:
        ugui_progress(s, lu_rect(n), n[4]);
        break;
    case LISPUI_OP_STEPPER:
        ugui_stepper(s, ctx, lu_rect(n + 2), s0, s1, (uint16_t)n[0], (uint16_t)n[1]);
        break;
    case LISPUI_OP_SEGMENTED:
    case LISPUI_OP_TABS:
        if (r->nstrs > 0) {
            if (r->op == LISPUI_OP_TABS) ugui_tab_bar(s, ctx, lu_rect(n + 1), r->str, r->nstrs, n[5], (uint16_t)n[0]);
            else ugui_segmented(s, ctx, lu_rect(n + 1), r->str, r->nstrs, n[5], (uint16_t)n[0]);
        }
        break;
    case LISPUI_OP_LIST: {
        lu_list_t *l = lu_list_make((uint16_t)n[0]);
        int scroll = l ? l->scroll : 0;
        int row_h = ugui_serif_line_h(s) + 12 * LU_S;
        ugui_rect_t rr = lu_rect(n + 1);
        ugui_fill(s, rr, UGUI_GRAY_CARD, CANVAS1_WHITE);
        (void)ugui_vlist(s, ctx, rr, r->nstrs, row_h, &scroll, lu_row, r);
        if (l) {
            l->scroll = scroll;
            l->rect = rr;
            l->count = r->nstrs;
            l->row_h = row_h;
        }
        break;
    }
    case LISPUI_OP_TEXTFIELD: {
        lu_text_t *t = lu_text_make((uint16_t)n[0]);
        if (t) ugui_textfield(s, ctx, lu_rect(n + 1), &t->t, (uint16_t)n[0]);
        break;
    }
    case LISPUI_OP_SECTION:
        ugui_section_header(s, n[0] * LU_S, n[1] * LU_S, n[2] * LU_S, s0);
        break;
    case LISPUI_OP_DISABLE:
        ugui_disable_next(ctx, (uint8_t)n[0]);
        break;
    case LISPUI_OP_DIALOG:
        if (r->nstrs >= 3)
            (void)ugui_dialog(s, ctx, s0, s1, r->str + 2, r->nstrs - 2, (uint16_t)n[0]);
        break;
    default:
        break;
    }
}

static canvas1_t *lu_backing(void) {
#if defined(LCDTERM_PRESENT)
    canvas1_t *back = lcdterm_get_reader_backing(s_lu.slot);
    if (back) return back;
#endif
    return ramscreen_get_reader_backing(s_lu.slot);
}

/* The kept picture, drawn again, and shown. */
static void lu_draw(void) {
    canvas1_t *back = lu_backing();
    if (!s_lu.active || !back || s_lu.w <= 0 || s_lu.h <= 0) return;
    ugui_surface_t surf = { .c1 = back, .w = s_lu.w, .h = s_lu.h, .scale = LU_S };
    ugui_ctx_t *ctx = &s_lu.ui;
    ugui_begin(ctx);
    ugui_set_min_target(ctx, ugui_target_px());
    ugui_fill(&surf, (ugui_rect_t){ 0, 0, (int16_t)s_lu.w, (int16_t)s_lu.h }, UGUI_GRAY_PAPER, CANVAS1_WHITE);
    s_lu.nsliders = 0;
    uint32_t at = 0;
    while ((at = lu_parse(s_lu.list, s_lu.len, at, &s_lu.rec)) != 0) lu_draw_rec(&surf, ctx, &s_lu.rec);
    ugui_end(&surf, ctx);
    s_lu.seq++;
    uint8_t req[2] = { 'W', s_lu.slot }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);

    /* A text field with the focus: the on-screen keyboard (where there is
     * touch and no keyboard; the console decides). */
    bool want = ctx->focus_id && lu_text_find(ctx->focus_id) && !ctx->modal;
    if (want != s_lu.osk_want) {
        s_lu.osk_want = want;
        console_osk_for_view(RIBBON_WIN_LISPUI, s_lu.slot, want);
    }
}

/* --- Input ------------------------------------------------------------------ */

static void lu_slider_to(lu_slider_t *sl, int x) {
    ugui_surface_t ss = { .scale = LU_S };
    int v = ugui_slider_value(&ss, sl->rect, x, sl->min, sl->max);
    if (v == sl->value) return;
    sl->value = v;
    lu_push(LISPUI_EV_CHANGE, sl->id, 0, 0, 0, v);
}

/* A widget acted on: a tap at x (px), or Enter on its focus (x < 0). */
static void lu_act(uint16_t id, int16_t idx, int x) {
    if (lu_text_find(id)) {
        ugui_set_focus(&s_lu.ui, id, 0);
        lu_draw();
        return;
    }
    lu_slider_t *sl = lu_slider_find(id);
    if (sl) {
        if (x >= 0) lu_slider_to(sl, x);
        return;
    }
    lu_push(LISPUI_EV_PRESS, id, idx, 0, 0, 0);
}

static lu_list_t *lu_list_at(int x, int y) {
    for (int i = 0; i < LU_LISTS; i++) {
        lu_list_t *l = &s_lu.lists[i];
        if (l->used && x >= l->rect.x && x < l->rect.x + l->rect.w && y >= l->rect.y && y < l->rect.y + l->rect.h)
            return l;
    }
    return NULL;
}

static void lu_scroll(lu_list_t *l, int dy_px) {
    if (!l) return;
    int before = l->scroll;
    l->scroll += dy_px;
    int most = l->count * l->row_h - l->rect.h;
    if (l->scroll > most) l->scroll = most;
    if (l->scroll < 0) l->scroll = 0;
    if (l->scroll != before) lu_draw();
}

static void lu_key(uint32_t key, uint8_t mods) {
    ugui_ctx_t *ctx = &s_lu.ui;
    lu_text_t *t = ctx->focus_id ? lu_text_find(ctx->focus_id) : NULL;
    if (t && !ctx->modal) {
        switch (ugui_text_key(&t->t, key, mods)) {
        case UGUI_TEXT_CHANGED: lu_push(LISPUI_EV_CHANGE, t->id, 0, 0, 0, t->t.len); lu_draw(); return;
        case UGUI_TEXT_MOVED:   lu_draw(); return;
        case UGUI_TEXT_SUBMIT:  lu_push(LISPUI_EV_SUBMIT, t->id, 0, 0, 0, t->t.len); lu_draw(); return;
        case UGUI_TEXT_CANCEL:
            ctx->focus_id = 0;
            ctx->focus_shown = false;
            lu_push(LISPUI_EV_CANCEL, t->id, 0, 0, 0, 0);
            lu_draw();
            return;
        default: break;
        }
    }
    lu_slider_t *sl = lu_slider_find(ctx->focus_id);
    if (sl && ctx->focus_shown && (key == KEY_LEFT || key == KEY_RIGHT)) {
        int step = (sl->max - sl->min) / 20;
        int v = sl->value + (key == KEY_RIGHT ? (step > 0 ? step : 1) : -(step > 0 ? step : 1));
        if (v < sl->min) v = sl->min;
        if (v > sl->max) v = sl->max;
        if (v != sl->value) {
            sl->value = v;
            lu_push(LISPUI_EV_CHANGE, sl->id, 0, 0, 0, v);
        }
        return;
    }
    uint16_t id = 0;
    int16_t idx = 0;
    switch (ugui_nav(ctx, key, mods, &id, &idx)) {
    case UGUI_NAV_MOVED:
        lu_draw();
        return;
    case UGUI_NAV_ACTIVATE: {
        uint32_t seq = s_lu.seq;
        lu_act(id, idx, -1);
        if (s_lu.seq == seq) lu_draw();
        return;
    }
    case UGUI_NAV_BACK:
        lu_push(LISPUI_EV_KEY, 0, (int16_t)mods, 0, 0, 0x1b);
        return;
    case UGUI_NAV_EDGE: {
        lu_list_t *l = NULL;
        for (int i = 0; i < LU_LISTS && !l; i++) if (s_lu.lists[i].used && s_lu.lists[i].id == ctx->focus_id) l = &s_lu.lists[i];
        if (l && (key == KEY_UP || key == KEY_DOWN)) lu_scroll(l, key == KEY_DOWN ? l->row_h : -l->row_h);
        return;
    }
    default:
        lu_push(LISPUI_EV_KEY, 0, (int16_t)mods, 0, 0, (int32_t)key);
        return;
    }
}

/* --- The view ---------------------------------------------------------------- */

static bool lu_active(uint8_t inst) { return s_lu.active && inst == s_lu.slot; }

static void lu_event(uint8_t inst, const win_event_t *ev) {
    if (!lu_active(inst)) return;
    ugui_ctx_t *ctx = &s_lu.ui;
    switch (ev->type) {
    case WIN_EV_POINTER_DOWN:
        if (ev->button == 1 && (ev->flags & WIN_EVF_PRESS)) {
            s_lu.press_x = ev->x;
            s_lu.press_y = ev->y;
            bool changed = ugui_arm(ctx, ev->x, ev->y);
            lu_slider_t *sl = lu_slider_find(ctx->armed_id);
            if (sl) lu_slider_to(sl, ev->x);
            if (changed) lu_draw();
        }
        return;
    case WIN_EV_POINTER_UP: {
        int16_t idx = 0;
        if (ev->button == 2) {
            if (ev->flags & WIN_EVF_TAP) {
                uint16_t id = ugui_hit_test(ctx, ev->x, ev->y, &idx);
                lu_push(LISPUI_EV_CONTEXT, id, idx, ev->x, ev->y, 0);
            }
            return;
        }
        if (ev->flags & WIN_EVF_TAP) {
            bool was_armed = ugui_disarm(ctx);
            uint32_t seq = s_lu.seq;
            uint16_t id = ugui_hit_test(ctx, ev->x, ev->y, &idx);
            if (id) lu_act(id, idx, ev->x);
            else lu_push(LISPUI_EV_TAP, 0, 0, ev->x, ev->y, 0);
            if (was_armed && s_lu.active && s_lu.seq == seq) lu_draw();
        } else if (ev->flags & WIN_EVF_PRESS) {
            if (ugui_disarm(ctx)) lu_draw();
        }
        return;
    }
    case WIN_EV_POINTER_MOVE:
        if (ev->button == 0) {
            if (ugui_hover(ctx, ev->x, ev->y)) lu_draw();
        } else {
            lu_slider_t *sl = lu_slider_find(ctx->armed_id);
            if (sl) lu_slider_to(sl, ev->x);
        }
        return;
    case WIN_EV_DRAG:                                    /* the content follows the finger */
        if (ugui_disarm(ctx)) lu_draw();
        lu_scroll(lu_list_at(s_lu.press_x, s_lu.press_y), -ev->dy);
        return;
    case WIN_EV_SCROLL: {
        lu_list_t *l = lu_list_at(s_lu.press_x, s_lu.press_y);
        for (int i = 0; !l && i < LU_LISTS; i++) if (s_lu.lists[i].used) l = &s_lu.lists[i];
        if (!l) return;
        int notches = ev->dy < 0 ? -ev->dy : ev->dy;
        if (!(ev->flags & WIN_EVF_WHEEL)) notches = 3;
        if (notches > 4) notches = 4;
        lu_scroll(l, (ev->dy > 0 ? -1 : 1) * notches * l->row_h);
        return;
    }
    case WIN_EV_KEY_DOWN:
        lu_key(ev->key, ev->mods);
        return;
    default:
        return;
    }
}

static bool lu_captures(uint8_t inst, int x, int y) {
    /* accepted: read on the touch path while the views task may draw, as
     * the launcher's (a bounded array; a stale answer costs one contact). */
    return lu_active(inst) && ugui_drag_at(&s_lu.ui, x, y);
}

static void lu_resize(uint8_t inst, int w, int h) {
    if (!lu_active(inst)) return;
    if (w == s_lu.w && h == s_lu.h) return;
    s_lu.w = w;
    s_lu.h = h;
    lu_push(LISPUI_EV_RESIZE, 0, 0, w, h, 0);
    lu_draw();
}

static void lu_close(uint8_t inst) {
    if (!lu_active(inst)) return;
    s_lu.active = false;
    if (s_lu.osk_want) {
        s_lu.osk_want = false;
        console_osk_for_view(RIBBON_WIN_LISPUI, s_lu.slot, false);
    }
    lu_push(LISPUI_EV_CLOSE, 0, 0, 0, 0, 0);
}

static uint8_t *lu_surface(uint8_t inst, uint32_t *stride, int *w, int *h) {
    (void)inst; (void)stride; (void)w; (void)h;
    return NULL;                                         /* 1-bpp: the compositor converts it */
}

static uint32_t lu_seq(uint8_t inst) {
    (void)inst;
    return s_lu.seq;
}

const view_ops_t g_lispui_view = {
    .name = "lispui",
    .active = lu_active,
    .event = lu_event,
    .captures = lu_captures,
    .resize = lu_resize,
    .close = lu_close,
    .surface = lu_surface,
    .seq = lu_seq,
};

/* --- The Lisp task's side ------------------------------------------------------ */

typedef struct {
    int         op;
    const char *str;
    char       *out;
    uint32_t    cap;
    uint16_t    id;
    bool        ok;
    int         w, h;
} lu_call_t;

enum { LU_CALL_OPEN, LU_CALL_CLOSE, LU_CALL_COMMIT, LU_CALL_SIZE, LU_CALL_TEXT_GET, LU_CALL_TEXT_SET, LU_CALL_REPORT };

static void lu_report_now(lu_call_t *c);

static void lu_call(void *arg) {
    lu_call_t *c = arg;
    switch (c->op) {
    case LU_CALL_OPEN: {
        uint8_t req[3 + RIBBON_TITLE_MAX] = { 'v', RIBBON_WIN_LISPUI, SCREEN_VIEW_SINGLETON };
        uint32_t tl = 0;
        while (c->str[tl] && tl + 1u < RIBBON_TITLE_MAX) { req[3 + tl] = (uint8_t)c->str[tl]; tl++; }
        req[3 + tl] = '\0';
        uint8_t reply[SCREEN_REPLY_LEN];
        if (!console_canvas(req, 4u + tl, reply) || reply[0] != 0) { c->ok = false; return; }
        if (!s_lu.active || s_lu.slot != reply[1]) {     /* a new window: nothing kept */
            memset(&s_lu.ui, 0, sizeof s_lu.ui);
            memset(s_lu.text, 0, sizeof s_lu.text);
            memset(s_lu.lists, 0, sizeof s_lu.lists);
            s_lu.osk_want = false;
        }
        s_lu.active = true;
        s_lu.slot = reply[1];
        s_lu.w = (int)(reply[2] | (reply[3] << 8));
        s_lu.h = (int)(reply[4] | (reply[5] << 8));
        s_lu.len = 0;
        lu_draw();
        c->ok = true;
        return;
    }
    case LU_CALL_CLOSE:
        if (s_lu.active) {
            uint8_t slot = s_lu.slot;
            if (!console_close_view(RIBBON_WIN_LISPUI, slot)) lu_close(slot);   /* the last window: let go anyway */
        }
        return;
    case LU_CALL_COMMIT:
        memcpy(s_lu.list, s_build.buf, s_build.len);
        s_lu.len = s_build.len;
        lu_draw();
        c->ok = s_lu.active;
        return;
    case LU_CALL_SIZE:
        c->ok = s_lu.active;
        c->w = s_lu.w / LU_S;
        c->h = s_lu.h / LU_S;
        return;
    case LU_CALL_TEXT_GET: {
        lu_text_t *t = lu_text_find(c->id);
        uint32_t n = 0;
        if (t) while (t->buf[n] && n + 1u < c->cap) { c->out[n] = t->buf[n]; n++; }
        c->out[n] = '\0';
        return;
    }
    case LU_CALL_TEXT_SET: {
        lu_text_t *t = lu_text_make(c->id);
        if (!t) { c->ok = false; return; }
        uint32_t n = 0;
        while (c->str[n] && n + 1u < sizeof t->buf) { t->buf[n] = c->str[n]; n++; }
        t->buf[n] = '\0';
        t->t.len = (uint16_t)n;
        t->t.caret = t->t.anchor = (uint16_t)n;
        lu_draw();
        c->ok = true;
        return;
    }
    case LU_CALL_REPORT:
        lu_report_now(c);
        return;
    default:
        return;
    }
}

bool lispui_open(const char *title) {
    /* What the last window left unread is not this one's. */
    __atomic_store_n(&s_ring.tail, __atomic_load_n(&s_ring.head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    lu_call_t c = { .op = LU_CALL_OPEN, .str = (title && *title) ? title : "Lisp" };
    view_call(lu_call, &c);
    s_build.open = c.ok;
    s_build.len = 0;
    return c.ok;
}

void lispui_close(void) {
    lu_call_t c = { .op = LU_CALL_CLOSE };
    view_call(lu_call, &c);
    s_build.open = false;
}

bool lispui_is_open(void) {
    return s_build.open;
}

bool lispui_size(int *w, int *h) {
    lu_call_t c = { .op = LU_CALL_SIZE };
    view_call(lu_call, &c);
    if (w) *w = c.w;
    if (h) *h = c.h;
    return c.ok;
}

void lispui_begin(void) {
    s_build.len = 0;
    s_build.overflow = false;
}

static int16_t lu_clamp16(int32_t v) {
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

bool lispui_add(lispui_op_t op, const int32_t *nums, int nnums, const char *const *strs, int nstrs) {
    if (nnums < 0 || nnums > LISPUI_MAX_NUMS || nstrs < 0 || nstrs > LISPUI_MAX_STRS) return false;
    uint32_t need = 3u + 2u * (uint32_t)nnums;
    for (int i = 0; i < nstrs; i++) need += (uint32_t)strlen(strs[i] ? strs[i] : "") + 1u;
    if (s_build.len + need > sizeof s_build.buf) {
        s_build.overflow = true;
        return false;
    }
    uint8_t *b = s_build.buf + s_build.len;
    *b++ = (uint8_t)op;
    *b++ = (uint8_t)nnums;
    *b++ = (uint8_t)nstrs;
    for (int i = 0; i < nnums; i++) {
        int16_t v = lu_clamp16(nums[i]);
        *b++ = (uint8_t)v;
        *b++ = (uint8_t)((uint16_t)v >> 8);
    }
    for (int i = 0; i < nstrs; i++) {
        const char *s = strs[i] ? strs[i] : "";
        size_t l = strlen(s) + 1u;
        memcpy(b, s, l);
        b += l;
    }
    s_build.len += need;
    return true;
}

bool lispui_end(void) {
    if (!s_build.open) return false;
    lu_call_t c = { .op = LU_CALL_COMMIT };
    view_call(lu_call, &c);
    return c.ok && !s_build.overflow;
}

bool lispui_next_event(lispui_ev_t *ev) {
    uint32_t tail = s_ring.tail;
    if (tail == __atomic_load_n(&s_ring.head, __ATOMIC_ACQUIRE)) return false;
    *ev = s_ring.ev[tail];
    __atomic_store_n(&s_ring.tail, (tail + 1u) % LU_RING, __ATOMIC_RELEASE);
    if (ev->type == LISPUI_EV_CLOSE) s_build.open = false;
    return true;
}

void lispui_text_get(uint16_t id, char *out, uint32_t cap) {
    if (!out || cap == 0) return;
    lu_call_t c = { .op = LU_CALL_TEXT_GET, .id = id, .out = out, .cap = cap };
    view_call(lu_call, &c);
}

bool lispui_text_set(uint16_t id, const char *text) {
    lu_call_t c = { .op = LU_CALL_TEXT_SET, .id = id, .str = text ? text : "" };
    view_call(lu_call, &c);
    return c.ok;
}

/* On the views task, into c->out: the window, then each widget's rectangle
 * in screen pixels (where `ms click` and `ts tap` reach it), then the text
 * fields and lists. The caller prints it: what runs on the views task prints
 * to that task's console. */
static void lu_put(lu_call_t *c, uint32_t *n, const char *fmt, ...) {
    if (*n + 1u >= c->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = kvsnprintf(c->out + *n, c->cap - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n += (uint32_t)k;
}

static void lu_report_now(lu_call_t *c) {
    uint32_t n = 0;
    c->out[0] = '\0';
    if (!s_lu.active) {
        lu_put(c, &n, "ui: closed\n");
        return;
    }
    int x0 = 0, y0 = SCREEN_TILE_Y + SCREEN_TITLEBAR_H;
    uint8_t rq[2] = { 'R', 0 }, rp[SCREEN_REPLY_LEN];
    if (console_canvas(rq, 1, rp) && rp[0] == 0) {
        int count = rp[1], x_view = (int)(int16_t)(rp[3] | (rp[4] << 8));
        for (int i = 0; i < count; i++) {
            rq[1] = (uint8_t)i;
            uint8_t wr[SCREEN_REPLY_LEN];
            if (console_canvas(rq, 2, wr) && wr[0] == 0 && wr[1] == RIBBON_WIN_LISPUI && wr[2] == s_lu.slot) {
                x0 = (int)(wr[4] | (wr[5] << 8)) - x_view + 1;
                break;
            }
        }
    }
    const ugui_ctx_t *ctx = &s_lu.ui;
    lu_put(c, &n, "ui: open, slot %u, %d x %d pt (scale %d), at %d,%d; %d widgets, %u bytes; focus %u%s; armed %u; hover %u\n",
           (unsigned)s_lu.slot, s_lu.w / LU_S, s_lu.h / LU_S, LU_S, x0, y0, ctx->count, (unsigned)s_lu.len,
           (unsigned)ctx->focus_id, ctx->focus_shown ? " (shown)" : "", (unsigned)ctx->armed_id, (unsigned)ctx->hover_id);
    for (int i = 0; i < ctx->count; i++) {
        const ugui_hit_t *h = &ctx->hits[i];
        lu_put(c, &n, "  hit %u.%d at %d %d size %d %d%s\n", (unsigned)h->id, (int)h->index, x0 + h->rect.x,
               y0 + h->rect.y, h->rect.w, h->rect.h, (h->flags & UGUI_HIT_DISABLED) ? " disabled" : "");
    }
    for (int i = 0; i < LU_TEXTS; i++)
        if (s_lu.text[i].used) lu_put(c, &n, "  text %u \"%s\"\n", (unsigned)s_lu.text[i].id, s_lu.text[i].buf);
    for (int i = 0; i < LU_LISTS; i++)
        if (s_lu.lists[i].used)
            lu_put(c, &n, "  list %u scroll %d of %d rows\n", (unsigned)s_lu.lists[i].id, s_lu.lists[i].scroll,
                   s_lu.lists[i].count);
}

void lispui_report(void) {
    char *buf = (char *)palloc_pages(1);
    if (!buf) {
        cprintf("ui: no memory for the report\n");
        return;
    }
    lu_call_t c = { .op = LU_CALL_REPORT, .out = buf, .cap = PAGE_SIZE };
    view_call(lu_call, &c);
    cprintf("%s", buf);
    palloc_free(buf, 1);
}

#else   /* no views here */

#include "kernel/console.h"

bool lispui_open(const char *title) { (void)title; return false; }
void lispui_close(void) { }
bool lispui_is_open(void) { return false; }
bool lispui_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; return false; }
void lispui_begin(void) { }
bool lispui_add(lispui_op_t op, const int32_t *nums, int nnums, const char *const *strs, int nstrs) {
    (void)op; (void)nums; (void)nnums; (void)strs; (void)nstrs;
    return false;
}
bool lispui_end(void) { return false; }
bool lispui_next_event(lispui_ev_t *ev) { (void)ev; return false; }
void lispui_text_get(uint16_t id, char *out, uint32_t cap) { (void)id; if (out && cap) out[0] = '\0'; }
bool lispui_text_set(uint16_t id, const char *text) { (void)id; (void)text; return false; }
void lispui_report(void) { cprintf("ui: no views on this board\n"); }

#endif
