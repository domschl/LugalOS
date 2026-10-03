/*
 * The screen: a menu bar, the desktop, and the terminal and a canvas in
 * framed tiles -- 37.1/37.1a/37.3b, plan/phase37_screen_layouts_and_apps.md
 * §1. See drivers/screen.h for the geometry and the canvas protocol.
 *
 * Everything but screen_init() runs in the U-mode `lcdterm` task on the
 * RP2350-LCD-7, so it is LCDTERM_UTEXT, uses no libc and no string literals
 * (a literal is .rodata, outside the task's domain). screen_init() runs once,
 * in the kernel, before the task exists.
 */

#include "drivers/screen.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"

LCDTERM_UTEXT static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* The menu bar: white, the rule, the system name in bold on the left, and
 * the indicators ending 16 px from the right edge. The indicators win when
 * the two do not fit. Glyph rows 0..15 from y 2. */
LCDTERM_UTEXT static void draw_menu(screen_t *scr) {
    const canvas1_t *cv = &scr->cv;
    int w = cv->w;
    canvas1_fill(cv, 0, 0, w - 1, SCREEN_MENU_H - 2, CANVAS1_WHITE);
    canvas1_hline(cv, 0, w - 1, SCREEN_MENU_H - 1, CANVAS1_BLACK);
    uint32_t rn = cstr_len(scr->right);
    unsigned rlen = canvas1_text_len(scr->right, rn);
    int rx = w - 16 - 8 * (int)rlen;
    if (rx < 8) rx = 8;
    canvas1_text(cv, rx, 2, scr->right, rn, rlen, 0, FONT8X16_H, false);
    int room = (rx - 16 - 8) / 8;
    if (room > 0)
        canvas1_text(cv, 8, 2, scr->name, cstr_len(scr->name), (unsigned)room, 0, FONT8X16_H, true);

    /* Phase 44: Ribbon visualization stripe in the menu bar */
    if (scr->ribbon.count > 0) {
        int sx = 8 + 8 * (int)cstr_len(scr->name) + 16;
        int max_w = rx - 16 - sx;
        if (max_w > 20) {
            int cur_bx = sx;
            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                const ribbon_win_t *win = &scr->ribbon.wins[i];
                int bw = (win->cols >= 90) ? 22 :
                         (win->cols >= 60) ? 15 :
                         (win->cols >= 45) ? 11 : 8;
                if (cur_bx + bw > rx - 16) break;
                if (i == scr->ribbon.active_idx) {
                    /* Focused window: solid black mini-block */
                    canvas1_fill(cv, cur_bx, 4, cur_bx + bw - 1, 14, CANVAS1_BLACK);
                } else {
                    /* Inactive window: 1-px outline */
                    canvas1_hline(cv, cur_bx, cur_bx + bw - 1, 4, CANVAS1_BLACK);
                    canvas1_hline(cv, cur_bx, cur_bx + bw - 1, 14, CANVAS1_BLACK);
                    canvas1_vline(cv, cur_bx, 4, 14, CANVAS1_BLACK);
                    canvas1_vline(cv, cur_bx + bw - 1, 4, 14, CANVAS1_BLACK);
                }
                cur_bx += bw + 3;
            }
        }
    }
}

/* A title bar over x0..x1 from y0: the frame's top border, a white row, six
 * stripes with white rows between, a white row, a border -- 17 rows -- and
 * the title in bold, centred to the pixel in a white box. The glyph's rows
 * 1..14 are drawn from one pixel below the border, which leaves capitals two
 * white rows above and three below (the owner's adjustment of the C2
 * mockup). */
LCDTERM_UTEXT static void draw_titlebar(const canvas1_t *cv, int x0, int y0, int x1, const char *t) {
    canvas1_fill(cv, x0 + 1, y0 + 1, x1 - 1, y0 + 15, CANVAS1_WHITE);
    canvas1_hline(cv, x0, x1, y0, CANVAS1_BLACK);
    canvas1_hline(cv, x0, x1, y0 + 16, CANVAS1_BLACK);
    for (int y = y0 + 3; y <= y0 + 13; y += 2) canvas1_hline(cv, x0 + 2, x1 - 2, y, CANVAS1_BLACK);
    uint32_t n = cstr_len(t);
    unsigned len = canvas1_text_len(t, n);
    int max = (x1 - x0 - 1 - 40) / 8;        /* leave some stripes either side */
    if (max < 0) max = 0;
    if (len > (unsigned)max) len = (unsigned)max;
    if (len > 0) {
        int bw = 8 * (int)len + 12;
        int cx = (x0 + x1 + 1) / 2 - bw / 2;
        canvas1_fill(cv, cx, y0 + 1, cx + bw - 1, y0 + 15, CANVAS1_WHITE);
        canvas1_text(cv, cx + 6, y0 + 1, t, n, len, 1, 15, true);
    }
}

/* A tile: its frame and shadow, its interior white, and its title bar. */
LCDTERM_UTEXT static void draw_tile(const canvas1_t *cv, int x0, int y0, int x1, int y1, const char *t) {
    canvas1_fill(cv, x0, y0, x1, y1, CANVAS1_WHITE);
    canvas1_hline(cv, x0, x1, y1, CANVAS1_BLACK);
    canvas1_vline(cv, x0, y0, y1, CANVAS1_BLACK);
    canvas1_vline(cv, x1, y0, y1, CANVAS1_BLACK);
    canvas1_hline(cv, x0 + 1, x1 + 1, y1 + 1, CANVAS1_BLACK);   /* the shadow */
    canvas1_vline(cv, x1 + 1, y0 + 1, y1 + 1, CANVAS1_BLACK);
    draw_titlebar(cv, x0, y0, x1, t);
}

LCDTERM_UTEXT static bool has_text(const screen_t *scr) {
    if (scr->ribbon.count > 0) return ribbon_find_term(&scr->ribbon) >= 0;
    return scr->layout != SCREEN_LAYOUT_CANVAS;
}

LCDTERM_UTEXT static bool has_canvas(const screen_t *scr) {
    if (scr->ribbon.count > 0) return ribbon_find_canvas(&scr->ribbon) >= 0;
    return scr->layout != SCREEN_LAYOUT_TEXT;
}

LCDTERM_UTEXT static void draw_text_title(screen_t *scr) {
    if (has_text(scr)) draw_titlebar(&scr->cv, scr->fx0, scr->fy0, scr->fx1, scr->vt.title);
    scr->title_drawn = scr->vt.title_seq;
}

/* --- 38.8: the canvas stores ----------------------------------------------
 *
 * A slot holds the canvas tile's drawable area, row by row, (w + 7) / 8
 * bytes a row, pixel x of a row in bit x % 8 of byte x / 8 -- the
 * framebuffer's own order, but from the tile's left edge rather than the
 * screen's, so the same slot restores a split at either side. */

LCDTERM_UTEXT static uint8_t *store_slot(const screen_t *scr) {
    unsigned l = scr->layout;
    if (!scr->store || l == SCREEN_LAYOUT_TEXT || l > SCREEN_STORES) return 0;
    return scr->store + (uint32_t)(l - 1u) * scr->store_slot;
}

/* The canvas as it is now, into its layout's slot. */
LCDTERM_UTEXT static void store_save(screen_t *scr) {
    uint8_t *d = store_slot(scr);
    if (!d) return;
    const canvas1_t *cc = &scr->cc;
    unsigned rb = ((unsigned)cc->w + 7u) / 8u, s = cc->ox & 7u;
    for (unsigned y = 0; y < cc->h; y++) {
        const uint8_t *src = cc->fb + (uint32_t)(cc->oy + y) * cc->stride + (cc->ox >> 3);
        for (unsigned i = 0; i < rb; i++)
            d[i] = s ? (uint8_t)((src[i] >> s) | (src[i + 1u] << (8u - s))) : src[i];
        d += rb;
    }
    unsigned bit = 1u << (scr->layout - 1u);
    scr->store_gen[scr->layout - 1u] = scr->draw_gen;
    scr->store_ok = (uint8_t)(scr->store_ok | bit);
}

/* The layout's slot back onto the canvas, if it holds what was there when
 * nothing has been drawn since; false (and nothing done) otherwise. */
LCDTERM_UTEXT static bool store_restore(screen_t *scr) {
    const uint8_t *d = store_slot(scr);
    if (!d) return false;
    unsigned l = scr->layout - 1u;
    if (!(scr->store_ok & (1u << l)) || scr->store_gen[l] != scr->draw_gen) return false;
    const canvas1_t *cc = &scr->cc;
    unsigned rb = ((unsigned)cc->w + 7u) / 8u, s = cc->ox & 7u;
    unsigned tail = (unsigned)cc->w & 7u;
    for (unsigned y = 0; y < cc->h; y++) {
        uint8_t *dst = cc->fb + (uint32_t)(cc->oy + y) * cc->stride + (cc->ox >> 3);
        for (unsigned i = 0; i < rb; i++) {
            unsigned vm = (i + 1u == rb && tail) ? (1u << tail) - 1u : 0xffu;
            unsigned v = d[i] & vm;
            unsigned m0 = (vm << s) & 0xffu;
            dst[i] = (uint8_t)((dst[i] & ~m0) | ((v << s) & m0));
            if (s) {
                unsigned m1 = vm >> (8u - s);
                dst[i + 1u] = (uint8_t)((dst[i + 1u] & ~m1) | ((v >> (8u - s)) & m1));
            }
        }
        d += rb;
    }
    return true;
}

void screen_set_store(screen_t *scr, void *mem, uint32_t bytes) {
    uint32_t need = SCREEN_STORE_BYTES(scr->cv.w, scr->cv.h);
    scr->store_ok = 0;
    scr->store = (mem && bytes >= need) ? (uint8_t *)mem : 0;
    scr->store_slot = need / SCREEN_STORES;
}

LCDTERM_UTEXT static bool place(screen_t *scr, unsigned layout);

/* Every pixel again. The canvas comes back blank, so its damage moves on --
 * unless `restore` and its layout's store has it (38.8). */
LCDTERM_UTEXT static void draw_all_from(screen_t *scr, bool restore) {
    const canvas1_t *cv = &scr->cv;
    (void)place(scr, scr->layout);
    canvas1_fill(cv, 0, SCREEN_MENU_H, cv->w - 1, cv->h - 1, CANVAS1_GREY);
    if (scr->ribbon.count > 0) {
        int32_t xv = scr->ribbon.x_view;
        for (uint8_t i = 0; i < scr->ribbon.count; i++) {
            const ribbon_win_t *w = &scr->ribbon.wins[i];
            int32_t x0 = w->rx0 - xv;
            int32_t x1 = w->rx1 - xv;
            if (x1 + 1 >= 0 && x0 < (int32_t)cv->w) {
                const char *title = (w->type == RIBBON_WIN_TERM && i == scr->ribbon.active_idx)
                                  ? scr->vt.title
                                  : (w->type == RIBBON_WIN_CANVAS ? scr->ctitle : w->title);
                draw_tile(cv, (int)x0, SCREEN_TILE_Y, (int)x1, (int)scr->fy1, title);
            }
        }
        if (has_canvas(scr)) {
            if (!restore || !store_restore(scr)) scr->damage++;
        }
    } else {
        if (has_text(scr)) draw_tile(cv, scr->fx0, scr->fy0, scr->fx1, scr->fy1, scr->vt.title);
        if (has_canvas(scr)) {
            draw_tile(cv, scr->cx0, scr->cy0, scr->cx1, scr->cy1, scr->ctitle);
            if (!restore || !store_restore(scr)) scr->damage++;
        }
    }
    scr->title_drawn = scr->vt.title_seq;
    draw_menu(scr);
    vtterm_repaint(&scr->vt);
}

LCDTERM_UTEXT static void draw_all(screen_t *scr) {
    draw_all_from(scr, false);
}

/* The tiles' geometry for `layout` (screen.h's table), and the text window
 * moved and resized to match. False for a split the screen is too narrow
 * for. */
LCDTERM_UTEXT static bool place(screen_t *scr, unsigned layout) {
    int C = scr->cv.w / 8;
    unsigned rows = SCREEN_TEXT_ROWS(scr->cv.h);
    int y1 = SCREEN_TEXT_Y + 16 * (int)rows + 1;
    int col0 = 1, cols = scr->full_cols;

    if (scr->ribbon.count > 2) {
        scr->layout = (uint8_t)layout;
        /* Position active terminal */
        int term_idx = -1;
        if (scr->ribbon.active_idx < scr->ribbon.count &&
            scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
            term_idx = (int)scr->ribbon.active_idx;
        } else {
            term_idx = ribbon_find_term(&scr->ribbon);
        }
        if (term_idx >= 0) {
            const ribbon_win_t *tw = &scr->ribbon.wins[term_idx];
            int32_t x0 = tw->rx0 - scr->ribbon.x_view;
            int c0 = (int)((x0 + 4) / 8);
            if (c0 < 1) c0 = 1;
            if (c0 > C - 2) c0 = C - 2;
            int max_c = C - 1 - c0;
            int win_c = (int)tw->cols;
            if (win_c > max_c) win_c = max_c;
            if (win_c < 1) win_c = 1;
            col0 = c0;
            cols = win_c;
            scr->fx0 = (int16_t)x0;
            scr->fx1 = (int16_t)(x0 + 8 * tw->cols + 6);
            scr->fy0 = SCREEN_TILE_Y;
            scr->fy1 = (int16_t)y1;
        } else {
            scr->fx0 = scr->fx1 = 0;
            scr->fy0 = scr->fy1 = 0;
        }

        /* Position canvas if present in ribbon */
        int c_idx = ribbon_find_canvas(&scr->ribbon);
        if (c_idx >= 0) {
            const ribbon_win_t *cw = &scr->ribbon.wins[c_idx];
            int32_t cx0 = cw->rx0 - scr->ribbon.x_view;
            int32_t cx1 = cw->rx1 - scr->ribbon.x_view;
            scr->cx0 = (int16_t)cx0;
            scr->cx1 = (int16_t)cx1;
            scr->cy0 = SCREEN_TILE_Y;
            scr->cy1 = (int16_t)y1;
            int cw_w = (int)(scr->cx1 - scr->cx0 - 1);
            int cw_h = (int)(scr->cy1 - scr->cy0 - 17);
            /* canvas1_t's origin is unsigned: a tile that is not wholly on
             * the panel gets no drawable area until it slides into view. */
            if (cw_w < 0 || cx0 < 0 || cx1 >= (int32_t)scr->cv.w) cw_w = 0;
            if (cw_h < 0) cw_h = 0;
            if (cw_w == 0 || cw_h == 0) {
                canvas1_window(&scr->cc, &scr->cv, 0, 0, 0, 0);
            } else {
                canvas1_window(&scr->cc, &scr->cv, scr->cx0 + 1, scr->cy0 + 17,
                               (unsigned)cw_w, (unsigned)cw_h);
            }
        } else {
            scr->cx0 = scr->cx1 = 0;
            scr->cy0 = scr->cy1 = 0;
            canvas1_window(&scr->cc, &scr->cv, 0, 0, 0, 0);
        }

        bool term_visible = (term_idx >= 0 && scr->fx0 >= 0 &&
                             scr->fx1 < (int16_t)scr->cv.w);
        if (!term_visible) { col0 = 1; cols = 1; }
        fbtext_t text;
        fbtext_init(&text, scr->cv.fb + SCREEN_TEXT_Y * scr->cv.stride + (uint32_t)col0, scr->cv.stride,
                    (unsigned)cols, rows);
        text.xbyte = (uint16_t)col0;
        text.whole_rows = (term_idx >= 0 && cols == scr->full_cols && col0 == 1);
        vtterm_resize(&scr->vt, &text);
        vtterm_set_hidden(&scr->vt, !term_visible);
        return true;
    }

    if (layout == SCREEN_LAYOUT_SPLIT_WIDE || layout == SCREEN_LAYOUT_SPLIT_HALF ||
        layout == SCREEN_LAYOUT_SPLIT_NARROW) {
        cols = layout == SCREEN_LAYOUT_SPLIT_WIDE ? 38 : layout == SCREEN_LAYOUT_SPLIT_HALF ? 48 : 64;
        col0 = scr->swapped ? 1 : C - 1 - cols;
        if (8 * (C - 1 - cols) - 14 < 4 + 24) return false;   /* no room for a canvas */
    } else if (layout != SCREEN_LAYOUT_TEXT && layout != SCREEN_LAYOUT_CANVAS) {
        return false;
    }
    scr->layout = (uint8_t)layout;
    scr->fx0 = (int16_t)(8 * col0 - 4);
    scr->fx1 = (int16_t)(8 * (col0 + cols) + 2);
    scr->fy0 = SCREEN_TILE_Y;
    scr->fy1 = (int16_t)y1;
    bool split = layout != SCREEN_LAYOUT_TEXT && layout != SCREEN_LAYOUT_CANVAS;
    if (split && scr->swapped) {                 /* 37.5a: text left, canvas right */
        scr->cx0 = (int16_t)(scr->fx1 + 10);
        scr->cx1 = (int16_t)(8 * (C - 1) + 2);
    } else {
        scr->cx0 = 4;
        scr->cx1 = (int16_t)(layout == SCREEN_LAYOUT_CANVAS ? 8 * (C - 1) + 2 : 8 * col0 - 14);
    }
    scr->cy0 = SCREEN_TILE_Y;
    scr->cy1 = (int16_t)y1;
    canvas1_window(&scr->cc, &scr->cv, scr->cx0 + 1, scr->cy0 + 17,
                   has_canvas(scr) ? (unsigned)(scr->cx1 - scr->cx0 - 1) : 0u,
                   has_canvas(scr) ? (unsigned)(scr->cy1 - scr->cy0 - 17) : 0u);
    fbtext_t text;
    fbtext_init(&text, scr->cv.fb + SCREEN_TEXT_Y * scr->cv.stride + (uint32_t)col0, scr->cv.stride,
                (unsigned)cols, rows);
    text.xbyte = (uint16_t)col0;
    text.whole_rows = (layout == SCREEN_LAYOUT_TEXT && cols == scr->full_cols && col0 == 1);
    vtterm_resize(&scr->vt, &text);
    vtterm_set_hidden(&scr->vt, !has_text(scr));

    /* Sync ribbon geometry when ribbon has <= 2 windows (standard layout sync) */
    if (scr->ribbon.count <= 2) {
        if (split) {
            scr->ribbon.count = 2;
            if (scr->swapped) {
                scr->ribbon.wins[0].type = RIBBON_WIN_TERM;
                scr->ribbon.wins[0].cols = (uint8_t)cols;
                scr->ribbon.wins[0].rx0 = scr->fx0;
                scr->ribbon.wins[0].rx1 = scr->fx1;
                scr->ribbon.wins[1].type = RIBBON_WIN_CANVAS;
                scr->ribbon.wins[1].cols = (uint8_t)((scr->cx1 - scr->cx0 - 6) / 8);
                scr->ribbon.wins[1].rx0 = scr->cx0;
                scr->ribbon.wins[1].rx1 = scr->cx1;
            } else {
                scr->ribbon.wins[0].type = RIBBON_WIN_CANVAS;
                scr->ribbon.wins[0].cols = (uint8_t)((scr->cx1 - scr->cx0 - 6) / 8);
                scr->ribbon.wins[0].rx0 = scr->cx0;
                scr->ribbon.wins[0].rx1 = scr->cx1;
                scr->ribbon.wins[1].type = RIBBON_WIN_TERM;
                scr->ribbon.wins[1].cols = (uint8_t)cols;
                scr->ribbon.wins[1].rx0 = scr->fx0;
                scr->ribbon.wins[1].rx1 = scr->fx1;
            }
        } else if (layout == SCREEN_LAYOUT_TEXT) {
            scr->ribbon.count = 1;
            scr->ribbon.active_idx = 0;
            scr->ribbon.wins[0].type = RIBBON_WIN_TERM;
            scr->ribbon.wins[0].cols = (uint8_t)cols;
            scr->ribbon.wins[0].rx0 = scr->fx0;
            scr->ribbon.wins[0].rx1 = scr->fx1;
        } else if (layout == SCREEN_LAYOUT_CANVAS) {
            scr->ribbon.count = 1;
            scr->ribbon.active_idx = 0;
            scr->ribbon.wins[0].type = RIBBON_WIN_CANVAS;
            scr->ribbon.wins[0].cols = (uint8_t)((scr->cx1 - scr->cx0 - 6) / 8);
            scr->ribbon.wins[0].rx0 = scr->cx0;
            scr->ribbon.wins[0].rx1 = scr->cx1;
        }
        scr->ribbon.x_view = 0;
        scr->ribbon.x_target = 0;
    }
    return true;
}

void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h) {
    canvas1_init(&scr->cv, fb, stride, w, h);
    scr->full_cols = (uint8_t)SCREEN_TEXT_COLS(w);
    scr->damage = 0;
    scr->store = 0;
    scr->store_ok = 0;
    scr->store_slot = 0;
    scr->draw_gen = 0;
    scr->redrawing = 0;
    scr->locked = 0;
    scr->swapped = 0;
    const char *name = "LugalOS";
    unsigned i = 0;
    for (; name[i] && i < SCREEN_NAME_MAX - 1u; i++) scr->name[i] = name[i];
    scr->name[i] = '\0';
    const char *ct = "Canvas";
    for (i = 0; ct[i] && i < SCREEN_CTITLE_MAX - 1u; i++) scr->ctitle[i] = ct[i];
    scr->ctitle[i] = '\0';
    scr->right[0] = '\0';
    fbtext_t text;
    fbtext_init(&text, (uint8_t *)fb + SCREEN_TEXT_Y * stride + SCREEN_TEXT_X / 8, stride,
                SCREEN_TEXT_COLS(w), SCREEN_TEXT_ROWS(h));
    vtterm_init(&scr->vt, &text, scr->shadow);
    vtterm_set_title(&scr->vt, name, 7);
    ribbon_init(&scr->ribbon, (uint16_t)w, (uint16_t)h);
    ribbon_insert(&scr->ribbon, 0, RIBBON_WIN_TERM, 0, (uint8_t)SCREEN_TEXT_COLS(w), name);
    (void)place(scr, SCREEN_LAYOUT_TEXT);
    draw_all(scr);
}

LCDTERM_UTEXT void screen_write(screen_t *scr, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) vtterm_putc(&scr->vt, s[i]);
    if (scr->vt.title_seq != scr->title_drawn) draw_text_title(scr);
}

LCDTERM_UTEXT void screen_set_title(screen_t *scr, const char *s, uint32_t n) {
    vtterm_set_title(&scr->vt, s, n);
    if (scr->vt.title_seq != scr->title_drawn) draw_text_title(scr);
}

LCDTERM_UTEXT void screen_set_right(screen_t *scr, const char *s, uint32_t n) {
    if (n > SCREEN_RIGHT_MAX - 1u) {
        n = SCREEN_RIGHT_MAX - 1u;
        while (n > 0 && ((unsigned char)s[n] & 0xc0u) == 0x80u) n--;   /* not mid-character */
    }
    for (uint32_t i = 0; i < n; i++) scr->right[i] = s[i];
    scr->right[n] = '\0';
    draw_menu(scr);
}

LCDTERM_UTEXT void screen_repaint(screen_t *scr) {
    draw_all(scr);
}

LCDTERM_UTEXT bool screen_set_layout(screen_t *scr, unsigned layout) {
    /* The layout already showing: nothing to do, and above all no damage.
     * A program's redraw function typically starts by asking for its
     * layout; if that cleared the canvas, the next prompt would see new
     * damage and call it again, for ever. */
    if (layout == scr->layout) return true;
    if (has_canvas(scr)) store_save(scr);
    if (!place(scr, layout)) return false;     /* place() changed nothing */
    scr->redrawing = 0;
    if (layout == SCREEN_LAYOUT_TEXT) scr->locked = 0;
    draw_all_from(scr, true);
    return true;
}

LCDTERM_UTEXT void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows) {
    *cols = scr->vt.text.cols;
    *rows = scr->vt.text.rows;
}

/* --- The canvas protocol (37.3b) ------------------------------------------ */

LCDTERM_UTEXT static int arg16(const uint8_t *req, uint32_t n, uint32_t i) {
    uint32_t at = 1u + 2u * i;
    if (at + 2u > n) return 0;
    return (int)(int16_t)(uint16_t)(req[at] | ((uint16_t)req[at + 1u] << 8));
}

LCDTERM_UTEXT static unsigned colour(int c) {
    return c == 0 ? CANVAS1_WHITE : c == 2 ? CANVAS1_GREY : CANVAS1_BLACK;
}

LCDTERM_UTEXT void screen_canvas(screen_t *scr, const uint8_t *req, uint32_t n, uint8_t *reply) {
    const canvas1_t *cc = &scr->cc;
    uint8_t status = 0, pixel = 0;
    uint8_t op = n ? req[0] : 0;
    if (op == 'L') {
        status = (n >= 2 && screen_set_layout(scr, req[1])) ? 0 : 1;
    } else if (op == 'T') {
        uint32_t len = n - 1u;
        if (len > SCREEN_CTITLE_MAX - 1u) {
            len = SCREEN_CTITLE_MAX - 1u;
            while (len > 0 && (req[1u + len] & 0xc0u) == 0x80u) len--;
        }
        for (uint32_t i = 0; i < len; i++) scr->ctitle[i] = (char)req[1u + i];
        scr->ctitle[len] = '\0';
        int c_idx = ribbon_find_canvas(&scr->ribbon);
        if (c_idx >= 0) {
            ribbon_set_title(&scr->ribbon, (uint8_t)c_idx, scr->ctitle);
        }
        if (has_canvas(scr)) draw_titlebar(&scr->cv, scr->cx0, scr->cy0, scr->cx1, scr->ctitle);
    } else if (op == 'w') {
        /* The divider's five places, from text only to canvas only, with
         * the canvas on the left; swapped, the same places mirrored.
         * Computed, not a table: a const table is .rodata, outside the
         * lcdterm task's domain. */
        int dir = n >= 2 ? (int)(int8_t)req[1] : 0;
        if (scr->ribbon.count > 1) {
            if (ribbon_resize_active(&scr->ribbon, dir > 0)) {
                draw_all_from(scr, true);
                status = 0;
            } else {
                status = 1;
            }
        } else {
            int at = scr->layout == SCREEN_LAYOUT_TEXT ? 0
                   : scr->layout == SCREEN_LAYOUT_SPLIT_NARROW ? 1
                   : scr->layout == SCREEN_LAYOUT_SPLIT_HALF ? 2
                   : scr->layout == SCREEN_LAYOUT_SPLIT_WIDE ? 3 : 4;
            int to = at + (scr->swapped ? -dir : dir);
            unsigned next = to == 0 ? SCREEN_LAYOUT_TEXT : to == 1 ? SCREEN_LAYOUT_SPLIT_NARROW
                          : to == 2 ? SCREEN_LAYOUT_SPLIT_HALF : to == 3 ? SCREEN_LAYOUT_SPLIT_WIDE
                          : SCREEN_LAYOUT_CANVAS;
            status = (scr->locked || dir == 0 || to < 0 || to > 4 || !screen_set_layout(scr, next)) ? 1 : 0;
        }
    } else if (op == 'X') {
        if (scr->locked) {
            status = 1;
        } else if (scr->ribbon.count > 1) {
            uint8_t a = scr->ribbon.active_idx;
            uint8_t b = (a + 1 < scr->ribbon.count) ? (a + 1) : (a > 0 ? a - 1 : 0);
            if (ribbon_swap(&scr->ribbon, a, b)) {
                draw_all_from(scr, true);
                status = 0;
            } else {
                status = 1;
            }
        } else if (scr->layout == SCREEN_LAYOUT_TEXT || scr->layout == SCREEN_LAYOUT_CANVAS) {
            /* One pane full: show the other one full (the owner's call). */
            status = screen_set_layout(scr, scr->layout == SCREEN_LAYOUT_TEXT ? SCREEN_LAYOUT_CANVAS
                                                                          : SCREEN_LAYOUT_TEXT) ? 0 : 1;
        } else {
            store_save(scr);            /* 38.8: the same canvas, other side */
            scr->swapped = (uint8_t)!scr->swapped;
            if (scr->layout != SCREEN_LAYOUT_TEXT && scr->layout != SCREEN_LAYOUT_CANVAS) {
                (void)place(scr, scr->layout);
                draw_all_from(scr, true);
            }
        }
    } else if (op == 'N') {
        /* Phase 44: Insert a new window in the ribbon (Cmd+Enter) */
        uint8_t vid = (n >= 2) ? req[1] : 0;
        uint8_t cols = (n >= 3 && req[2]) ? req[2] : 48;
        int idx = ribbon_insert(&scr->ribbon, (uint8_t)(scr->ribbon.active_idx + 1u),
                                RIBBON_WIN_TERM, vid, cols, 0);
        if (idx >= 0) {
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '<') {
        /* Phase 44: Focus left window (Cmd+Left) */
        if (ribbon_focus_step(&scr->ribbon, -1)) {
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '>') {
        /* Phase 44: Focus right window (Cmd+Right) */
        if (ribbon_focus_step(&scr->ribbon, 1)) {
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '[') {
        /* Phase 44: Move active window left (Cmd+Ctrl+Left) */
        if (ribbon_move(&scr->ribbon, scr->ribbon.active_idx, -1)) {
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == ']') {
        /* Phase 44: Move active window right (Cmd+Ctrl+Right) */
        if (ribbon_move(&scr->ribbon, scr->ribbon.active_idx, 1)) {
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'J') {
        /* Phase 44: Jump directly to window N (Cmd+1..9) */
        uint8_t target = (n >= 2) ? req[1] : 0;
        if (ribbon_focus(&scr->ribbon, target)) {
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'C') {
        /* Phase 44: Close active window (Cmd+W) */
        uint8_t closed_type = (scr->ribbon.active_idx < scr->ribbon.count)
                            ? scr->ribbon.wins[scr->ribbon.active_idx].type : 0;
        uint8_t closed_vid = (scr->ribbon.active_idx < scr->ribbon.count)
                           ? scr->ribbon.wins[scr->ribbon.active_idx].vterm_id : 0;
        if (scr->ribbon.count > 1 && ribbon_remove(&scr->ribbon, scr->ribbon.active_idx)) {
            if (closed_type == RIBBON_WIN_CANVAS || ribbon_find_canvas(&scr->ribbon) < 0) {
                scr->layout = SCREEN_LAYOUT_TEXT;
            }
            while (ribbon_step_scroll(&scr->ribbon, 0)) {
                draw_all_from(scr, true);
            }
            draw_all_from(scr, true);
            status = 0;
            reply[10] = closed_type;
            reply[11] = closed_vid;
        } else {
            status = 1;
        }
    } else if (op == 'K') {
        scr->locked = (n >= 2 && req[1]) ? 1 : 0;
    } else if (op == 'Z') {
        scr->store_ok = 0;              /* 38.8 */
        scr->redrawing = 0;
    } else if (op == 'D') {
        scr->redrawing = (n >= 2 && req[1]) ? 1 : 0;
    } else if (op == 'S' || op == 0) {
        /* the reply is all */
    } else if (!has_canvas(scr)) {
        status = 1;
    } else {
        int a = arg16(req, n, 0), b = arg16(req, n, 1), c = arg16(req, n, 2);
        int d = arg16(req, n, 3), e = arg16(req, n, 4);
        if (op == 'F')      canvas1_fill(cc, 0, 0, cc->w - 1, cc->h - 1, colour(a));
        else if (op == 'p') canvas1_fill(cc, a, b, a, b, colour(c));
        else if (op == 'l') canvas1_line(cc, a, b, c, d, colour(e));
        else if (op == 'r') { if (c > 0 && d > 0) canvas1_fill(cc, a, b, a + c - 1, b + d - 1, colour(e)); }
        else if (op == 'o') {
            if (c > 0 && d > 0) {
                unsigned k = colour(e);
                canvas1_hline(cc, a, a + c - 1, b, k);
                canvas1_hline(cc, a, a + c - 1, b + d - 1, k);
                canvas1_vline(cc, a, b, b + d - 1, k);
                canvas1_vline(cc, a + c - 1, b, b + d - 1, k);
            }
        }
        else if (op == 'c') canvas1_circle(cc, a, b, c, colour(d));
        else if (op == 'i') { if (c > 0 && d > 0) canvas1_invert(cc, a, b, a + c - 1, b + d - 1); }
        else if (op == 't') {
            if (n >= 6) {
                const char *s = (const char *)req + 6;
                uint32_t len = n - 6u;
                canvas1_text(cc, a, b, s, len, canvas1_text_len(s, len), 0, FONT8X16_H, req[5] != 0);
            }
        }
        else if (op == 'b') {
            unsigned cnt = (unsigned)(c < 0 ? 0 : c);
            if (n >= 9u + (cnt + 7u) / 8u) canvas1_row(cc, a, b, req + 9, cnt, (unsigned)(d < 1 ? 1 : d));
            else status = 1;
        }
        else if (op == 'g') pixel = (uint8_t)canvas1_get(cc, a, b);
        else status = 1;
        /* 38.8: drawn on -- every other layout's store is now out of date,
         * unless this is a redraw of the same picture ('D'). */
        if (op != 'g' && status == 0 && !scr->redrawing) scr->draw_gen++;
    }
    reply[0] = status;
    if (op != 'g' && scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
        reply[1] = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    } else {
        reply[1] = pixel;
    }
    reply[2] = (uint8_t)cc->w; reply[3] = (uint8_t)(cc->w >> 8);
    reply[4] = (uint8_t)cc->h; reply[5] = (uint8_t)(cc->h >> 8);
    reply[6] = (uint8_t)scr->damage; reply[7] = (uint8_t)(scr->damage >> 8);
    reply[8] = scr->layout;
    reply[9] = scr->swapped;
}
