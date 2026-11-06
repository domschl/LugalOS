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
}

void ugui_add_hit(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id, int16_t index) {
    if (!ctx || id == 0 || ctx->count >= UGUI_MAX_HITBOXES) return;
    if (r.w <= 0 || r.h <= 0) return;
    ugui_hit_t *h = &ctx->hits[ctx->count++];
    h->rect = r;
    h->id = id;
    h->index = index;
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
        if (i == from) continue;
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
    if (slot < 0 || !ctx->focus_shown) {                 /* first key: only show where it is */
        if (slot < 0) focus_to(ctx, 0);
        ctx->focus_shown = true;
        return UGUI_NAV_MOVED;
    }
    if (activate) {
        if (id) *id = ctx->hits[slot].id;
        if (index) *index = ctx->hits[slot].index;
        return UGUI_NAV_ACTIVATE;
    }
    if (tab) {
        int step = (mods & KMOD_SHIFT) ? -1 : 1;
        focus_to(ctx, (slot + step + ctx->count) % ctx->count);
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
static void ugui_invert(const ugui_surface_t *s, int x0, int y0, int x1, int y1) {
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
    if (slot < 0) {
        if (ctx->focus_id == 0 && !ctx->focus_shown) return;
        focus_to(ctx, 0);
        slot = 0;
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

uint16_t ugui_hit_test(const ugui_ctx_t *ctx, int x, int y, int16_t *out_index) {
    if (!ctx) return 0;
    /* Search in reverse order so later (topmost) widgets take precedence */
    for (int i = ctx->count - 1; i >= 0; i--) {
        const ugui_hit_t *h = &ctx->hits[i];
        if (x >= h->rect.x && x < (int)h->rect.x + (int)h->rect.w &&
            y >= h->rect.y && y < (int)h->rect.y + (int)h->rect.h) {
            if (out_index) *out_index = h->index;
            return h->id;
        }
    }
    return 0;
}

void ugui_fill(const ugui_surface_t *s, ugui_rect_t r, uint8_t gray, unsigned pat1) {
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

int ugui_text_serif(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
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

int ugui_text_mono(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
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
    active = active || ugui_is_armed(ctx, id, 0);
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
    ugui_add_hit(ctx, r, id, 0);
}

void ugui_toolbar_button(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                         const char *label, uint16_t id, bool active) {
    active = active || ugui_is_armed(ctx, id, 0);
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

    /* Subtle 1*sc bottom-right shadow */
    ugui_hline(s, x0 + sc, x1 + sc, y1 + 1, sc, UGUI_GRAY_SHADOW, CANVAS1_BLACK);
    ugui_vline(s, x1 + 1, y0 + sc, y1 + sc, sc, UGUI_GRAY_SHADOW, CANVAS1_BLACK);

    if (label && *label) {
        uint32_t len = (uint32_t)strlen(label);
        int tw = ugui_text_serif_width(s, label, len);
        int lh = ugui_serif_line_h(s);
        int tx = r.x + (r.w - tw) / 2;
        int ty = r.y + (r.h - lh) / 2;
        if (tx < r.x + 2 * sc) tx = r.x + 2 * sc;
        ugui_text_serif(s, tx, ty, label, len, true, false, active);
    }
    ugui_add_hit(ctx, r, id, 0);
}

void ugui_card(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
               const uint16_t icon16[16], const char *title, const char *subtitle,
               uint16_t id, bool active) {
    active = active || ugui_is_armed(ctx, id, 0);
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

    ugui_add_hit(ctx, r, id, 0);
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
    if (!s || r.w <= 0 || r.h <= 0) return;
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

    ugui_toolbar_button(s, ctx, inc_r, "+", inc_id, false);
}

void ugui_segmented(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                    const char *const *labels, int count, int selected_idx,
                    uint16_t base_id) {
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
        bool sel = (i == selected_idx) || ugui_is_armed(ctx, (uint16_t)(base_id + (uint16_t)i), (int16_t)i);
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
        ugui_add_hit(ctx, seg, (uint16_t)(base_id + (uint16_t)i), (int16_t)i);
    }
}

void ugui_list_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                   const char *prefix, const char *title, const char *right_meta,
                   uint16_t id, int16_t index, bool selected) {
    selected = selected || ugui_is_armed(ctx, id, index);
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

    ugui_add_hit(ctx, r, id, index);
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

    return true;
}
