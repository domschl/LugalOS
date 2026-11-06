/* drivers/ugui.c — Stateless Classic-Macintosh Micro-Toolkit
 *
 * Immediate-mode drawing and hit-box geometry for Classic Mac surfaces.
 * Renders simultaneously to 1-bpp canvas1_t and optional 8-bpp GRAY8
 * anti-aliased framebuffers, scaling borders, drop shadows, icons, and
 * typography by s->scale.
 */

#include "drivers/ugui.h"
#include "kernel/keyseq.h"
#include "drivers/font_serif.h"
#include "drivers/font_noto_serif.h"
#include "drivers/font_noto_mono.h"
#include "drivers/font8x16.h"
#include "lugalos_config.h"
#include <string.h>

#if defined(CONFIG_BOARD_TAB5)
#define UGUI_HAS_NOTO_FONTS 1
#else
#define UGUI_HAS_NOTO_FONTS 0
#endif

void ugui_begin(ugui_ctx_t *ctx) {
    if (!ctx) return;
    ctx->count = 0;
    ctx->nbounds = 0;
    ctx->cur_bound = 0;
    ctx->modal = false;
    ctx->modal_from = 0;
}

/* The first hit that counts: a modal's own from where it began. */
static int live_from(const ugui_ctx_t *ctx) {
    return ctx->modal ? ctx->modal_from : 0;
}

/* --- Clipping ------------------------------------------------------------
 *
 * A clipped surface is drawn through a view of its clip rectangle: a 1-bpp
 * window on it (canvas1_window()), the GRAY8 buffer from its corner, the
 * coordinates moved by its origin. The primitives below then clip to the
 * view's bounds as they clip to a surface's. */

typedef struct {
    ugui_surface_t s;
    canvas1_t      win;
    int            dx, dy;
} ugui_view_t;

ugui_surface_t ugui_clip(const ugui_surface_t *s, ugui_rect_t r) {
    ugui_surface_t c = *s;
    int x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    int bx0 = s->clipped ? s->clip.x : 0, by0 = s->clipped ? s->clip.y : 0;
    int bx1 = s->clipped ? s->clip.x + s->clip.w : s->w, by1 = s->clipped ? s->clip.y + s->clip.h : s->h;
    if (x0 < bx0) x0 = bx0;
    if (y0 < by0) y0 = by0;
    if (x1 > bx1) x1 = bx1;
    if (y1 > by1) y1 = by1;
    c.clipped = true;
    c.clip = (ugui_rect_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 > x0 ? x1 - x0 : 0), (int16_t)(y1 > y0 ? y1 - y0 : 0) };
    return c;
}

/* The surface to draw on for s: s itself unclipped, else the view (NULL for
 * an empty clip: nothing to draw). */
static const ugui_surface_t *view(const ugui_surface_t *s, ugui_view_t *v) {
    v->dx = v->dy = 0;
    if (!s->clipped) return s;
    if (s->clip.w <= 0 || s->clip.h <= 0) return NULL;
    v->s = *s;
    v->s.clipped = false;
    v->dx = s->clip.x;
    v->dy = s->clip.y;
    if (s->c1) {
        canvas1_window(&v->win, s->c1, s->clip.x, s->clip.y, (unsigned)s->clip.w, (unsigned)s->clip.h);
        v->s.c1 = &v->win;
    }
    if (s->aa_fb) v->s.aa_fb = s->aa_fb + (uint32_t)s->clip.y * s->aa_stride + (uint32_t)s->clip.x;
    v->s.w = s->clip.w;
    v->s.h = s->clip.h;
    return &v->s;
}

static void raw_fill(const ugui_surface_t *s, ugui_rect_t r, uint8_t gray, unsigned pat1);
static int raw_text_serif(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                          bool bold, bool italic, bool inverted);
static int raw_text_mono(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                         bool bold, bool inverted);
static void raw_invert(const ugui_surface_t *s, int x0, int y0, int x1, int y1);
static void raw_dim(const ugui_surface_t *s, ugui_rect_t r);

void ugui_fill(const ugui_surface_t *s, ugui_rect_t r, uint8_t gray, unsigned pat1) {
    if (!s) return;
    ugui_view_t v;
    const ugui_surface_t *t = view(s, &v);
    if (!t) return;
    r.x = (int16_t)(r.x - v.dx);
    r.y = (int16_t)(r.y - v.dy);
    raw_fill(t, r, gray, pat1);
}

int ugui_text_serif(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                    bool bold, bool italic, bool inverted) {
    if (!s) return 0;
    ugui_view_t v;
    const ugui_surface_t *t = view(s, &v);
    if (!t) return ugui_text_serif_width(s, str, len);
    return raw_text_serif(t, x - v.dx, y - v.dy, str, len, bold, italic, inverted);
}

int ugui_text_mono(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                   bool bold, bool inverted) {
    if (!s) return 0;
    ugui_view_t v;
    const ugui_surface_t *t = view(s, &v);
    if (!t) return (int)len * ugui_mono_cell_w(s);
    return raw_text_mono(t, x - v.dx, y - v.dy, str, len, bold, inverted);
}

static void ugui_invert(const ugui_surface_t *s, int x0, int y0, int x1, int y1) {
    ugui_view_t v;
    const ugui_surface_t *t = view(s, &v);
    if (t) raw_invert(t, x0 - v.dx, y0 - v.dy, x1 - v.dx, y1 - v.dy);
}

static void ugui_dim(const ugui_surface_t *s, ugui_rect_t r) {
    ugui_view_t v;
    const ugui_surface_t *t = view(s, &v);
    if (!t) return;
    r.x = (int16_t)(r.x - v.dx);
    r.y = (int16_t)(r.y - v.dy);
    raw_dim(t, r);
}

int ugui_target_px(void) {
#if defined(CONFIG_PANEL_DPI)
    return (int)((UGUI_TARGET_MM * (unsigned)CONFIG_PANEL_DPI * 10u + 127u) / 254u);
#else
    return 0;
#endif
}

void ugui_set_min_target(ugui_ctx_t *ctx, int min_px) {
    if (ctx) ctx->min_target = (int16_t)(min_px > 0 ? min_px : 0);
}

void ugui_slop_bounds(ugui_ctx_t *ctx, ugui_rect_t r) {
    if (!ctx || ctx->nbounds >= UGUI_MAX_BOUNDS) return;
    ctx->bounds[ctx->nbounds++] = r;
    ctx->cur_bound = ctx->nbounds;
}

void ugui_slop_bounds_end(ugui_ctx_t *ctx) {
    if (ctx) ctx->cur_bound = 0;
}

static void grow_axis(int16_t *pos, int16_t *len, int min, int lo, int hi) {
    if (*len < min) {
        int p = *pos - (min - *len) / 2;
        *pos = (int16_t)p;
        *len = (int16_t)min;
    }
    if (hi > lo) {                                       /* bounded: clip */
        int a = *pos < lo ? lo : *pos, b = *pos + *len > hi ? hi : *pos + *len;
        *pos = (int16_t)a;
        *len = (int16_t)(b > a ? b - a : 0);
    }
}

ugui_rect_t ugui_touch_rect(const ugui_ctx_t *ctx, int i) {
    ugui_rect_t r = ctx->hits[i].rect;
    if (ctx->min_target <= 0) return r;
    const ugui_rect_t *b = ctx->hits[i].bound ? &ctx->bounds[ctx->hits[i].bound - 1] : NULL;
    grow_axis(&r.x, &r.w, ctx->min_target, b ? b->x : 0, b ? b->x + b->w : 0);
    grow_axis(&r.y, &r.h, ctx->min_target, b ? b->y : 0, b ? b->y + b->h : 0);
    return r;
}

static bool in_rect(const ugui_rect_t *r, int x, int y) {
    return x >= r->x && x < (int)r->x + (int)r->w && y >= r->y && y < (int)r->y + (int)r->h;
}

/* Squared distance from (x, y) to r (0 inside). */
static long rect_dist2(const ugui_rect_t *r, int x, int y) {
    long dx = x < r->x ? r->x - x : x >= r->x + r->w ? x - (r->x + r->w - 1) : 0;
    long dy = y < r->y ? r->y - y : y >= r->y + r->h ? y - (r->y + r->h - 1) : 0;
    return dx * dx + dy * dy;
}

static void add_hit_flags(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id, int16_t index, uint8_t flags) {
    if (!ctx || id == 0 || ctx->count >= UGUI_MAX_HITBOXES) return;
    if (r.w <= 0 || r.h <= 0) return;
    ugui_hit_t *h = &ctx->hits[ctx->count++];
    h->rect = r;
    h->id = id;
    h->index = index;
    h->flags = flags;
    h->bound = ctx->cur_bound;
}

void ugui_add_hit(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id, int16_t index) {
    add_hit_flags(ctx, r, id, index, 0);
}

/* A widget's hit-box on surface s: cut to s's clip (gone when nothing of it
 * shows), and its touch area kept inside the clip unless bounds are set. */
static void add_hit_s(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id,
                      int16_t index, uint8_t flags) {
    if (!ctx) return;
    if (!s || !s->clipped) { add_hit_flags(ctx, r, id, index, flags); return; }
    int x0 = r.x > s->clip.x ? r.x : s->clip.x, y0 = r.y > s->clip.y ? r.y : s->clip.y;
    int x1 = r.x + r.w < s->clip.x + s->clip.w ? r.x + r.w : s->clip.x + s->clip.w;
    int y1 = r.y + r.h < s->clip.y + s->clip.h ? r.y + r.h : s->clip.y + s->clip.h;
    if (x1 <= x0 || y1 <= y0) return;
    uint8_t keep = ctx->cur_bound;
    if (!keep) {
        const ugui_rect_t *last = ctx->nbounds ? &ctx->bounds[ctx->nbounds - 1] : NULL;
        if (last && last->x == s->clip.x && last->y == s->clip.y && last->w == s->clip.w && last->h == s->clip.h)
            ctx->cur_bound = ctx->nbounds;
        else
            ugui_slop_bounds(ctx, s->clip);
    }
    add_hit_flags(ctx, (ugui_rect_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) }, id, index, flags);
    ctx->cur_bound = keep;
}

void ugui_disable_next(ugui_ctx_t *ctx, uint8_t parts) {
    if (ctx) ctx->next_disabled = parts;
}

/* The next widget's dimmed parts, taken: the following one starts enabled. */
static uint8_t take_disabled(ugui_ctx_t *ctx) {
    if (!ctx) return 0;
    uint8_t m = ctx->next_disabled;
    ctx->next_disabled = 0;
    return m;
}

/* Dims r as drawn: GRAY8 two thirds of the way to the card's white, 1-bpp
 * the Classic Mac way, every other ink pixel cleared in a checkerboard. */
static void raw_dim(const ugui_surface_t *s, ugui_rect_t r) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    if (x0 > x1 || y0 > y1) return;
    if (s->aa_fb)
        for (int y = y0; y <= y1; y++) {
            uint8_t *p = s->aa_fb + (uint32_t)y * s->aa_stride;
            for (int x = x0; x <= x1; x++) p[x] = (uint8_t)((p[x] + 2u * UGUI_GRAY_CARD) / 3u);
        }
    if (s->c1)
        for (int y = y0; y <= y1; y++)
            for (int x = x0 + ((x0 + y) & 1); x <= x1; x += 2)
                if (canvas1_get(s->c1, x, y)) canvas1_fill(s->c1, x, y, x, y, CANVAS1_WHITE);
}

bool ugui_arm(ugui_ctx_t *ctx, int x, int y) {
    if (!ctx) return false;
    bool was_shown = ctx->focus_shown;
    ctx->focus_shown = false;                            /* touch: no focus ring */
    int16_t index = 0;
    uint16_t id = ugui_hit_test(ctx, x, y, &index);
    if (id == ctx->armed_id && (id == 0 || index == ctx->armed_index)) return was_shown;
    ctx->armed_id = id;
    ctx->armed_index = index;
    return true;
}

bool ugui_disarm(ugui_ctx_t *ctx) {
    if (!ctx || ctx->armed_id == 0) return false;
    ctx->armed_id = 0;
    ctx->armed_index = 0;
    return true;
}

bool ugui_is_armed(const ugui_ctx_t *ctx, uint16_t id, int16_t index) {
    return ctx && id != 0 && ctx->armed_id == id && ctx->armed_index == index;
}

static int focus_slot(const ugui_ctx_t *ctx) {
    for (int i = 0; i < ctx->count; i++)
        if (ctx->hits[i].id == ctx->focus_id && ctx->hits[i].index == ctx->focus_index) return i;
    return -1;
}

static bool focusable(const ugui_ctx_t *ctx, int slot) {
    return slot >= live_from(ctx) && !(ctx->hits[slot].flags & UGUI_HIT_DISABLED);
}

/* The first focusable slot from `from` stepping by step (wrapping), or -1. */
static int focus_step(const ugui_ctx_t *ctx, int from, int step) {
    for (int n = 1; n <= ctx->count; n++) {
        int i = ((from + step * n) % ctx->count + ctx->count) % ctx->count;
        if (focusable(ctx, i)) return i;
    }
    return -1;
}

static void focus_to(ugui_ctx_t *ctx, int slot) {
    ctx->focus_id = ctx->hits[slot].id;
    ctx->focus_index = ctx->hits[slot].index;
}

/* The nearest widget from slot `from` in direction (dx, dy): its centre must
 * lie that way; the distance along the direction counts once, the distance
 * across it twice, so a widget straight ahead beats a nearer one aside. */
static int focus_nearest(const ugui_ctx_t *ctx, int from, int dx, int dy) {
    const ugui_rect_t *f = &ctx->hits[from].rect;
    int fx = f->x + f->w / 2, fy = f->y + f->h / 2;
    int best = -1;
    long best_d = 0;
    for (int i = 0; i < ctx->count; i++) {
        if (i == from || !focusable(ctx, i)) continue;
        const ugui_rect_t *r = &ctx->hits[i].rect;
        int along, across;
        if (dx) {
            int edge = dx > 0 ? r->x - (f->x + f->w - 1) : f->x - (r->x + r->w - 1);
            if (edge <= 0) continue;                     /* not that way */
            along = edge;
            across = (r->y + r->h / 2) - fy;
        } else {
            int edge = dy > 0 ? r->y - (f->y + f->h - 1) : f->y - (r->y + r->h - 1);
            if (edge <= 0) continue;
            along = edge;
            across = (r->x + r->w / 2) - fx;
        }
        if (across < 0) across = -across;
        long d = (long)along + 2L * (long)across;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

ugui_nav_t ugui_nav(ugui_ctx_t *ctx, uint32_t key, uint8_t mods, uint16_t *id, int16_t *index) {
    if (!ctx) return UGUI_NAV_NONE;
    bool tab = key == '\t';
    bool arrow = key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT;
    bool activate = key == '\r' || key == '\n' || key == ' ';
    if (key == 0x1b) return UGUI_NAV_BACK;
    if (!tab && !arrow && !activate) return UGUI_NAV_NONE;
    if (ctx->count == 0) return arrow ? UGUI_NAV_EDGE : UGUI_NAV_NONE;
    int slot = focus_slot(ctx);
    if (slot >= 0 && slot < live_from(ctx)) slot = -1;   /* behind a modal */
    if (slot < 0 || !ctx->focus_shown) {                 /* first key: only show where it is */
        if (slot < 0 || !focusable(ctx, slot)) {
            int first = focus_step(ctx, -1, 1);
            if (first < 0) return arrow ? UGUI_NAV_EDGE : UGUI_NAV_NONE;
            focus_to(ctx, first);
        }
        ctx->focus_shown = true;
        return UGUI_NAV_MOVED;
    }
    if (activate) {
        if (!focusable(ctx, slot)) return UGUI_NAV_NONE;   /* dimmed since: nothing to do */
        if (id) *id = ctx->hits[slot].id;
        if (index) *index = ctx->hits[slot].index;
        return UGUI_NAV_ACTIVATE;
    }
    if (tab) {
        int to = focus_step(ctx, slot, (mods & KMOD_SHIFT) ? -1 : 1);
        if (to >= 0) focus_to(ctx, to);
        return UGUI_NAV_MOVED;
    }
    int to = focus_nearest(ctx, slot, key == KEY_RIGHT ? 1 : key == KEY_LEFT ? -1 : 0,
                           key == KEY_DOWN ? 1 : key == KEY_UP ? -1 : 0);
    if (to < 0) return UGUI_NAV_EDGE;
    focus_to(ctx, to);
    return UGUI_NAV_MOVED;
}

bool ugui_is_focused(const ugui_ctx_t *ctx, uint16_t id, int16_t index) {
    return ctx && ctx->focus_shown && id != 0 && ctx->focus_id == id && ctx->focus_index == index;
}

/* Inverts r on both surfaces. */
static void raw_invert(const ugui_surface_t *s, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= s->w) x1 = s->w - 1;
    if (y1 >= s->h) y1 = s->h - 1;
    if (x0 > x1 || y0 > y1) return;
    if (s->c1) canvas1_invert(s->c1, x0, y0, x1, y1);
    if (s->aa_fb)
        for (int y = y0; y <= y1; y++) {
            uint8_t *p = s->aa_fb + (uint32_t)y * s->aa_stride;
            for (int x = x0; x <= x1; x++) p[x] = (uint8_t)(255u - p[x]);
        }
}

void ugui_end(const ugui_surface_t *s, ugui_ctx_t *ctx) {
    if (!ctx || ctx->count == 0) return;
    int slot = focus_slot(ctx);
    if (slot >= 0 && slot < live_from(ctx)) slot = -1;   /* behind a modal: into it */
    if (slot < 0) {
        if (ctx->focus_id == 0 && !ctx->focus_shown && !ctx->modal) return;
        slot = focus_step(ctx, -1, 1);
        if (slot < 0) return;                            /* every widget dimmed */
        focus_to(ctx, slot);
    }
    if (!s || !ctx->focus_shown) return;
    /* the dotted ring: dashes of 2 units, `t` thick, 2 units inside */
    int sc = s->scale > 0 ? s->scale : 1;
    int t = sc, dash = 2 * sc;
    const ugui_rect_t *r = &ctx->hits[slot].rect;
    int x0 = r->x + 2 * sc, y0 = r->y + 2 * sc;
    int x1 = r->x + r->w - 1 - 2 * sc, y1 = r->y + r->h - 1 - 2 * sc;
    if (x1 - x0 < 2 * t || y1 - y0 < 2 * t) return;
    for (int x = x0; x <= x1; x += 2 * dash) {
        int xe = x + dash - 1 > x1 ? x1 : x + dash - 1;
        ugui_invert(s, x, y0, xe, y0 + t - 1);
        ugui_invert(s, x, y1 - t + 1, xe, y1);
    }
    for (int y = y0 + t; y <= y1 - t; y += 2 * dash) {
        int ye = y + dash - 1 > y1 - t ? y1 - t : y + dash - 1;
        ugui_invert(s, x0, y, x0 + t - 1, ye);
        ugui_invert(s, x1 - t + 1, y, x1, ye);
    }
}

bool ugui_drag_at(const ugui_ctx_t *ctx, int x, int y) {
    int16_t idx = 0;
    uint16_t id = ugui_hit_test(ctx, x, y, &idx);
    if (!id) return false;
    for (int i = live_from(ctx); i < ctx->count; i++)
        if (ctx->hits[i].id == id && ctx->hits[i].index == idx) return (ctx->hits[i].flags & UGUI_HIT_DRAG) != 0;
    return false;
}

uint16_t ugui_hit_test_drawn(const ugui_ctx_t *ctx, int x, int y, int16_t *out_index) {
    if (!ctx) return 0;
    for (int i = ctx->count - 1; i >= live_from(ctx); i--) {
        const ugui_hit_t *h = &ctx->hits[i];
        if (!in_rect(&h->rect, x, y)) continue;
        if (h->flags & UGUI_HIT_DISABLED) return 0;
        if (out_index) *out_index = h->index;
        return h->id;
    }
    return 0;
}

uint16_t ugui_hit_test(const ugui_ctx_t *ctx, int x, int y, int16_t *out_index) {
    if (!ctx) return 0;
    /* As drawn first, in reverse order so later (topmost) widgets take
     * precedence; then the grown touch areas, the nearest drawn widget's. */
    int best = -1;
    for (int i = ctx->count - 1; i >= live_from(ctx) && best < 0; i--)
        if (in_rect(&ctx->hits[i].rect, x, y)) best = i;
    if (best < 0 && ctx->min_target > 0) {
        long best_d = 0;
        for (int i = ctx->count - 1; i >= live_from(ctx); i--) {
            ugui_rect_t t = ugui_touch_rect(ctx, i);
            if (!in_rect(&t, x, y)) continue;
            long d = rect_dist2(&ctx->hits[i].rect, x, y);
            if (best < 0 || d < best_d) { best = i; best_d = d; }
        }
    }
    if (best < 0) return 0;
    const ugui_hit_t *h = &ctx->hits[best];
    if (h->flags & UGUI_HIT_DISABLED) return 0;          /* dimmed: swallowed */
    if (out_index) *out_index = h->index;
    return h->id;
}

static void raw_fill(const ugui_surface_t *s, ugui_rect_t r, uint8_t gray, unsigned pat1) {
    if (!s || r.w <= 0 || r.h <= 0) return;
    int x0 = r.x;
    int y0 = r.y;
    int x1 = (int)r.x + (int)r.w - 1;
    int y1 = (int)r.y + (int)r.h - 1;

    if (s->c1) {
        canvas1_fill(s->c1, x0, y0, x1, y1, pat1);
    }
    if (s->aa_fb) {
        int cx0 = x0 < 0 ? 0 : x0;
        int cy0 = y0 < 0 ? 0 : y0;
        int cx1 = x1 >= s->w ? s->w - 1 : x1;
        int cy1 = y1 >= s->h ? s->h - 1 : y1;
        if (cx0 <= cx1 && cy0 <= cy1) {
            size_t span = (size_t)(cx1 - cx0 + 1);
            for (int y = cy0; y <= cy1; y++) {
                memset(s->aa_fb + (uint32_t)y * s->aa_stride + cx0, gray, span);
            }
        }
    }
}

void ugui_hline(const ugui_surface_t *s, int x0, int x1, int y, int thick, uint8_t gray, unsigned pat1) {
    if (!s || thick <= 0 || x1 < x0) return;
    ugui_rect_t r = {
        .x = (int16_t)x0,
        .y = (int16_t)y,
        .w = (int16_t)(x1 - x0 + 1),
        .h = (int16_t)thick,
    };
    ugui_fill(s, r, gray, pat1);
}

void ugui_vline(const ugui_surface_t *s, int x, int y0, int y1, int thick, uint8_t gray, unsigned pat1) {
    if (!s || thick <= 0 || y1 < y0) return;
    ugui_rect_t r = {
        .x = (int16_t)x,
        .y = (int16_t)y0,
        .w = (int16_t)thick,
        .h = (int16_t)(y1 - y0 + 1),
    };
    ugui_fill(s, r, gray, pat1);
}

void ugui_box_shadow(const ugui_surface_t *s, ugui_rect_t r, bool inverted) {
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    int shadow = 2 * sc;
    int border = sc;

    /* Bottom-right drop shadow */
    if (r.w > shadow && r.h > shadow) {
        ugui_rect_t sh_b = {
            .x = (int16_t)(r.x + shadow),
            .y = (int16_t)(r.y + r.h),
            .w = r.w,
            .h = (int16_t)shadow,
        };
        ugui_rect_t sh_r = {
            .x = (int16_t)(r.x + r.w),
            .y = (int16_t)(r.y + shadow),
            .w = (int16_t)shadow,
            .h = r.h,
        };
        ugui_fill(s, sh_b, UGUI_GRAY_SHADOW, CANVAS1_BLACK);
        ugui_fill(s, sh_r, UGUI_GRAY_SHADOW, CANVAS1_BLACK);
    }

    /* Interior fill */
    uint8_t bg_gray = inverted ? UGUI_GRAY_INK : UGUI_GRAY_CARD;
    unsigned bg_pat = inverted ? CANVAS1_BLACK : CANVAS1_WHITE;
    ugui_fill(s, r, bg_gray, bg_pat);

    /* Outer border */
    int x0 = r.x;
    int y0 = r.y;
    int x1 = (int)r.x + (int)r.w - 1;
    int y1 = (int)r.y + (int)r.h - 1;
    ugui_hline(s, x0, x1, y0, border, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_hline(s, x0, x1, y1 - border + 1, border, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_vline(s, x0, y0, y1, border, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_vline(s, x1 - border + 1, y0, y1, border, UGUI_GRAY_BORDER, CANVAS1_BLACK);
}

int ugui_serif_line_h(const ugui_surface_t *s) {
    int sc = (s && s->scale > 0) ? s->scale : 1;
    return (sc >= 2) ? 32 : 16;
}

int ugui_mono_cell_w(const ugui_surface_t *s) {
    int sc = (s && s->scale > 0) ? s->scale : 1;
    return 8 * sc;
}

int ugui_mono_cell_h(const ugui_surface_t *s) {
    int sc = (s && s->scale > 0) ? s->scale : 1;
    return 16 * sc;
}

int ugui_text_serif_width(const ugui_surface_t *s, const char *str, uint32_t len) {
    if (!str || len == 0) return 0;
    int sc = (s && s->scale > 0) ? s->scale : 1;
    if (sc >= 2) {
#if UGUI_HAS_NOTO_FONTS
        return (int)font_noto_serif_text_width(str, len);
#else
        return sc * (int)font_serif_text_width(str, len);
#endif
    }
    return (int)font_serif_text_width(str, len);
}

static int raw_text_serif(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                          bool bold, bool italic, bool inverted) {
    if (!s || !str || len == 0) return 0;
    int sc = s->scale > 0 ? s->scale : 1;
    uint8_t fg = inverted ? UGUI_GRAY_CARD : UGUI_GRAY_INK;
    uint8_t bg = inverted ? UGUI_GRAY_INK : UGUI_GRAY_PAPER;
    int w = 0;

#if UGUI_HAS_NOTO_FONTS
    if (sc >= 2) {
        if (s->c1) {
            w = canvas1_noto_serif_text(s->c1, x, y, str, len, bold, italic);
            if (inverted && w > 0) {
                /* Re-draw via temporary clear/invert so white-on-black text is legible in 1-bpp */
                canvas1_fill(s->c1, x, y, x + w - 1, y + 31, CANVAS1_WHITE);
                canvas1_noto_serif_text(s->c1, x, y, str, len, bold, italic);
                canvas1_invert(s->c1, x, y, x + w - 1, y + 31);
            }
        }
        if (s->aa_fb) {
            int aw = font_noto_serif_aa_text_gray8(s->aa_fb, s->aa_stride, s->w, s->h,
                                                   x, y, str, len, fg, bg, bold, italic);
            if (!s->c1) w = aw;
        }
        return w;
    }
#else
    (void)sc;
#endif
    if (s->c1) {
        w = canvas1_serif_text(s->c1, x, y, str, len, bold, italic);
        if (inverted && w > 0) {
            canvas1_fill(s->c1, x, y, x + w - 1, y + 15, CANVAS1_WHITE);
            canvas1_serif_text(s->c1, x, y, str, len, bold, italic);
            canvas1_invert(s->c1, x, y, x + w - 1, y + 15);
        }
    }
    if (s->aa_fb) {
        int aw = font_serif_aa_text_gray8(s->aa_fb, s->aa_stride, s->w, s->h,
                                          x, y, str, len, fg, bg, bold, italic);
        if (!s->c1) w = aw;
    }
    return w;
}

static int raw_text_mono(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                         bool bold, bool inverted) {
    if (!s || !str || len == 0) return 0;
    int sc = s->scale > 0 ? s->scale : 1;
    uint8_t fg = inverted ? UGUI_GRAY_CARD : UGUI_GRAY_INK;
#if UGUI_HAS_NOTO_FONTS
    uint8_t bg = inverted ? UGUI_GRAY_INK : UGUI_GRAY_PAPER;

    if (sc >= 2) {
        int cw = 16;
        int ch = 32;
        if (s->c1) {
            int total_w = (int)len * cw;
            if (inverted && total_w > 0) {
                canvas1_fill(s->c1, x, y, x + total_w - 1, y + ch - 1, CANVAS1_WHITE);
            }
            for (uint32_t i = 0; i < len && str[i]; i++) {
                uint8_t code = (uint8_t)str[i];
                if (code < FONT_NOTO_MONO_FIRST) code = FONT_NOTO_MONO_REPLACEMENT;
                const uint16_t *glyph = font_noto_mono_get_glyph(code);
                int cx = x + (int)i * cw;
                for (int r = 0; r < ch; r++) {
                    int py = y + r;
                    if (py < 0 || py >= (int)s->c1->h) continue;
                    uint16_t bits = glyph[r];
                    if (bold) bits |= (uint16_t)(bits << 1);
                    for (int b = 0; b < cw; b++) {
                        int px = cx + b;
                        if (px >= 0 && px < (int)s->c1->w && ((bits >> b) & 1u)) {
                            canvas1_fill(s->c1, px, py, px, py, CANVAS1_BLACK);
                        }
                    }
                }
            }
            if (inverted && total_w > 0) {
                canvas1_invert(s->c1, x, y, x + total_w - 1, y + ch - 1);
            }
        }
        if (s->aa_fb) {
            int cx = x;
            for (uint32_t i = 0; i < len && str[i]; i++) {
                font_noto_mono_aa_glyph_gray8(s->aa_fb, s->aa_stride, s->w, s->h,
                                              cx, y, (uint8_t)str[i], fg, bg);
                cx += cw;
            }
        }
        return (int)len * cw;
    }
#endif
    int cw = 8 * sc;
    int ch = 16 * sc;
    if (s->c1) {
        int total_w = (int)len * cw;
        if (inverted && total_w > 0) {
            canvas1_fill(s->c1, x, y, x + total_w - 1, y + ch - 1, CANVAS1_WHITE);
        }
        canvas1_text(s->c1, x, y, str, len, len, 0, 16, bold);
        if (inverted && total_w > 0) {
            canvas1_invert(s->c1, x, y, x + total_w - 1, y + ch - 1);
        }
    }
    if (s->aa_fb) {
        int cx = x;
        for (uint32_t i = 0; i < len && str[i]; i++) {
            uint8_t code = (uint8_t)str[i];
            if (code < FONT8X16_FIRST) code = FONT8X16_REPLACEMENT;
            const uint8_t *glyph = font8x16_glyphs[code - FONT8X16_FIRST];
            for (int r = 0; r < 16; r++) {
                int py = y + r;
                if (py < 0 || py >= s->h) continue;
                uint8_t *dst = s->aa_fb + (uint32_t)py * s->aa_stride;
                uint8_t bits = glyph[r];
                if (bold) bits |= (uint8_t)(bits << 1);
                for (int b = 0; b < 8; b++) {
                    int px = cx + b;
                    if (px >= 0 && px < s->w && ((bits >> b) & 1u)) {
                        dst[px] = fg;
                    }
                }
            }
            cx += 8;
        }
    }
    return (int)len * cw;
}

void ugui_icon16(const ugui_surface_t *s, int x, int y, const uint16_t icon[16],
                 int extra_scale, bool inverted) {
    if (!s || !icon) return;
    int sc = (s->scale > 0 ? s->scale : 1) * (extra_scale > 0 ? extra_scale : 1);
    uint8_t fg_gray = inverted ? UGUI_GRAY_CARD : UGUI_GRAY_INK;
    unsigned fg_pat = inverted ? CANVAS1_WHITE : CANVAS1_BLACK;

    for (int row = 0; row < 16; row++) {
        uint16_t bits = icon[row];
        if (!bits) continue;
        for (int col = 0; col < 16; col++) {
            if ((bits >> col) & 1u) {
                ugui_rect_t px = {
                    .x = (int16_t)(x + col * sc),
                    .y = (int16_t)(y + row * sc),
                    .w = (int16_t)sc,
                    .h = (int16_t)sc,
                };
                ugui_fill(s, px, fg_gray, fg_pat);
            }
        }
    }
}

void ugui_button(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                 const char *label, uint16_t id, bool active) {
    uint8_t dis = take_disabled(ctx);
    active = !dis && (active || ugui_is_armed(ctx, id, 0));
    if (!s || r.w <= 0 || r.h <= 0) return;
    ugui_box_shadow(s, r, active);
    if (label && *label) {
        uint32_t len = (uint32_t)strlen(label);
        int tw = ugui_text_serif_width(s, label, len);
        int lh = ugui_serif_line_h(s);
        int tx = r.x + (r.w - tw) / 2;
        int ty = r.y + (r.h - lh) / 2;
        if (tx < r.x + 2) tx = r.x + 2;
        ugui_text_serif(s, tx, ty, label, len, true, false, active);
    }
    if (dis) ugui_dim(s, (ugui_rect_t){ r.x, r.y, (int16_t)(r.w + 3 * s->scale), (int16_t)(r.h + 3 * s->scale) });
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : 0);
}

void ugui_toolbar_button(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                         const char *label, uint16_t id, bool active) {
    uint8_t dis = take_disabled(ctx);
    active = !dis && (active || ugui_is_armed(ctx, id, 0));
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;

    uint8_t bg_gray = active ? UGUI_GRAY_INK : UGUI_GRAY_CARD;
    unsigned bg_pat = active ? CANVAS1_BLACK : CANVAS1_WHITE;
    ugui_fill(s, r, bg_gray, bg_pat);

    int x0 = r.x;
    int y0 = r.y;
    int x1 = (int)r.x + (int)r.w - 1;
    int y1 = (int)r.y + (int)r.h - 1;
    ugui_hline(s, x0, x1, y0, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_hline(s, x0, x1, y1 - sc + 1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_vline(s, x0, y0, y1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_vline(s, x1 - sc + 1, y0, y1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);

    /* Subtle 1*sc bottom-right shadow; a dimmed button is flat */
    if (!dis) {
        ugui_hline(s, x0 + sc, x1 + sc, y1 + 1, sc, UGUI_GRAY_SHADOW, CANVAS1_BLACK);
        ugui_vline(s, x1 + 1, y0 + sc, y1 + sc, sc, UGUI_GRAY_SHADOW, CANVAS1_BLACK);
    }

    if (label && *label) {
        uint32_t len = (uint32_t)strlen(label);
        int tw = ugui_text_serif_width(s, label, len);
        int lh = ugui_serif_line_h(s);
        int tx = r.x + (r.w - tw) / 2;
        int ty = r.y + (r.h - lh) / 2;
        if (tx < r.x + 2 * sc) tx = r.x + 2 * sc;
        ugui_text_serif(s, tx, ty, label, len, true, false, active);
    }
    if (dis) ugui_dim(s, r);
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : 0);
}

void ugui_card(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
               const uint16_t icon16[16], const char *title, const char *subtitle,
               uint16_t id, bool active) {
    uint8_t dis = take_disabled(ctx);
    active = !dis && (active || ugui_is_armed(ctx, id, 0));
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;

    ugui_box_shadow(s, r, active);

    int pad = 8 * sc;
    int cur_y = r.y + pad;

    if (icon16) {
        int icon_sz = 16 * sc;
        int ix = r.x + (r.w - icon_sz) / 2;
        ugui_icon16(s, ix, cur_y, icon16, 1, active);
        cur_y += icon_sz + 4 * sc;
    }

    int lh = ugui_serif_line_h(s);
    if (title && *title) {
        uint32_t tlen = (uint32_t)strlen(title);
        int tw = ugui_text_serif_width(s, title, tlen);
        int tx = r.x + (r.w - tw) / 2;
        if (tx < r.x + 4 * sc) tx = r.x + 4 * sc;
        ugui_text_serif(s, tx, cur_y, title, tlen, true, false, active);
        cur_y += lh;
    }

    if (subtitle && *subtitle && cur_y + lh <= r.y + r.h - 2 * sc) {
        uint32_t slen = (uint32_t)strlen(subtitle);
        int sw = ugui_text_serif_width(s, subtitle, slen);
        int sx = r.x + (r.w - sw) / 2;
        if (sx < r.x + 4 * sc) sx = r.x + 4 * sc;
        ugui_text_serif(s, sx, cur_y, subtitle, slen, false, true, active);
    }

    if (dis) ugui_dim(s, (ugui_rect_t){ r.x, r.y, (int16_t)(r.w + 3 * sc), (int16_t)(r.h + 3 * sc) });
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : 0);
}

void ugui_section_header(const ugui_surface_t *s, int x, int y, int w, const char *title) {
    if (!s || !title || w <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    int lh = ugui_serif_line_h(s);
    uint32_t len = (uint32_t)strlen(title);
    int tw = ugui_text_serif(s, x, y, title, len, true, false, false);
    int rule_x0 = x + tw + 8 * sc;
    int rule_x1 = x + w - 1;
    int rule_y = y + lh / 2;
    if (rule_x0 < rule_x1) {
        ugui_hline(s, rule_x0, rule_x1, rule_y, sc, UGUI_GRAY_MUTED, CANVAS1_BLACK);
    }
}

void ugui_stepper(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                  const char *label, const char *value_str,
                  uint16_t dec_id, uint16_t inc_id) {
    if (!s || r.w <= 0 || r.h <= 0) { (void)take_disabled(ctx); return; }
    int sc = s->scale > 0 ? s->scale : 1;
    int lh = ugui_serif_line_h(s);
    int ty = r.y + (r.h - lh) / 2;

    if (label && *label) {
        ugui_text_serif(s, r.x, ty, label, (uint32_t)strlen(label), true, false, false);
    }

    int btn_w = 32 * sc;
    int val_w = 56 * sc;
    int right_x = (int)r.x + (int)r.w;

    ugui_rect_t inc_r = {
        .x = (int16_t)(right_x - btn_w),
        .y = r.y,
        .w = (int16_t)btn_w,
        .h = r.h,
    };
    ugui_rect_t val_r = {
        .x = (int16_t)(inc_r.x - val_w - 4 * sc),
        .y = r.y,
        .w = (int16_t)val_w,
        .h = r.h,
    };
    ugui_rect_t dec_r = {
        .x = (int16_t)(val_r.x - btn_w - 4 * sc),
        .y = r.y,
        .w = (int16_t)btn_w,
        .h = r.h,
    };

    uint8_t dis = take_disabled(ctx);
    ugui_disable_next(ctx, dis & 1u);
    ugui_toolbar_button(s, ctx, dec_r, "-", dec_id, false);

    /* Value readout pill */
    ugui_fill(s, val_r, UGUI_GRAY_CARD, CANVAS1_WHITE);
    ugui_hline(s, val_r.x, val_r.x + val_r.w - 1, val_r.y, sc, UGUI_GRAY_MUTED, CANVAS1_BLACK);
    ugui_hline(s, val_r.x, val_r.x + val_r.w - 1, val_r.y + val_r.h - sc, sc, UGUI_GRAY_MUTED, CANVAS1_BLACK);
    if (value_str && *value_str) {
        uint32_t vlen = (uint32_t)strlen(value_str);
        int vw = ugui_text_serif_width(s, value_str, vlen);
        int vx = val_r.x + (val_r.w - vw) / 2;
        ugui_text_serif(s, vx, ty, value_str, vlen, false, false, false);
    }

    ugui_disable_next(ctx, (dis >> 1) & 1u);
    ugui_toolbar_button(s, ctx, inc_r, "+", inc_id, false);
}

void ugui_segmented(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                    const char *const *labels, int count, int selected_idx,
                    uint16_t base_id) {
    uint8_t dis = take_disabled(ctx);
    if (!s || !labels || count <= 0 || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    int seg_w = r.w / count;

    for (int i = 0; i < count; i++) {
        ugui_rect_t seg = {
            .x = (int16_t)(r.x + i * seg_w),
            .y = r.y,
            .w = (int16_t)((i == count - 1) ? (r.w - i * seg_w) : seg_w),
            .h = r.h,
        };
        bool seg_dis = i < 8 && ((dis >> i) & 1u);
        bool sel = (i == selected_idx) || (!seg_dis && ugui_is_armed(ctx, (uint16_t)(base_id + (uint16_t)i), (int16_t)i));
        ugui_fill(s, seg, sel ? UGUI_GRAY_INK : UGUI_GRAY_CARD, sel ? CANVAS1_BLACK : CANVAS1_WHITE);
        ugui_hline(s, seg.x, seg.x + seg.w - 1, seg.y, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        ugui_hline(s, seg.x, seg.x + seg.w - 1, seg.y + seg.h - sc, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        ugui_vline(s, seg.x, seg.y, seg.y + seg.h - 1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        if (i == count - 1) {
            ugui_vline(s, seg.x + seg.w - sc, seg.y, seg.y + seg.h - 1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        }

        if (labels[i] && *labels[i]) {
            uint32_t llen = (uint32_t)strlen(labels[i]);
            int lw = ugui_text_serif_width(s, labels[i], llen);
            int lh = ugui_serif_line_h(s);
            int lx = seg.x + (seg.w - lw) / 2;
            int ly = seg.y + (seg.h - lh) / 2;
            ugui_text_serif(s, lx, ly, labels[i], llen, true, false, sel);
        }
        if (seg_dis) ugui_dim(s, seg);
        add_hit_s(s, ctx, seg, (uint16_t)(base_id + (uint16_t)i), (int16_t)i, seg_dis ? UGUI_HIT_DISABLED : 0);
    }
}

void ugui_list_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                   const char *prefix, const char *title, const char *right_meta,
                   uint16_t id, int16_t index, bool selected) {
    uint8_t dis = take_disabled(ctx);
    selected = !dis && (selected || ugui_is_armed(ctx, id, index));
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    int lh = ugui_serif_line_h(s);
    int ty = r.y + (r.h - lh) / 2;

    ugui_fill(s, r, selected ? UGUI_GRAY_INK : UGUI_GRAY_CARD,
              selected ? CANVAS1_BLACK : CANVAS1_WHITE);

    int cx = r.x + 8 * sc;
    if (prefix && *prefix) {
        int pw = ugui_text_serif(s, cx, ty, prefix, (uint32_t)strlen(prefix), true, false, selected);
        cx += pw + 6 * sc;
    } else {
        int dot_sz = 3 * sc;
        int dot_y = r.y + (r.h - dot_sz) / 2;
        ugui_rect_t dot = { (int16_t)cx, (int16_t)dot_y, (int16_t)dot_sz, (int16_t)dot_sz };
        ugui_fill(s, dot, selected ? UGUI_GRAY_PAPER : UGUI_GRAY_INK,
                  selected ? CANVAS1_WHITE : CANVAS1_BLACK);
        cx += dot_sz + 8 * sc;
    }
    if (title && *title) {
        ugui_text_serif(s, cx, ty, title, (uint32_t)strlen(title), false, false, selected);
    }
    if (right_meta && *right_meta) {
        uint32_t mlen = (uint32_t)strlen(right_meta);
        int mw = ugui_text_serif_width(s, right_meta, mlen);
        int mx = r.x + r.w - mw - 8 * sc;
        if (mx > cx + 16 * sc) {
            ugui_text_serif(s, mx, ty, right_meta, mlen, false, true, selected);
        }
    }

    /* Subtle bottom divider rule */
    ugui_hline(s, r.x + 4 * sc, r.x + r.w - 1 - 4 * sc, r.y + r.h - 1, 1,
               UGUI_GRAY_TRACK, CANVAS1_GREY);

    if (dis) ugui_dim(s, r);
    add_hit_s(s, ctx, r, id, index, dis ? UGUI_HIT_DISABLED : 0);
}

void ugui_scrollbar(const ugui_surface_t *s, ugui_rect_t track, int scroll_y,
                    int total_h, int view_h) {
    if (!s || track.w <= 0 || track.h <= 0 || total_h <= view_h || view_h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;

    ugui_fill(s, track, UGUI_GRAY_TRACK, CANVAS1_GREY);

    int min_thumb = 16 * sc;
    int thumb_h = (track.h * view_h) / total_h;
    if (thumb_h < min_thumb) thumb_h = min_thumb;
    if (thumb_h > track.h) thumb_h = track.h;

    int range = total_h - view_h;
    int sy = scroll_y < 0 ? 0 : (scroll_y > range ? range : scroll_y);
    int thumb_y = track.y + (range > 0 ? (sy * (track.h - thumb_h)) / range : 0);

    ugui_rect_t thumb = {
        .x = track.x,
        .y = (int16_t)thumb_y,
        .w = track.w,
        .h = (int16_t)thumb_h,
    };
    ugui_fill(s, thumb, UGUI_GRAY_SHADOW, CANVAS1_BLACK);
}

int ugui_vlist(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t view, int count, int row_h,
               int *scroll_px, ugui_row_fn row, void *user) {
    if (!s || !scroll_px || !row || row_h <= 0 || view.w <= 0 || view.h <= 0) return 0;
    int max = count * row_h - view.h;
    if (max < 0) max = 0;
    if (*scroll_px > max) *scroll_px = max;
    if (*scroll_px < 0) *scroll_px = 0;
    ugui_surface_t vs = ugui_clip(s, view);
    int drawn = 0;
    for (int i = *scroll_px / row_h; i < count; i++) {
        int y = view.y + i * row_h - *scroll_px;
        if (y >= view.y + view.h) break;
        row(&vs, ctx, (ugui_rect_t){ view.x, (int16_t)y, view.w, (int16_t)row_h }, i, user);
        drawn++;
    }
    return drawn;
}

int ugui_vlist_reveal(int scroll_px, int view_h, int row_h, int index) {
    int top = index * row_h, bottom = top + row_h;
    if (top < scroll_px) return top;
    if (bottom > scroll_px + view_h) return bottom - view_h;
    return scroll_px;
}


/* --- Controls ------------------------------------------------------------- */

static void frame(const ugui_surface_t *s, ugui_rect_t r, int t) {
    ugui_hline(s, r.x, r.x + r.w - 1, r.y, t, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_hline(s, r.x, r.x + r.w - 1, r.y + r.h - t, t, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_vline(s, r.x, r.y, r.y + r.h - 1, t, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    ugui_vline(s, r.x + r.w - t, r.y, r.y + r.h - 1, t, UGUI_GRAY_BORDER, CANVAS1_BLACK);
}

/* A control's label at x, centred in r's height; returns its width. */
static int row_label(const ugui_surface_t *s, int x, ugui_rect_t r, const char *label, bool inverted) {
    if (!label || !*label) return 0;
    return ugui_text_serif(s, x, r.y + (r.h - ugui_serif_line_h(s)) / 2, label, (uint32_t)strlen(label),
                           false, false, inverted);
}

void ugui_toggle(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *label, bool on, uint16_t id) {
    uint8_t dis = take_disabled(ctx);
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    bool armed = !dis && ugui_is_armed(ctx, id, 0);
    if (armed) ugui_fill(s, r, UGUI_GRAY_TRACK, CANVAS1_GREY);
    row_label(s, r.x + 4 * sc, r, label, false);
    int sw = 36 * sc, sh = 18 * sc;
    ugui_rect_t pill = { (int16_t)(r.x + r.w - sw - 4 * sc), (int16_t)(r.y + (r.h - sh) / 2), (int16_t)sw, (int16_t)sh };
    ugui_fill(s, pill, on ? UGUI_GRAY_INK : UGUI_GRAY_CARD, on ? CANVAS1_BLACK : CANVAS1_WHITE);
    frame(s, pill, sc);
    int kw = sh - 4 * sc;
    ugui_rect_t knob = { (int16_t)(on ? pill.x + sw - kw - 2 * sc : pill.x + 2 * sc), (int16_t)(pill.y + 2 * sc),
                         (int16_t)kw, (int16_t)kw };
    ugui_fill(s, knob, UGUI_GRAY_CARD, CANVAS1_WHITE);
    frame(s, knob, sc);
    if (dis) ugui_dim(s, r);
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : 0);
}

void ugui_checkbox(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *label, bool checked, uint16_t id) {
    uint8_t dis = take_disabled(ctx);
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    bool armed = !dis && ugui_is_armed(ctx, id, 0);
    int bs = 14 * sc;
    ugui_rect_t box = { (int16_t)(r.x + 4 * sc), (int16_t)(r.y + (r.h - bs) / 2), (int16_t)bs, (int16_t)bs };
    ugui_fill(s, box, armed ? UGUI_GRAY_INK : UGUI_GRAY_CARD, armed ? CANVAS1_BLACK : CANVAS1_WHITE);
    frame(s, box, sc);
    if (checked)                                         /* the Classic Mac cross */
        for (int i = 2 * sc; i < bs - 2 * sc; i += sc) {
            uint8_t g = armed ? UGUI_GRAY_CARD : UGUI_GRAY_INK;
            unsigned p = armed ? CANVAS1_WHITE : CANVAS1_BLACK;
            ugui_fill(s, (ugui_rect_t){ (int16_t)(box.x + i), (int16_t)(box.y + i), (int16_t)sc, (int16_t)sc }, g, p);
            ugui_fill(s, (ugui_rect_t){ (int16_t)(box.x + bs - sc - i), (int16_t)(box.y + i), (int16_t)sc, (int16_t)sc }, g, p);
        }
    row_label(s, box.x + bs + 6 * sc, r, label, false);
    if (dis) ugui_dim(s, r);
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : 0);
}

/* The slider's track: inset by half a thumb at each end. */
static void slider_track(const ugui_surface_t *s, ugui_rect_t r, int *x0, int *x1) {
    int sc = s->scale > 0 ? s->scale : 1;
    *x0 = r.x + 6 * sc;
    *x1 = r.x + r.w - 1 - 6 * sc;
    if (*x1 <= *x0) *x1 = *x0 + 1;
}

int ugui_slider_value(const ugui_surface_t *s, ugui_rect_t r, int x, int min, int max) {
    if (!s || max <= min) return min;
    int x0, x1;
    slider_track(s, r, &x0, &x1);
    if (x <= x0) return min;
    if (x >= x1) return max;
    return min + (int)(((long)(x - x0) * (max - min) + (x1 - x0) / 2) / (x1 - x0));
}

void ugui_slider(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, int value, int min, int max, uint16_t id) {
    uint8_t dis = take_disabled(ctx);
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    if (max <= min) max = min + 1;
    if (value < min) value = min;
    if (value > max) value = max;
    int x0, x1;
    slider_track(s, r, &x0, &x1);
    int cy = r.y + r.h / 2;
    ugui_fill(s, (ugui_rect_t){ (int16_t)x0, (int16_t)(cy - 2 * sc), (int16_t)(x1 - x0 + 1), (int16_t)(4 * sc) },
              UGUI_GRAY_TRACK, CANVAS1_GREY);
    int tx = x0 + (int)((long)(value - min) * (x1 - x0) / (max - min));
    ugui_fill(s, (ugui_rect_t){ (int16_t)x0, (int16_t)(cy - 2 * sc), (int16_t)(tx - x0 + 1), (int16_t)(4 * sc) },
              UGUI_GRAY_INK, CANVAS1_BLACK);
    bool armed = !dis && ugui_is_armed(ctx, id, 0);
    ugui_rect_t thumb = { (int16_t)(tx - 5 * sc), (int16_t)(cy - 9 * sc), (int16_t)(10 * sc), (int16_t)(18 * sc) };
    ugui_fill(s, thumb, armed ? UGUI_GRAY_INK : UGUI_GRAY_CARD, armed ? CANVAS1_BLACK : CANVAS1_WHITE);
    frame(s, thumb, sc);
    if (dis) ugui_dim(s, r);
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : UGUI_HIT_DRAG);
}

void ugui_progress(const ugui_surface_t *s, ugui_rect_t r, int permille) {
    if (!s || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    ugui_fill(s, r, UGUI_GRAY_CARD, CANVAS1_WHITE);
    int inner = r.w - 4 * sc;
    int fw = (int)((long)inner * permille / 1000);
    if (fw > 0)
        ugui_fill(s, (ugui_rect_t){ (int16_t)(r.x + 2 * sc), (int16_t)(r.y + 2 * sc), (int16_t)fw, (int16_t)(r.h - 4 * sc) },
                  UGUI_GRAY_INK, CANVAS1_BLACK);
    frame(s, r, sc);
}

void ugui_tab_bar(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *const *labels,
                  int count, int selected, uint16_t base_id) {
    uint8_t dis = take_disabled(ctx);
    if (!s || !labels || count <= 0 || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    int tw = r.w / count;
    ugui_hline(s, r.x, r.x + r.w - 1, r.y + r.h - sc, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
    for (int i = 0; i < count; i++) {
        uint16_t id = (uint16_t)(base_id + (uint16_t)i);
        bool sel = i == selected, tdis = i < 8 && ((dis >> i) & 1u);
        bool armed = !tdis && ugui_is_armed(ctx, id, (int16_t)i);
        ugui_rect_t t = { (int16_t)(r.x + i * tw + sc), (int16_t)(r.y + (sel ? 0 : 3 * sc)),
                          (int16_t)((i == count - 1 ? r.w - i * tw : tw) - 2 * sc), (int16_t)(r.h - (sel ? 0 : 3 * sc)) };
        ugui_fill(s, t, armed ? UGUI_GRAY_INK : sel ? UGUI_GRAY_CARD : UGUI_GRAY_TOOLBAR,
                  armed ? CANVAS1_BLACK : CANVAS1_WHITE);
        ugui_hline(s, t.x, t.x + t.w - 1, t.y, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        ugui_vline(s, t.x, t.y, t.y + t.h - 1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        ugui_vline(s, t.x + t.w - sc, t.y, t.y + t.h - 1, sc, UGUI_GRAY_BORDER, CANVAS1_BLACK);
        if (sel)                                         /* open toward the content */
            ugui_hline(s, t.x + sc, t.x + t.w - 1 - sc, r.y + r.h - sc, sc, UGUI_GRAY_CARD, CANVAS1_WHITE);
        if (labels[i]) {
            uint32_t n = (uint32_t)strlen(labels[i]);
            int lw = ugui_text_serif_width(s, labels[i], n);
            ugui_text_serif(s, t.x + (t.w - lw) / 2, t.y + (t.h - ugui_serif_line_h(s)) / 2, labels[i], n,
                            sel, false, armed);
        }
        if (tdis) ugui_dim(s, t);
        add_hit_s(s, ctx, t, id, (int16_t)i, tdis ? UGUI_HIT_DISABLED : 0);
    }
}

int ugui_path_prefix(const char *path, int n) {
    if (!path || path[0] != '/') return 0;
    if (n <= 0) return 1;
    int i = 1, seg = 0;
    while (path[i]) {
        while (path[i] && path[i] != '/') i++;
        if (++seg == n) return i;
        if (path[i]) i++;
    }
    return i;
}

void ugui_path_bar(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *path, uint16_t id) {
    (void)take_disabled(ctx);
    if (!s || !path || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    /* The segments: "/" then each name. */
    const char *seg[16];
    int len[16], n = 0;
    seg[n] = "/"; len[n++] = 1;
    for (const char *p = path; *p && n < 16;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != '/') q++;
        seg[n] = p; len[n++] = (int)(q - p);
        p = q;
    }
    int pad = 6 * sc, sep_w = ugui_text_serif_width(s, ">", 1) + 2 * pad;
    int w[16], total = 0;
    for (int i = 0; i < n; i++) { w[i] = ugui_text_serif_width(s, seg[i], (uint32_t)len[i]) + 2 * pad; total += w[i] + (i ? sep_w : 0); }
    int ell_w = ugui_text_serif_width(s, "...", 3) + 2 * pad;
    int first = 0;
    while (total > r.w && first < n - 1) { total -= w[first] + sep_w; first++; if (first == 1) total += ell_w + sep_w; }
    ugui_fill(s, r, UGUI_GRAY_TOOLBAR, CANVAS1_WHITE);
    int x = r.x, ty = r.y + (r.h - ugui_serif_line_h(s)) / 2;
    if (first > 0) {
        ugui_text_serif(s, x + pad, ty, "...", 3, false, true, false);
        x += ell_w;
        ugui_text_serif(s, x + pad, ty, ">", 1, false, false, false);
        x += sep_w;
    }
    for (int i = first; i < n; i++) {
        if (i > first) { ugui_text_serif(s, x + pad, ty, ">", 1, false, false, false); x += sep_w; }
        ugui_rect_t sr = { (int16_t)x, r.y, (int16_t)w[i], r.h };
        bool armed = ugui_is_armed(ctx, id, (int16_t)i), last = i == n - 1;
        if (armed) ugui_fill(s, sr, UGUI_GRAY_INK, CANVAS1_BLACK);
        ugui_text_serif(s, x + pad, ty, seg[i], (uint32_t)len[i], last, false, armed);
        add_hit_s(s, ctx, sr, id, (int16_t)i, 0);
        x += w[i];
    }
}

void ugui_text_init(ugui_text_t *t, char *buf, uint16_t cap) {
    t->buf = buf;
    t->cap = cap;
    t->len = (uint16_t)strlen(buf);
    t->caret = t->anchor = t->len;
}

static void text_cut(ugui_text_t *t) {
    int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
    if (a == b) return;
    memmove(t->buf + a, t->buf + b, (size_t)(t->len - b + 1));
    t->len = (uint16_t)(t->len - (b - a));
    t->caret = t->anchor = (uint16_t)a;
}

ugui_text_ev_t ugui_text_key(ugui_text_t *t, uint32_t key, uint8_t mods) {
    if (!t || !t->buf) return UGUI_TEXT_NONE;
    bool sel = (mods & KMOD_SHIFT) != 0;
    bool has_sel = t->anchor != t->caret;
    if (key == '\r' || key == '\n') return UGUI_TEXT_SUBMIT;
    if (key == 0x1b) return UGUI_TEXT_CANCEL;
    if (key == KEY_LEFT || key == KEY_RIGHT || key == KEY_HOME || key == KEY_END) {
        int c = t->caret;
        if (!sel && has_sel && (key == KEY_LEFT || key == KEY_RIGHT)) {
            int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
            c = key == KEY_LEFT ? a : b;                 /* to that end of the selection */
        } else if (key == KEY_LEFT) { if (c > 0) c--; }
        else if (key == KEY_RIGHT) { if (c < t->len) c++; }
        else c = key == KEY_HOME ? 0 : t->len;
        t->caret = (uint16_t)c;
        if (!sel) t->anchor = t->caret;
        return UGUI_TEXT_MOVED;
    }
    if (key == 0x7f || key == 0x08 || key == KEY_DELETE) {
        if (has_sel) { text_cut(t); return UGUI_TEXT_CHANGED; }
        if (key == KEY_DELETE) { if (t->caret >= t->len) return UGUI_TEXT_MOVED; t->anchor = (uint16_t)(t->caret + 1); }
        else { if (t->caret == 0) return UGUI_TEXT_MOVED; t->anchor = (uint16_t)(t->caret - 1); }
        text_cut(t);
        return UGUI_TEXT_CHANGED;
    }
    if (key >= 0x20 && key < 0x7f) {                     /* ASCII until the fonts go further (71.9) */
        text_cut(t);
        if (t->len + 1u >= t->cap) return UGUI_TEXT_MOVED;
        memmove(t->buf + t->caret + 1, t->buf + t->caret, (size_t)(t->len - t->caret + 1));
        t->buf[t->caret] = (char)key;
        t->len++;
        t->caret = t->anchor = (uint16_t)(t->caret + 1);
        return UGUI_TEXT_CHANGED;
    }
    return UGUI_TEXT_NONE;
}

void ugui_textfield(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const ugui_text_t *t, uint16_t id) {
    uint8_t dis = take_disabled(ctx);
    if (!s || !t || r.w <= 0 || r.h <= 0) return;
    int sc = s->scale > 0 ? s->scale : 1;
    ugui_fill(s, r, UGUI_GRAY_CARD, CANVAS1_WHITE);
    frame(s, r, sc);
    ugui_rect_t in = { (int16_t)(r.x + 4 * sc), (int16_t)(r.y + 2 * sc), (int16_t)(r.w - 8 * sc), (int16_t)(r.h - 4 * sc) };
    bool focused = ctx && ctx->focus_id == id;
    /* Scrolled so the caret shows. */
    int caret_x = ugui_text_serif_width(s, t->buf, t->caret);
    int off = caret_x > in.w - 2 * sc ? caret_x - (in.w - 2 * sc) : 0;
    ugui_surface_t cs = ugui_clip(s, in);
    int ty = r.y + (r.h - ugui_serif_line_h(s)) / 2, tx = in.x - off;
    ugui_text_serif(&cs, tx, ty, t->buf, t->len, false, false, false);
    if (focused) {
        int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
        if (a != b) {
            int xa = tx + ugui_text_serif_width(s, t->buf, (uint32_t)a), xb = tx + ugui_text_serif_width(s, t->buf, (uint32_t)b);
            ugui_invert(&cs, xa, in.y, xb - 1, in.y + in.h - 1);
        } else {
            ugui_fill(&cs, (ugui_rect_t){ (int16_t)(tx + caret_x), in.y, (int16_t)sc, in.h }, UGUI_GRAY_INK, CANVAS1_BLACK);
        }
    }
    if (dis) ugui_dim(s, r);
    add_hit_s(s, ctx, r, id, 0, dis ? UGUI_HIT_DISABLED : 0);
}

void ugui_set_focus(ugui_ctx_t *ctx, uint16_t id, int16_t index) {
    if (!ctx) return;
    ctx->focus_id = id;
    ctx->focus_index = index;
}

void ugui_modal_begin(const ugui_surface_t *s, ugui_ctx_t *ctx) {
    if (s) ugui_dim(s, (ugui_rect_t){ 0, 0, (int16_t)s->w, (int16_t)s->h });
    if (!ctx) return;
    ctx->modal = true;
    ctx->modal_from = (int16_t)ctx->count;
}

/* The buttons along a modal's bottom edge, from the right; the last label
 * is the default, framed bold. */
static void modal_buttons(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t box, const char *const *labels,
                          int count, uint16_t base_id) {
    int sc = s->scale > 0 ? s->scale : 1;
    int bh = 22 * sc, pad = 10 * sc, x = box.x + box.w - pad;
    for (int i = count - 1; i >= 0; i--) {
        int bw = ugui_text_serif_width(s, labels[i], (uint32_t)strlen(labels[i])) + 24 * sc;
        if (bw < 64 * sc) bw = 64 * sc;
        x -= bw;
        ugui_rect_t b = { (int16_t)x, (int16_t)(box.y + box.h - pad - bh), (int16_t)bw, (int16_t)bh };
        ugui_button(s, ctx, b, labels[i], (uint16_t)(base_id + (uint16_t)i), false);
        if (i == count - 1)                              /* the default: a second frame around it */
            frame(s, (ugui_rect_t){ (int16_t)(b.x - 3 * sc), (int16_t)(b.y - 3 * sc), (int16_t)(b.w + 6 * sc), (int16_t)(b.h + 6 * sc) }, 2 * sc);
        x -= 12 * sc;
    }
}

ugui_rect_t ugui_dialog(const ugui_surface_t *s, ugui_ctx_t *ctx, const char *title, const char *message,
                        const char *const *buttons, int count, uint16_t base_id) {
    ugui_rect_t none = { 0, 0, 0, 0 };
    if (!s) return none;
    int sc = s->scale > 0 ? s->scale : 1;
    int lh = ugui_serif_line_h(s), lines = 0, widest = title ? ugui_text_serif_width(s, title, (uint32_t)strlen(title)) : 0;
    for (const char *p = message; p && *p;) {
        const char *q = p;
        while (*q && *q != '\n') q++;
        int lw = ugui_text_serif_width(s, p, (uint32_t)(q - p));
        if (lw > widest) widest = lw;
        lines++;
        p = *q ? q + 1 : q;
    }
    int w = widest + 40 * sc, min_w = 260 * sc;
    if (w < min_w) w = min_w;
    if (w > s->w - 20 * sc) w = s->w - 20 * sc;
    int h = 16 * sc + (title ? lh + 8 * sc : 0) + lines * lh + 16 * sc + 22 * sc + 14 * sc;
    ugui_rect_t box = { (int16_t)((s->w - w) / 2), (int16_t)((s->h - h) / 2), (int16_t)w, (int16_t)h };
    ugui_modal_begin(s, ctx);
    ugui_box_shadow(s, box, false);
    frame(s, (ugui_rect_t){ (int16_t)(box.x + 3 * sc), (int16_t)(box.y + 3 * sc), (int16_t)(box.w - 6 * sc), (int16_t)(box.h - 6 * sc) }, sc);
    int y = box.y + 16 * sc;
    if (title) { ugui_text_serif(s, box.x + 20 * sc, y, title, (uint32_t)strlen(title), true, false, false); y += lh + 8 * sc; }
    for (const char *p = message; p && *p;) {
        const char *q = p;
        while (*q && *q != '\n') q++;
        ugui_text_serif(s, box.x + 20 * sc, y, p, (uint32_t)(q - p), false, false, false);
        y += lh;
        p = *q ? q + 1 : q;
    }
    if (buttons && count > 0) modal_buttons(s, ctx, box, buttons, count, base_id);
    return box;
}

ugui_rect_t ugui_action_sheet(const ugui_surface_t *s, ugui_ctx_t *ctx, const char *title,
                              const char *const *items, int count, uint16_t base_id) {
    ugui_rect_t none = { 0, 0, 0, 0 };
    if (!s || !items || count <= 0) return none;
    int sc = s->scale > 0 ? s->scale : 1;
    int lh = ugui_serif_line_h(s), ih = 28 * sc, gap = 6 * sc, pad = 10 * sc;
    int w = s->w > 420 * sc ? 400 * sc : s->w - 20 * sc;
    int h = pad + (title ? lh + gap : 0) + count * (ih + gap) + pad - gap;
    ugui_rect_t box = { (int16_t)((s->w - w) / 2), (int16_t)(s->h - h - 10 * sc), (int16_t)w, (int16_t)h };
    ugui_modal_begin(s, ctx);
    ugui_box_shadow(s, box, false);
    int y = box.y + pad;
    if (title) {
        int tw = ugui_text_serif_width(s, title, (uint32_t)strlen(title));
        ugui_text_serif(s, box.x + (box.w - tw) / 2, y, title, (uint32_t)strlen(title), false, true, false);
        y += lh + gap;
    }
    for (int i = 0; i < count; i++) {
        ugui_button(s, ctx, (ugui_rect_t){ (int16_t)(box.x + pad), (int16_t)y, (int16_t)(box.w - 2 * pad), (int16_t)ih },
                    items[i], (uint16_t)(base_id + (uint16_t)i), false);
        y += ih + gap;
    }
    return box;
}

static void selftest_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t row, int index, void *user) {
    (void)user;
    ugui_list_row(s, ctx, row, NULL, "r", NULL, 50, (int16_t)index, false);
}

bool ugui_selftest(void) {
    uint8_t fb[16 * 64];
    memset(fb, 0, sizeof(fb));
    canvas1_t c1;
    canvas1_init(&c1, fb, 16, 128, 64);

    ugui_surface_t surf = {
        .c1 = &c1,
        .aa_fb = NULL,
        .aa_stride = 0,
        .w = 128,
        .h = 64,
        .scale = 1,
    };

    ugui_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ugui_begin(&ctx);

    ugui_rect_t btn1 = { .x = 4, .y = 4, .w = 50, .h = 20 };
    ugui_rect_t btn2 = { .x = 60, .y = 4, .w = 60, .h = 20 };
    ugui_toolbar_button(&surf, &ctx, btn1, "Back", 101, false);
    ugui_button(&surf, &ctx, btn2, "Open", 102, true);

    const char *const segs[3] = { "A", "B", "C" };
    ugui_rect_t seg_r = { .x = 4, .y = 32, .w = 120, .h = 20 };
    ugui_segmented(&surf, &ctx, seg_r, segs, 3, 1, 200);

    if (ctx.count != 5) return false;

    int16_t idx = -1;
    if (ugui_hit_test(&ctx, 10, 10, &idx) != 101) return false;
    if (ugui_hit_test(&ctx, 75, 10, &idx) != 102) return false;
    if (ugui_hit_test(&ctx, 64, 40, &idx) != 201 || idx != 1) return false;
    if (ugui_hit_test(&ctx, 110, 40, &idx) != 202 || idx != 2) return false;
    if (ugui_hit_test(&ctx, 2, 2, &idx) != 0) return false;

    /* Verify border pixels were drawn onto the 1-bpp canvas */
    if (canvas1_get(&c1, 4, 4) != 1u) return false;

    /* Press feedback: arming the widget under a point, once; a second
     * segment re-arms; disarming clears it; the arming survives a pass. */
    if (!ugui_arm(&ctx, 10, 10) || !ugui_is_armed(&ctx, 101, 0)) return false;
    if (ugui_arm(&ctx, 12, 12)) return false;                     /* same widget: no change */
    if (!ugui_arm(&ctx, 110, 40) || !ugui_is_armed(&ctx, 202, 2) || ugui_is_armed(&ctx, 101, 0)) return false;
    ugui_begin(&ctx);
    if (!ugui_is_armed(&ctx, 202, 2)) return false;
    if (!ugui_disarm(&ctx) || ugui_disarm(&ctx) || ugui_is_armed(&ctx, 202, 2)) return false;

    /* Keyboard focus over the same boxes: 101 102 on top, segments 200..202
     * below. The first key only shows it; Tab / Shift-Tab wrap; an arrow
     * picks the nearest that way (Down from 102 is the segment under it);
     * Enter activates; a touch hides the ring; a lost widget gives the
     * focus to the first. */
    ugui_begin(&ctx);
    ugui_add_hit(&ctx, btn1, 101, 0);
    ugui_add_hit(&ctx, btn2, 102, 0);
    for (int i = 0; i < 3; i++)
        ugui_add_hit(&ctx, (ugui_rect_t){ (int16_t)(4 + 40 * i), 32, 40, 20 }, (uint16_t)(200 + i), (int16_t)i);
    uint16_t fid = 0;
    int16_t fidx = -1;
    if (ugui_nav(&ctx, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 101, 0)) return false;
    if (ugui_nav(&ctx, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 102, 0)) return false;
    if (ugui_nav(&ctx, '\t', KMOD_SHIFT, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 101, 0)) return false;
    if (ugui_nav(&ctx, '\t', KMOD_SHIFT, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 202, 2)) return false;
    if (ugui_nav(&ctx, KEY_UP, 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 102, 0)) return false;
    if (ugui_nav(&ctx, KEY_LEFT, 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 101, 0)) return false;
    if (ugui_nav(&ctx, KEY_LEFT, 0, &fid, &fidx) != UGUI_NAV_EDGE) return false;
    if (ugui_nav(&ctx, KEY_RIGHT, 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 102, 0)) return false;
    if (ugui_nav(&ctx, KEY_DOWN, 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 202, 2)) return false;
    if (ugui_nav(&ctx, KEY_DOWN, 0, &fid, &fidx) != UGUI_NAV_EDGE) return false;
    if (ugui_nav(&ctx, '\r', 0, &fid, &fidx) != UGUI_NAV_ACTIVATE || fid != 202 || fidx != 2) return false;
    if (ugui_nav(&ctx, 0x1b, 0, &fid, &fidx) != UGUI_NAV_BACK || ugui_nav(&ctx, 'x', 0, &fid, &fidx) != UGUI_NAV_NONE) return false;
    (void)ugui_arm(&ctx, 10, 10);
    (void)ugui_disarm(&ctx);
    if (ugui_is_focused(&ctx, 202, 2)) return false;                /* touch: hidden */
    if (ugui_nav(&ctx, '\r', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&ctx, 202, 2)) return false;
    ctx.focus_id = 999;
    ugui_end(NULL, &ctx);
    if (!ugui_is_focused(&ctx, 101, 0)) return false;

    /* The ring inverts a dotted frame inside the focused widget: a drawn
     * button's interior pixel at the ring changes, one further in does not. */
    memset(fb, 0, sizeof(fb));
    ugui_begin(&ctx);
    ugui_add_hit(&ctx, btn1, 101, 0);
    ugui_end(&surf, &ctx);
    if (canvas1_get(&c1, btn1.x + 2, btn1.y + 2) != 1u || canvas1_get(&c1, btn1.x + 10, btn1.y + 10) != 0u) return false;

    /* A dimmed part: its tap hits nothing (not even what lies under it), it
     * cannot be armed, the focus skips it; the stepper's mask dims one half
     * only, and the next widget starts enabled again. */
    ugui_ctx_t dc;
    memset(&dc, 0, sizeof dc);
    ugui_begin(&dc);
    ugui_add_hit(&dc, (ugui_rect_t){ 0, 0, 128, 64 }, 300, 0);   /* underneath */
    ugui_disable_next(&dc, 1);
    ugui_toolbar_button(&surf, &dc, btn1, "Back", 101, false);
    ugui_disable_next(&dc, 2);
    ugui_stepper(&surf, &dc, (ugui_rect_t){ 0, 32, 128, 20 }, NULL, "1", 110, 111);
    ugui_toolbar_button(&surf, &dc, btn2, "Open", 102, false);
    if (ugui_hit_test(&dc, 10, 10, NULL) != 0 || ugui_arm(&dc, 10, 10)) return false;
    if (ugui_hit_test(&dc, 75, 10, NULL) != 102) return false;
    if (ugui_hit_test(&dc, 112, 40, NULL) != 0) return false;               /* the + */
    int16_t di = -1;
    uint16_t dec = 0;
    for (int x = 0; x < 128 && !dec; x++) {
        uint16_t h = ugui_hit_test(&dc, x, 40, &di);
        if (h == 110) dec = h;
    }
    if (dec != 110) return false;                                         /* the - acts */
    if (ugui_nav(&dc, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&dc, 300, 0)) return false;
    if (ugui_nav(&dc, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&dc, 110, 0)) return false;
    if (ugui_nav(&dc, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&dc, 102, 0)) return false;
    if (ugui_nav(&dc, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&dc, 300, 0)) return false;

    /* Touch targets: two 10x10 boxes 4 px apart grow to 30x30; a point
     * between them goes to the nearer; a point on one as drawn is that one;
     * bounds clip the growth (a point outside them hits nothing). */
    ugui_ctx_t tc;
    memset(&tc, 0, sizeof tc);
    ugui_begin(&tc);
    ugui_set_min_target(&tc, 30);
    ugui_add_hit(&tc, (ugui_rect_t){ 40, 20, 10, 10 }, 1, 0);
    ugui_add_hit(&tc, (ugui_rect_t){ 54, 20, 10, 10 }, 2, 0);
    ugui_slop_bounds(&tc, (ugui_rect_t){ 0, 40, 128, 12 });
    ugui_add_hit(&tc, (ugui_rect_t){ 10, 42, 8, 8 }, 3, 0);
    ugui_slop_bounds_end(&tc);
    ugui_rect_t g = ugui_touch_rect(&tc, 0);
    if (g.w != 30 || g.h != 30 || g.x != 30 || g.y != 10) return false;
    if (ugui_hit_test(&tc, 51, 25, NULL) != 1 || ugui_hit_test(&tc, 53, 25, NULL) != 2) return false;
    if (ugui_hit_test(&tc, 45, 12, NULL) != 1 || ugui_hit_test(&tc, 56, 25, NULL) != 2) return false;
    if (ugui_hit_test(&tc, 45, 5, NULL) != 0) return false;                  /* beyond the target */
    if (ugui_hit_test(&tc, 4, 46, NULL) != 3 || ugui_hit_test(&tc, 14, 38, NULL) != 0) return false;
    ugui_set_min_target(&tc, 0);
    if (ugui_hit_test(&tc, 51, 25, NULL) != 0) return false;                  /* as drawn only */

    /* Clipping: a fill through a clip lands only inside it; clips nest as
     * an intersection; a widget's hit-box is cut to the clip. */
    memset(fb, 0, sizeof(fb));
    ugui_surface_t cs = ugui_clip(&surf, (ugui_rect_t){ 10, 10, 20, 20 });
    ugui_fill(&cs, (ugui_rect_t){ 0, 0, 128, 64 }, UGUI_GRAY_INK, CANVAS1_BLACK);
    if (canvas1_get(&c1, 5, 5) != 0u || canvas1_get(&c1, 15, 15) != 1u || canvas1_get(&c1, 30, 30) != 0u ||
        canvas1_get(&c1, 29, 29) != 1u) return false;
    ugui_surface_t cs2 = ugui_clip(&cs, (ugui_rect_t){ 20, 0, 100, 100 });
    if (cs2.clip.x != 20 || cs2.clip.y != 10 || cs2.clip.w != 10 || cs2.clip.h != 20) return false;
    ugui_ctx_t hc;
    memset(&hc, 0, sizeof hc);
    ugui_begin(&hc);
    ugui_toolbar_button(&cs, &hc, (ugui_rect_t){ 0, 12, 40, 10 }, "x", 7, false);
    if (ugui_hit_test(&hc, 5, 15, NULL) != 0 || ugui_hit_test(&hc, 12, 15, NULL) != 7) return false;
    ugui_toolbar_button(&cs, &hc, (ugui_rect_t){ 60, 12, 20, 10 }, "y", 8, false);   /* wholly outside */
    if (hc.count != 1) return false;

    /* The virtual list: ten 10 px rows, a 25 px view scrolled by 15 draws
     * rows 1..3 (row 1 cut at the top); the scroll is clamped; reveal. */
    int sp = 15;
    ugui_begin(&hc);
    if (ugui_vlist(&surf, &hc, (ugui_rect_t){ 0, 30, 100, 25 }, 10, 10, &sp, selftest_row, NULL) != 3) return false;
    if (hc.count != 3 || hc.hits[0].index != 1 || hc.hits[0].rect.y != 30 || hc.hits[0].rect.h != 5) return false;
    sp = 1000;
    (void)ugui_vlist(&surf, &hc, (ugui_rect_t){ 0, 30, 100, 25 }, 10, 10, &sp, selftest_row, NULL);
    if (sp != 75) return false;
    if (ugui_vlist_reveal(0, 25, 10, 9) != 75 || ugui_vlist_reveal(75, 25, 10, 0) != 0 ||
        ugui_vlist_reveal(10, 25, 10, 2) != 10) return false;

    /* Text field editing: insert, caret moves, Shift-selection replaced by
     * typing, Backspace / Delete, the buffer's bound, submit / cancel. */
    char tb[8] = "ab";
    ugui_text_t tf;
    ugui_text_init(&tf, tb, sizeof tb);
    if (ugui_text_key(&tf, 'c', 0) != UGUI_TEXT_CHANGED || strcmp(tb, "abc") != 0 || tf.caret != 3) return false;
    (void)ugui_text_key(&tf, KEY_HOME, 0);
    (void)ugui_text_key(&tf, KEY_RIGHT, KMOD_SHIFT);
    (void)ugui_text_key(&tf, KEY_RIGHT, KMOD_SHIFT);                 /* "ab" selected */
    if (ugui_text_key(&tf, 'X', 0) != UGUI_TEXT_CHANGED || strcmp(tb, "Xc") != 0 || tf.caret != 1) return false;
    (void)ugui_text_key(&tf, 0x7f, 0);
    (void)ugui_text_key(&tf, KEY_DELETE, 0);
    if (tf.len != 0 || tb[0] != '\0') return false;
    for (int i = 0; i < 10; i++) (void)ugui_text_key(&tf, 'z', 0);
    if (tf.len != 7 || tb[7] != '\0') return false;                    /* 8 bytes: 7 and the NUL */
    if (ugui_text_key(&tf, '\r', 0) != UGUI_TEXT_SUBMIT || ugui_text_key(&tf, 0x1b, 0) != UGUI_TEXT_CANCEL ||
        ugui_text_key(&tf, '\t', 0) != UGUI_TEXT_NONE) return false;

    /* Path prefixes as the path bar counts its segments. */
    if (ugui_path_prefix("/sd0/docs", 0) != 1 || ugui_path_prefix("/sd0/docs", 1) != 4 ||
        ugui_path_prefix("/sd0/docs", 2) != 9) return false;

    /* A modal: only what follows ugui_modal_begin() is hit or focused; a tap
     * on what is under it hits nothing. */
    ugui_begin(&hc);
    hc.focus_id = 0;
    hc.focus_shown = false;
    ugui_add_hit(&hc, (ugui_rect_t){ 0, 0, 128, 64 }, 60, 0);
    ugui_modal_begin(NULL, &hc);
    ugui_add_hit(&hc, (ugui_rect_t){ 40, 20, 20, 10 }, 61, 0);
    if (ugui_hit_test(&hc, 5, 5, NULL) != 0 || ugui_hit_test(&hc, 45, 25, NULL) != 61) return false;
    if (ugui_nav(&hc, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&hc, 61, 0)) return false;
    if (ugui_nav(&hc, '\t', 0, &fid, &fidx) != UGUI_NAV_MOVED || !ugui_is_focused(&hc, 61, 0)) return false;

    return true;
}
