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

LCDTERM_UTEXT static void shadow_clear(uint16_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

LCDTERM_UTEXT static int screen_find_term(const screen_t *scr, uint8_t vterm_id) {
    for (uint32_t i = 0; i < SCREEN_MAX_TERMS; i++) {
        if (scr->terms[i].in_use && scr->terms[i].vterm_id == vterm_id) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static int screen_alloc_term(screen_t *scr, uint8_t vterm_id) {
    int idx = screen_find_term(scr, vterm_id);
    if (idx >= 0) return idx;
    for (uint32_t i = 0; i < SCREEN_MAX_TERMS; i++) {
        if (!scr->terms[i].in_use) {
            scr->terms[i].in_use = true;
            scr->terms[i].vterm_id = vterm_id;
            uint32_t term_cells = (uint32_t)scr->full_cols * (uint32_t)SCREEN_TEXT_ROWS(scr->cv.h);
            uint16_t *t_shadow = scr->shadow + i * term_cells;
            shadow_clear(t_shadow, term_cells);
            fbtext_t text;
            fbtext_init(&text, 0, 0, scr->full_cols, SCREEN_TEXT_ROWS(scr->cv.h));
            vtterm_init(&scr->terms[i].vt, &text, t_shadow);
            vtterm_set_hidden(&scr->terms[i].vt, true);
            scr->terms[i].vt.title[0] = '\0';
            scr->terms[i].vt.title_seq = 0;
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static void screen_free_term(screen_t *scr, uint8_t vterm_id) {
    int idx = screen_find_term(scr, vterm_id);
    if (idx > 0) { /* never free root terminal 0 */
        scr->terms[idx].in_use = false;
        scr->terms[idx].vterm_id = 0;
        scr->terms[idx].vt.title[0] = '\0';
        scr->terms[idx].vt.title_seq = 0;
        vtterm_set_hidden(&scr->terms[idx].vt, true);
        uint32_t term_cells = (uint32_t)scr->full_cols * (uint32_t)SCREEN_TEXT_ROWS(scr->cv.h);
        shadow_clear(scr->shadow + idx * term_cells, term_cells);
    }
}

LCDTERM_UTEXT static int screen_find_canvas_by_vterm(const screen_t *scr, uint8_t vterm_id) {
    for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
        if (scr->canvases[i].in_use && scr->canvases[i].vterm_id == vterm_id) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static int screen_alloc_canvas(screen_t *scr, uint8_t vterm_id) {
    int idx = screen_find_canvas_by_vterm(scr, vterm_id);
    if (idx >= 0) return idx;
    for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
        if (!scr->canvases[i].in_use) {
            scr->canvases[i].in_use = true;
            scr->canvases[i].locked = false;
            scr->canvases[i].vterm_id = vterm_id;
            scr->canvases[i].title[0] = '\0';
            if (scr->store) {
                canvas1_init(&scr->canvases[i].backing,
                             scr->store + i * scr->store_slot,
                             scr->cv.w / 8u, scr->cv.w, scr->cv.h);
            }
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static void screen_free_canvas(screen_t *scr, uint8_t cid) {
    if (cid < SCREEN_MAX_CANVASES) {
        scr->canvases[cid].in_use = false;
        scr->canvases[cid].locked = false;
        scr->canvases[cid].vterm_id = 0;
        scr->canvases[cid].title[0] = '\0';
    }
}

/* Top bar indicator icons: SD card (11x12) and 9P network badge (14x10) */
LCDTERM_URODATA static const uint16_t icon_sd[12] = {
    0xff80, 0x80c0, 0xaa60, 0xaa20, 0x8020, 0x9b20,
    0x92a0, 0x9aa0, 0x8aa0, 0x9b20, 0x8020, 0xffe0
};

LCDTERM_URODATA static const uint16_t icon_p9[10] = {
    0x7ff8, 0xfffc, 0xce7c, 0xd14c, 0xd17c,
    0xcf4c, 0xc14c, 0xce4c, 0xfffc, 0x7ff8
};

LCDTERM_UTEXT static void draw_icon16(const canvas1_t *cv, int x, int y, int w, int h, const uint16_t *rows) {
    for (int r = 0; r < h; r++) {
        uint16_t row = rows[r];
        for (int c = 0; c < w; c++) {
            if ((row >> (15 - c)) & 1u) {
                canvas1_fill(cv, x + c, y + r, x + c, y + r, CANVAS1_BLACK);
            }
        }
    }
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

    /* Draw indicator icons left of the clock */
    int icon_left = rx;
    if (scr->status_flags & SCREEN_STATUS_P9) {
        icon_left -= (14 + 6);
        draw_icon16(cv, icon_left, 4, 14, 10, icon_p9);
    }
    if (scr->status_flags & SCREEN_STATUS_SD) {
        icon_left -= (11 + 6);
        draw_icon16(cv, icon_left, 3, 11, 12, icon_sd);
    }

    int room = (icon_left - 16 - 8) / 8;
    if (room > 0)
        canvas1_text(cv, 8, 2, scr->name, cstr_len(scr->name), (unsigned)room, 0, FONT8X16_H, true);

    /* Phase 44: Ribbon visualization stripe in the menu bar */
    if (scr->ribbon.count > 0) {
        int sx = 8 + 8 * (int)cstr_len(scr->name) + 16;
        int max_w = icon_left - 16 - sx;
        if (max_w > 20) {
            int cur_bx = sx;
            int min_vis_bx = -1;
            int max_vis_bx = -1;
            bool cut_left = false;
            bool cut_right = false;
            int cut_vis_x0 = -1;
            int cut_vis_x1 = -1;

            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                const ribbon_win_t *win = &scr->ribbon.wins[i];
                int bw = (win->cols >= 90) ? 22 :
                         (win->cols >= 60) ? 15 :
                         (win->cols >= 45) ? 11 : 8;
                if (cur_bx + bw > rx - 16) break;

                int32_t x0 = win->rx0 - scr->ribbon.x_view;
                int32_t x1 = win->rx1 - scr->ribbon.x_view;
                bool is_vis = (x1 >= 0 && x0 < (int32_t)cv->w);
                if (is_vis) {
                    int win_w = (int)(win->rx1 - win->rx0 + 1);
                    if (win_w < 1) win_w = 1;

                    if (min_vis_bx < 0) {
                        min_vis_bx = cur_bx;
                        if (x0 < 0) {
                            cut_left = true;
                            int cut_px = (int)(((-x0) * (int32_t)bw) / win_w);
                            if (cut_px >= bw) cut_px = bw - 1;
                            cut_vis_x0 = cur_bx + cut_px;
                        } else {
                            cut_vis_x0 = cur_bx;
                        }
                    }

                    if (x1 >= (int32_t)cv->w) {
                        cut_right = true;
                        int vis_px = (int)((((int32_t)cv->w - x0) * (int32_t)bw) / win_w);
                        if (vis_px < 1) vis_px = 1;
                        if (vis_px > bw) vis_px = bw;
                        cut_vis_x1 = cur_bx + vis_px - 1;
                    } else {
                        cut_right = false;
                        cut_vis_x1 = cur_bx + bw - 1;
                    }
                    max_vis_bx = cur_bx + bw - 1;
                }

                /* Block outline */
                canvas1_fill(cv, cur_bx, 4, cur_bx + bw - 1, 14, CANVAS1_WHITE);
                canvas1_hline(cv, cur_bx, cur_bx + bw - 1, 4, CANVAS1_BLACK);
                canvas1_hline(cv, cur_bx, cur_bx + bw - 1, 14, CANVAS1_BLACK);
                canvas1_vline(cv, cur_bx, 4, 14, CANVAS1_BLACK);
                canvas1_vline(cv, cur_bx + bw - 1, 4, 14, CANVAS1_BLACK);

                if (win->type == RIBBON_WIN_CANVAS && i != scr->ribbon.active_idx) {
                    /* Small graphic mark in center of inactive canvas block */
                    canvas1_fill(cv, cur_bx + bw / 2 - 1, 8, cur_bx + bw / 2 + 1, 10, CANVAS1_BLACK);
                }

                if (i == scr->ribbon.active_idx) {
                    /* Active window: zebra stripes inside mini-block */
                    for (int y = 6; y <= 12; y += 2) {
                        canvas1_hline(cv, cur_bx + 1, cur_bx + bw - 2, y, CANVAS1_BLACK);
                    }
                }
                cur_bx += bw + 3;
            }

            if (min_vis_bx >= 0 && max_vis_bx >= min_vis_bx) {
                int vx0 = cut_left ? cut_vis_x0 : (min_vis_bx - 2);
                int vx1 = cut_right ? cut_vis_x1 : (max_vis_bx + 2);
                if (vx0 < sx - 2) vx0 = sx - 2;
                if (vx1 > rx - 14) vx1 = rx - 14;
                if (vx1 >= vx0) {
                    canvas1_hline(cv, vx0, vx1, 2, CANVAS1_GREY);
                    canvas1_hline(cv, vx0, vx1, 16, CANVAS1_GREY);
                    canvas1_vline(cv, vx0, 2, 16, CANVAS1_GREY);
                    canvas1_vline(cv, vx1, 2, 16, CANVAS1_GREY);
                }
            }
        }
    }
}

/* A title bar over x0..x1 from y0: the frame's top border, a white row, six
 * stripes with white rows between (if active), a white row, a border -- 17 rows -- and
 * the title in bold, centred to the pixel in a white box. Inactive windows
 * remain plain white inside with the title (original classic Macintosh style). */
LCDTERM_UTEXT static void draw_titlebar(const canvas1_t *cv, int x0, int y0, int x1, const char *t, bool active) {
    int vx0 = x0 < 0 ? 0 : x0;
    int vx1 = x1 >= (int)cv->w ? (int)cv->w - 1 : x1;
    if (vx0 >= vx1) return;

    canvas1_fill(cv, vx0 + 1, y0 + 1, vx1 - 1, y0 + 15, CANVAS1_WHITE);
    canvas1_hline(cv, vx0, vx1, y0, CANVAS1_BLACK);
    canvas1_hline(cv, vx0, vx1, y0 + 16, CANVAS1_BLACK);
    if (active) {
        for (int y = y0 + 3; y <= y0 + 13; y += 2) canvas1_hline(cv, vx0 + 2, vx1 - 2, y, CANVAS1_BLACK);
    }
    uint32_t n = cstr_len(t);
    unsigned len = canvas1_text_len(t, n);
    int max = (vx1 - vx0 - 1 - 40) / 8;        /* leave some stripes either side */
    if (max < 0) max = 0;
    if (len > (unsigned)max) len = (unsigned)max;
    if (len > 0) {
        int bw = 8 * (int)len + 12;
        int cx = (vx0 + vx1 + 1) / 2 - bw / 2;
        if (cx < vx0 + 2) cx = vx0 + 2;
        canvas1_fill(cv, cx, y0 + 1, cx + bw - 1, y0 + 15, CANVAS1_WHITE);
        canvas1_text(cv, cx + 6, y0 + 1, t, n, len, 1, 15, true);
    }
}

/* A tile: its frame and shadow, its interior white, and its title bar. */
LCDTERM_UTEXT static void draw_tile(const canvas1_t *cv, int x0, int y0, int x1, int y1, const char *t, bool active) {
    int vx0 = x0 < 0 ? 0 : x0;
    int vx1 = x1 >= (int)cv->w ? (int)cv->w - 1 : x1;
    if (vx0 >= vx1) return;

    canvas1_fill(cv, vx0, y0, vx1, y1, CANVAS1_WHITE);
    canvas1_hline(cv, vx0, vx1, y1, CANVAS1_BLACK);
    if (x0 >= 0) canvas1_vline(cv, x0, y0, y1, CANVAS1_BLACK);
    if (x1 < (int)cv->w) canvas1_vline(cv, x1, y0, y1, CANVAS1_BLACK);
    if (x1 + 1 < (int)cv->w) {
        canvas1_hline(cv, vx0 + 1, x1 + 1, y1 + 1, CANVAS1_BLACK);   /* the shadow */
        canvas1_vline(cv, x1 + 1, y0 + 1, y1 + 1, CANVAS1_BLACK);
    }
    draw_titlebar(cv, x0, y0, x1, t, active);
}

LCDTERM_UTEXT static bool has_text(const screen_t *scr) {
    if (scr->layout == SCREEN_LAYOUT_CANVAS) return false;
    if (scr->ribbon.count > 0) return ribbon_find_term(&scr->ribbon) >= 0;
    return true;
}

LCDTERM_UTEXT static bool has_canvas(const screen_t *scr) {
    if (scr->layout == SCREEN_LAYOUT_TEXT) return false;
    if (scr->ribbon.count > 0) return ribbon_find_canvas(&scr->ribbon) >= 0;
    return true;
}

LCDTERM_UTEXT static vtterm_t *active_vt(screen_t *scr) {
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
        scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
        int t = screen_find_term(scr, scr->ribbon.wins[scr->ribbon.active_idx].vterm_id);
        if (t >= 0) return &scr->terms[t].vt;
    }
    return &scr->terms[0].vt;
}

LCDTERM_UTEXT static void draw_text_title(screen_t *scr) {
    vtterm_t *avt = active_vt(scr);
    if (has_text(scr)) draw_titlebar(&scr->cv, scr->fx0, scr->fy0, scr->fx1, avt->title, true);
    scr->title_drawn = avt->title_seq;
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
    int c_idx = (scr->ribbon.count > 0) ? ribbon_find_canvas(&scr->ribbon) : -1;
    uint8_t cid = (c_idx >= 0) ? scr->ribbon.wins[c_idx].vterm_id : 0;
    canvas1_t *cc_back = (cid < SCREEN_MAX_CANVASES && scr->canvases[cid].in_use && scr->store) ? &scr->canvases[cid].backing : 0;

    unsigned rb = ((unsigned)cc->w + 7u) / 8u;
    if (cc_back && cc_back->fb) {
        for (unsigned y = 0; y < cc->h && y < cc_back->h; y++) {
            const uint8_t *src = cc_back->fb + (uint32_t)y * cc_back->stride;
            for (unsigned i = 0; i < rb; i++) d[i] = src[i];
            d += rb;
        }
    } else {
        if (cc->ox < 0 || cc->oy < 0 || (int)cc->ox + (int)cc->w > (int)scr->cv.w) return;
        unsigned s = (unsigned)cc->ox & 7u;
        for (unsigned y = 0; y < cc->h; y++) {
            const uint8_t *src = cc->fb + (uint32_t)(cc->oy + y) * cc->stride + (uint32_t)(cc->ox >> 3);
            for (unsigned i = 0; i < rb; i++)
                d[i] = s ? (uint8_t)((src[i] >> s) | (src[i + 1u] << (8u - s))) : src[i];
            d += rb;
        }
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
    if (cc->ox < 0 || cc->oy < 0 || (int)cc->ox + (int)cc->w > (int)scr->cv.w) return false;
    unsigned rb = ((unsigned)cc->w + 7u) / 8u, s = (unsigned)cc->ox & 7u;
    unsigned tail = (unsigned)cc->w & 7u;

    int c_idx = (scr->ribbon.count > 0) ? ribbon_find_canvas(&scr->ribbon) : -1;
    uint8_t cid = (c_idx >= 0) ? scr->ribbon.wins[c_idx].vterm_id : 0;
    canvas1_t *cc_back = (cid < SCREEN_MAX_CANVASES && scr->canvases[cid].in_use && scr->store) ? &scr->canvases[cid].backing : 0;

    for (unsigned y = 0; y < cc->h; y++) {
        uint8_t *dst = cc->fb + (uint32_t)(cc->oy + y) * cc->stride + (uint32_t)(cc->ox >> 3);
        if (cc_back && cc_back->fb && y < cc_back->h) {
            uint8_t *bdst = cc_back->fb + (uint32_t)y * cc_back->stride;
            for (unsigned i = 0; i < rb; i++) bdst[i] = d[i];
        }
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
    if (scr->store) {
        canvas1_init(&scr->c_backing, scr->store, scr->cv.w / 8u, scr->cv.w, scr->cv.h);
        canvas1_fill(&scr->c_backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
        for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
            canvas1_init(&scr->canvases[i].backing, scr->store + i * scr->store_slot, scr->cv.w / 8u, scr->cv.w, scr->cv.h);
            canvas1_fill(&scr->canvases[i].backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
        }
    }
}

LCDTERM_UTEXT static void blit_canvas_backing(screen_t *scr, uint8_t slot,
                                              int tile_screen_x, int tile_screen_y,
                                              int tile_w, int tile_h) {
    if (!scr->store || slot >= SCREEN_STORES) return;
    int W = tile_w;
    int H = tile_h;
    if (W <= 0 || H <= 0) return;

    int X = tile_screen_x;
    int Y = tile_screen_y;
    int cv_w = (int)scr->cv.w;
    int cv_h = (int)scr->cv.h;

    int x0 = (X < 0) ? -X : 0;
    int x1 = (X + W > cv_w) ? (cv_w - X - 1) : (W - 1);
    if (x0 > x1) return;

    int y0 = (Y < 0) ? -Y : 0;
    int y1 = (Y + H > cv_h) ? (cv_h - Y - 1) : (H - 1);
    if (y0 > y1) return;

    const uint8_t *src_fb = scr->store + (uint32_t)slot * scr->store_slot;
    uint32_t src_stride = scr->cv.w / 8u;
    int len = x1 - x0 + 1;

    for (int y = y0; y <= y1; y++) {
        int sy = Y + y;
        int sx0 = X + x0;
        const uint8_t *srow = src_fb + (uint32_t)y * src_stride;
        uint8_t *drow = scr->cv.fb + (uint32_t)sy * scr->cv.stride;
        int k = 0;

        /* Align destination to byte boundary */
        while (((sx0 + k) & 7) != 0 && k < len) {
            int tx = x0 + k;
            int sx = sx0 + k;
            uint8_t bit = (uint8_t)((srow[tx >> 3] >> (tx & 7)) & 1u);
            uint8_t mask = (uint8_t)(1u << (sx & 7));
            if (bit) drow[sx >> 3] |= mask;
            else drow[sx >> 3] &= (uint8_t)~mask;
            k++;
        }

        /* Fast byte transfers */
        int tx_shift = (x0 + k) & 7;
        if (tx_shift == 0) {
            while (k + 8 <= len) {
                drow[(sx0 + k) >> 3] = srow[(x0 + k) >> 3];
                k += 8;
            }
        } else {
            while (k + 8 <= len) {
                int sb = (x0 + k) >> 3;
                uint8_t v = (uint8_t)((srow[sb] >> tx_shift) | (srow[sb + 1] << (8 - tx_shift)));
                drow[(sx0 + k) >> 3] = v;
                k += 8;
            }
        }

        /* Trailing bits */
        while (k < len) {
            int tx = x0 + k;
            int sx = sx0 + k;
            uint8_t bit = (uint8_t)((srow[tx >> 3] >> (tx & 7)) & 1u);
            uint8_t mask = (uint8_t)(1u << (sx & 7));
            if (bit) drow[sx >> 3] |= mask;
            else drow[sx >> 3] &= (uint8_t)~mask;
            k++;
        }
    }
}

LCDTERM_UTEXT static void clear_ribbon_background(const screen_t *scr) {
    const canvas1_t *cv = &scr->cv;
    int w = (int)cv->w;
    int h = (int)cv->h;
    int tile_y0 = SCREEN_TILE_Y;
    unsigned rows = SCREEN_TEXT_ROWS(cv->h);
    int tile_y1 = SCREEN_TEXT_Y + 16 * (int)rows + 1;

    /* Top and bottom margin stripes */
    if (tile_y0 > SCREEN_MENU_H) {
        canvas1_fill(cv, 0, SCREEN_MENU_H, w - 1, tile_y0 - 1, CANVAS1_GREY);
    }
    if (tile_y1 + 2 < h) {
        canvas1_fill(cv, 0, tile_y1 + 2, w - 1, h - 1, CANVAS1_GREY);
    }

    if (scr->ribbon.count == 0) {
        canvas1_fill(cv, 0, tile_y0, w - 1, tile_y1 + 1, CANVAS1_GREY);
        return;
    }

    int32_t xv = scr->ribbon.x_view;
    int cur_x = 0;

    for (uint8_t i = 0; i < scr->ribbon.count; i++) {
        const ribbon_win_t *win = &scr->ribbon.wins[i];
        int32_t x0 = win->rx0 - xv;
        int32_t x1 = win->rx1 - xv;

        if (x1 + 1 < 0) continue;          /* completely to the left */
        if (x0 >= w) break;                /* completely to the right */

        int wx0 = (x0 < 0) ? 0 : (int)x0;
        int wx1 = (x1 + 1 >= w) ? (w - 1) : (int)(x1 + 1); /* +1 for shadow */

        if (wx0 > cur_x) {
            canvas1_fill(cv, cur_x, tile_y0, wx0 - 1, tile_y1 + 1, CANVAS1_GREY);
        }
        cur_x = wx1 + 1;
    }

    if (cur_x < w) {
        canvas1_fill(cv, cur_x, tile_y0, w - 1, tile_y1 + 1, CANVAS1_GREY);
    }
}

LCDTERM_UTEXT static bool place(screen_t *scr, unsigned layout);

/* Every pixel again. The canvas comes back blank, so its damage moves on --
 * unless `restore` and its layout's store has it (38.8). */
LCDTERM_UTEXT static void draw_all_from(screen_t *scr, bool restore) {
    const canvas1_t *cv = &scr->cv;
    (void)place(scr, scr->layout);
    clear_ribbon_background(scr);

    if (scr->ribbon.count > 0) {
        int32_t xv = scr->ribbon.x_view;
        int tile_y1 = SCREEN_TEXT_Y + 16 * (int)SCREEN_TEXT_ROWS(cv->h) + 1;

        for (uint8_t i = 0; i < scr->ribbon.count; i++) {
            const ribbon_win_t *w = &scr->ribbon.wins[i];
            int32_t x0 = w->rx0 - xv;
            int32_t x1 = w->rx1 - xv;
            if (x1 + 1 >= 0 && x0 < (int32_t)cv->w) {
                const char *title = w->title;
                if (w->type == RIBBON_WIN_CANVAS) {
                    uint8_t cid = w->vterm_id;
                    if (cid < SCREEN_MAX_CANVASES && scr->canvases[cid].title[0]) {
                        title = scr->canvases[cid].title;
                    } else if (scr->ctitle[0]) {
                        title = scr->ctitle;
                    }
                } else if (w->type == RIBBON_WIN_TERM) {
                    int t = screen_find_term(scr, w->vterm_id);
                    if (t >= 0 && scr->terms[t].vt.title[0]) {
                        title = scr->terms[t].vt.title;
                    }
                }
                bool active = (i == scr->ribbon.active_idx);
                draw_tile(cv, (int)x0, SCREEN_TILE_Y, (int)x1, tile_y1, title, active);
            }
        }

        /* Blit backing stores for all visible canvas windows */
        for (uint8_t i = 0; i < scr->ribbon.count; i++) {
            const ribbon_win_t *w = &scr->ribbon.wins[i];
            if (w->type == RIBBON_WIN_CANVAS) {
                uint8_t cid = w->vterm_id;
                int32_t cx0 = w->rx0 - xv;
                int32_t cx1 = w->rx1 - xv;
                if (cx1 >= 0 && cx0 < (int32_t)cv->w) {
                    int cw_w = (int)(w->rx1 - w->rx0 - 1);
                    int cw_h = (int)(tile_y1 - (SCREEN_TILE_Y + 17));
                    if (cw_w > 0 && cw_h > 0) {
                        bool restored = false;
                        if (restore) {
                            restored = store_restore(scr);
                        }
                        if (restored) {
                            blit_canvas_backing(scr, cid, (int)cx0 + 1, SCREEN_TILE_Y + 17, cw_w, cw_h);
                        } else if (!restore && cid < SCREEN_MAX_CANVASES && scr->canvases[cid].in_use && scr->store) {
                            blit_canvas_backing(scr, cid, (int)cx0 + 1, SCREEN_TILE_Y + 17, cw_w, cw_h);
                        } else {
                            canvas1_fill(cv, (int)cx0 + 1, SCREEN_TILE_Y + 17, (int)cx1 - 1, tile_y1 - 1, CANVAS1_WHITE);
                            if (cid < SCREEN_MAX_CANVASES && scr->canvases[cid].in_use && scr->store) {
                                canvas1_fill(&scr->canvases[cid].backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
                            }
                            scr->damage++;
                        }
                    }
                }
            }
        }
    } else {
        if (has_text(scr)) draw_tile(cv, scr->fx0, scr->fy0, scr->fx1, scr->fy1, scr->vt.title, true);
        if (has_canvas(scr)) {
            draw_tile(cv, scr->cx0, scr->cy0, scr->cx1, scr->cy1, scr->ctitle, false);
            if (!restore || !store_restore(scr)) {
                canvas1_fill(cv, scr->cx0 + 1, scr->cy0 + 17, scr->cx1 - 1, scr->cy1 - 1, CANVAS1_WHITE);
                scr->damage++;
            }
        }
    }
    vtterm_t *avt = active_vt(scr);
    scr->title_drawn = avt->title_seq;
    draw_menu(scr);
    if (scr->ribbon.count > 0) {
        for (uint32_t t = 0; t < SCREEN_MAX_TERMS; t++) {
            if (scr->terms[t].in_use && !scr->terms[t].vt.hidden) {
                vtterm_repaint(&scr->terms[t].vt);
            }
        }
    } else {
        vtterm_repaint(&scr->vt);
    }
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
    int32_t xv = scr->ribbon.x_view;
    scr->layout = (uint8_t)layout;

    /* Position all terminal windows in the ribbon */
    for (uint8_t i = 0; i < scr->ribbon.count; i++) {
        ribbon_win_t *w = &scr->ribbon.wins[i];
        if (w->type != RIBBON_WIN_TERM) continue;

        int t = screen_find_term(scr, w->vterm_id);
        if (t < 0) t = screen_alloc_term(scr, w->vterm_id);
        if (t < 0) continue;

        int32_t x0 = w->rx0 - xv;
        int32_t x1 = w->rx1 - xv;
        bool visible = has_text(scr) && (x1 >= 0 && x0 < (int32_t)scr->cv.w);
        if (!visible) {
            vtterm_set_hidden(&scr->terms[t].vt, true);
            continue;
        }

        int col_off = 0;
        int c0 = 1;
        int win_c = (int)w->cols;

        if (x0 >= 4) {
            c0 = (int)((x0 + 4) / 8);
            col_off = 0;
            int max_c = C - 1 - c0;
            if (win_c > max_c) win_c = max_c;
        } else {
            c0 = 1;
            col_off = (int)((4 - x0) / 8);
            if (col_off < 0) col_off = 0;
            if (col_off >= (int)w->cols) {
                vtterm_set_hidden(&scr->terms[t].vt, true);
                continue;
            }
            win_c = (int)w->cols - col_off;
            int max_c = C - 1 - c0;
            if (win_c > max_c) win_c = max_c;
        }
        if (win_c < 1) win_c = 1;

        vtterm_set_logical_cols(&scr->terms[t].vt, w->cols);
        vtterm_set_col_offset(&scr->terms[t].vt, (uint16_t)col_off);

        fbtext_t text;
        fbtext_init(&text, scr->cv.fb + SCREEN_TEXT_Y * scr->cv.stride + (uint32_t)c0, scr->cv.stride,
                    (unsigned)win_c, rows);
        text.xbyte = (uint16_t)c0;
        text.whole_rows = (win_c == scr->full_cols && c0 == 1);
        vtterm_resize(&scr->terms[t].vt, &text);
        vtterm_set_hidden(&scr->terms[t].vt, false);

        if (i == scr->ribbon.active_idx) {
            scr->fx0 = (int16_t)x0;
            scr->fx1 = (int16_t)(x0 + 8 * w->cols + 6);
            scr->fy0 = SCREEN_TILE_Y;
            scr->fy1 = (int16_t)y1;
        }
    }

    /* Position canvas if present in ribbon */
    int c_idx = ribbon_find_canvas(&scr->ribbon);
    if (c_idx >= 0) {
        const ribbon_win_t *cw = &scr->ribbon.wins[c_idx];
        int32_t cx0 = cw->rx0 - xv;
        int32_t cx1 = cw->rx1 - xv;
        scr->cx0 = (int16_t)cx0;
        scr->cx1 = (int16_t)cx1;
        scr->cy0 = SCREEN_TILE_Y;
        scr->cy1 = (int16_t)y1;
        int cw_w = (int)(cw->rx1 - cw->rx0 - 1);
        int cw_h = (int)(scr->cy1 - scr->cy0 - 17);
        if (cw_w < 0) cw_w = 0;
        if (cw_h < 0) cw_h = 0;
        if (cw_w == 0 || cw_h == 0) {
            canvas1_window(&scr->cc, &scr->cv, 0, 0, 0, 0);
        } else {
            canvas1_window(&scr->cc, &scr->cv, (int)scr->cx0 + 1, (int)scr->cy0 + 17,
                           (unsigned)cw_w, (unsigned)cw_h);
        }
    } else {
        scr->cx0 = scr->cx1 = 0;
        scr->cy0 = scr->cy1 = 0;
        canvas1_window(&scr->cc, &scr->cv, 0, 0, 0, 0);
    }

    scr->vt.text.fb = scr->terms[0].vt.text.fb;
    scr->vt.text.stride = scr->terms[0].vt.text.stride;
    scr->vt.text.cols = scr->terms[0].vt.text.cols;
    scr->vt.text.rows = scr->terms[0].vt.text.rows;
    scr->vt.text.xbyte = scr->terms[0].vt.text.xbyte;
    scr->vt.text.whole_rows = scr->terms[0].vt.text.whole_rows;
    scr->vt.hidden = scr->terms[0].vt.hidden;

    return true;
}

void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h) {
    canvas1_init(&scr->cv, fb, stride, w, h);
    canvas1_init(&scr->c_backing, 0, 0, 0, 0);
    scr->full_cols = (uint8_t)SCREEN_TEXT_COLS(w);
    scr->damage = 0;
    scr->store = 0;
    scr->store_ok = 0;
    scr->store_slot = 0;
    scr->draw_gen = 0;
    scr->redrawing = 0;
    scr->locked = 0;
    scr->swapped = 0;
    scr->status_flags = 0;
    scr->store_gen[0] = scr->store_gen[1] = scr->store_gen[2] = scr->store_gen[3] = 0;
    const char *name = "LugalOS";
    unsigned i = 0;
    for (; name[i] && i < SCREEN_NAME_MAX - 1u; i++) scr->name[i] = name[i];
    scr->name[i] = '\0';
    const char *ct = "Canvas";
    for (i = 0; ct[i] && i < SCREEN_CTITLE_MAX - 1u; i++) scr->ctitle[i] = ct[i];
    scr->ctitle[i] = '\0';
    scr->right[0] = '\0';

    for (uint32_t c = 0; c < SCREEN_MAX_CANVASES; c++) {
        canvas1_init(&scr->canvases[c].backing, 0, 0, 0, 0);
        scr->canvases[c].vterm_id = 0;
        scr->canvases[c].in_use = false;
        scr->canvases[c].locked = false;
        scr->canvases[c].title[0] = '\0';
    }

    uint32_t term_cells = (uint32_t)SCREEN_TEXT_COLS(w) * (uint32_t)SCREEN_TEXT_ROWS(h);
    for (uint32_t t = 0; t < SCREEN_MAX_TERMS; t++) {
        uint16_t *t_shadow = scr->shadow + t * term_cells;
        for (uint32_t j = 0; j < term_cells; j++) t_shadow[j] = 0;
        fbtext_t t_text;
        fbtext_init(&t_text, (uint8_t *)fb + SCREEN_TEXT_Y * stride + SCREEN_TEXT_X / 8, stride,
                    SCREEN_TEXT_COLS(w), SCREEN_TEXT_ROWS(h));
        vtterm_init(&scr->terms[t].vt, &t_text, t_shadow);
        scr->terms[t].in_use = (t == 0);
        scr->terms[t].vterm_id = (uint8_t)t;
    }
    vtterm_set_title(&scr->terms[0].vt, name, 7);

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

LCDTERM_UTEXT void screen_write_vterm(screen_t *scr, uint8_t vterm_id, const char *s, uint32_t n) {
    int t = screen_find_term(scr, vterm_id);
    if (t < 0) t = screen_alloc_term(scr, vterm_id);
    if (t < 0) t = 0;

    for (uint32_t i = 0; i < n; i++) {
        vtterm_putc(&scr->terms[t].vt, s[i]);
    }

    vtterm_t *avt = active_vt(scr);
    if (avt && avt->title_seq != scr->title_drawn) draw_text_title(scr);
}

LCDTERM_UTEXT void screen_write(screen_t *scr, const char *s, uint32_t n) {
    uint8_t vid = 0;
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
        scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
        vid = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    }
    screen_write_vterm(scr, vid, s, n);
}

LCDTERM_UTEXT void screen_set_title(screen_t *scr, const char *s, uint32_t n) {
    vtterm_t *avt = active_vt(scr);
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
        ribbon_set_title(&scr->ribbon, scr->ribbon.active_idx, s);
    }
    vtterm_set_title(avt, s, n);
    vtterm_set_title(&scr->vt, s, n);
    if (avt->title_seq != scr->title_drawn) draw_text_title(scr);
}

LCDTERM_UTEXT void screen_set_right(screen_t *scr, uint8_t flags, const char *s, uint32_t n) {
    scr->status_flags = flags;
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

LCDTERM_UTEXT bool screen_set_layout_vterm(screen_t *scr, uint8_t vid, unsigned layout) {
    int cid_check = screen_find_canvas_by_vterm(scr, vid);
    if (cid_check >= 0 && scr->canvases[cid_check].locked && layout != scr->layout && layout != SCREEN_LAYOUT_TEXT) return false;

    if (has_canvas(scr)) store_save(scr);

    if (layout == SCREEN_LAYOUT_TEXT) {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
            scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_CANVAS) {
            cid = (int)scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
        }
        if (cid < 0) {
            int c_any = ribbon_find_canvas(&scr->ribbon);
            if (c_any >= 0) cid = (int)scr->ribbon.wins[c_any].vterm_id;
        }

        if (cid >= 0) {
            int c_idx = ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid);
            if (c_idx >= 0) {
                ribbon_remove(&scr->ribbon, (uint8_t)c_idx);
                screen_free_canvas(scr, (uint8_t)cid);
                if (scr->ribbon.count == 1 && scr->ribbon.wins[0].type == RIBBON_WIN_TERM) {
                    scr->ribbon.wins[0].cols = scr->full_cols;
                }
                ribbon_layout(&scr->ribbon);
                scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
                scr->ribbon.x_view = scr->ribbon.x_target;
            }
        }
        if (ribbon_find_canvas(&scr->ribbon) < 0) {
            scr->layout = SCREEN_LAYOUT_TEXT;
            scr->locked = 0;
            scr->redrawing = 0;
        }
        (void)place(scr, scr->layout);
        draw_all_from(scr, true);
        return true;
    }

    if (layout != SCREEN_LAYOUT_SPLIT_WIDE && layout != SCREEN_LAYOUT_SPLIT_HALF &&
        layout != SCREEN_LAYOUT_SPLIT_NARROW && layout != SCREEN_LAYOUT_CANVAS) {
        return false;
    }

    int C = (int)(scr->cv.w / 8u);
    uint8_t text_cols = 0;
    if (layout == SCREEN_LAYOUT_SPLIT_WIDE) {
        text_cols = 38;
    } else if (layout == SCREEN_LAYOUT_SPLIT_HALF) {
        text_cols = 48;
    } else if (layout != SCREEN_LAYOUT_CANVAS) {
        text_cols = 64;
    }
    if (layout != SCREEN_LAYOUT_CANVAS) {
        if (8 * (C - 1 - (int)text_cols) - 14 < 4 + 24) return false;   /* no room for a canvas */
    }

    uint8_t canvas_cols = (layout == SCREEN_LAYOUT_CANVAS) ? scr->full_cols : (uint8_t)(scr->full_cols - text_cols - 2u);

    int cid = screen_find_canvas_by_vterm(scr, vid);
    int c_win = (cid >= 0) ? ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid) : -1;
    int t_win = ribbon_find_vterm(&scr->ribbon, vid);

    if (c_win >= 0) {
        /* Canvas already in ribbon for this vterm: update column widths */
        scr->ribbon.wins[c_win].cols = canvas_cols;
        if (t_win >= 0 && text_cols > 0) {
            scr->ribbon.wins[t_win].cols = text_cols;
        }
        if (layout == SCREEN_LAYOUT_CANVAS) {
            scr->ribbon.active_idx = (uint8_t)c_win;
        } else if (t_win >= 0) {
            scr->ribbon.active_idx = (uint8_t)t_win;
        }
        ribbon_layout(&scr->ribbon);
        scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
        scr->ribbon.x_view = scr->ribbon.x_target;
    } else {
        /* Allocate a canvas slot */
        cid = screen_alloc_canvas(scr, vid);
        if (cid < 0) return false; /* out of canvas slots */

        if (t_win >= 0 && text_cols > 0) {
            scr->ribbon.wins[t_win].cols = text_cols;
        } else if (t_win < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
                   scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM && text_cols > 0) {
            scr->ribbon.wins[scr->ribbon.active_idx].cols = text_cols;
            t_win = (int)scr->ribbon.active_idx;
        }

        uint8_t act = (t_win >= 0) ? (uint8_t)t_win : scr->ribbon.active_idx;
        uint8_t insert_idx;
        if (scr->swapped) {
            insert_idx = act;
        } else {
            insert_idx = (uint8_t)(act + 1u);
        }

        char def_ctitle[8];
        def_ctitle[0] = 'C'; def_ctitle[1] = 'a'; def_ctitle[2] = 'n';
        def_ctitle[3] = 'v'; def_ctitle[4] = 'a'; def_ctitle[5] = 's';
        def_ctitle[6] = '\0';
        const char *ctitle = scr->canvases[cid].title[0] ? scr->canvases[cid].title : def_ctitle;
        int idx = ribbon_insert(&scr->ribbon, insert_idx, RIBBON_WIN_CANVAS, (uint8_t)cid, canvas_cols, ctitle);
        if (idx >= 0) {
            if (layout == SCREEN_LAYOUT_CANVAS) {
                scr->ribbon.active_idx = (uint8_t)idx;
            } else if (insert_idx <= act) {
                scr->ribbon.active_idx = (uint8_t)(act + 1u);
            }
            ribbon_layout(&scr->ribbon);
            scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
            scr->ribbon.x_view = scr->ribbon.x_target;
        } else {
            screen_free_canvas(scr, (uint8_t)cid);
            return false;
        }
    }

    scr->layout = (uint8_t)layout;
    scr->redrawing = 0;
    (void)place(scr, layout);
    draw_all_from(scr, true);
    return true;
}

LCDTERM_UTEXT bool screen_set_layout(screen_t *scr, unsigned layout) {
    uint8_t vid = 0;
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
        scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
        vid = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    }
    return screen_set_layout_vterm(scr, vid, layout);
}

LCDTERM_UTEXT void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows) {
    const vtterm_t *avt = ((screen_t *)scr)->ribbon.count > 0 ? active_vt((screen_t *)scr) : &scr->vt;
    *cols = avt->text.cols;
    *rows = avt->text.rows;
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

LCDTERM_UTEXT void screen_canvas_vterm(screen_t *scr, uint8_t vid, const uint8_t *req, uint32_t n, uint8_t *reply) {
    uint8_t status = 0, pixel = 0;
    uint8_t op = n ? req[0] : 0;
    if (op == 'L') {
        status = (n >= 2 && screen_set_layout_vterm(scr, vid, req[1])) ? 0 : 1;
    } else if (op == 'T') {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
            scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_CANVAS) {
            cid = (int)scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
        }
        if (cid < 0) {
            int c_any = ribbon_find_canvas(&scr->ribbon);
            if (c_any >= 0) cid = (int)scr->ribbon.wins[c_any].vterm_id;
        }
        if (cid < 0) cid = 0;

        uint32_t len = n - 1u;
        if (len > SCREEN_CTITLE_MAX - 1u) {
            len = SCREEN_CTITLE_MAX - 1u;
            while (len > 0 && (req[1u + len] & 0xc0u) == 0x80u) len--;
        }
        for (uint32_t i = 0; i < len; i++) {
            scr->canvases[cid].title[i] = (char)req[1u + i];
            scr->ctitle[i] = (char)req[1u + i];
        }
        scr->canvases[cid].title[len] = '\0';
        scr->ctitle[len] = '\0';

        int c_win = ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid);
        if (c_win >= 0) {
            ribbon_set_title(&scr->ribbon, (uint8_t)c_win, scr->canvases[cid].title);
            int32_t cx0 = scr->ribbon.wins[c_win].rx0 - scr->ribbon.x_view;
            int32_t cx1 = scr->ribbon.wins[c_win].rx1 - scr->ribbon.x_view;
            if (cx1 >= 0 && cx0 < (int32_t)scr->cv.w) {
                bool active = (c_win == (int)scr->ribbon.active_idx);
                draw_titlebar(&scr->cv, (int)cx0, SCREEN_TILE_Y, (int)cx1, scr->canvases[cid].title, active);
            }
        }
        status = 0;
    } else if (op == 'w') {
        int dir = n >= 2 ? (int)(int8_t)req[1] : 0;
        if (ribbon_resize_active(&scr->ribbon, dir > 0)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'X') {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid >= 0 && scr->canvases[cid].locked) {
            status = 1;
        } else {
            int c_win = (cid >= 0) ? ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid) : -1;
            int t_win = ribbon_find_vterm(&scr->ribbon, vid);
            if (t_win < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
                scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
                t_win = (int)scr->ribbon.active_idx;
            }

            if (c_win < 0) {
                /* No canvas for this terminal: open split canvas! */
                status = screen_set_layout_vterm(scr, vid, SCREEN_LAYOUT_SPLIT_HALF) ? 0 : 1;
            } else if (t_win >= 0 && c_win >= 0) {
                /* Swap position of terminal and canvas */
                store_save(scr);
                ribbon_swap(&scr->ribbon, (uint8_t)t_win, (uint8_t)c_win);
                scr->swapped = (uint8_t)!scr->swapped;
                scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
                scr->ribbon.x_view = scr->ribbon.x_target;
                (void)place(scr, scr->layout);
                draw_all_from(scr, true);
                status = 0;
            } else {
                status = 1;
            }
        }
    } else if (op == 'N') {
        /* Phase 44: Insert a new window in the ribbon (Cmd+Enter) */
        uint8_t n_vid = (n >= 2) ? req[1] : 0;
        uint8_t cols = (n >= 3 && req[2]) ? req[2] : 48;
        const char *title = (n > 3 && req[3]) ? (const char *)(req + 3) : 0;
        char def_title[16];
        if (!title) {
            def_title[0] = 'l'; def_title[1] = 's'; def_title[2] = 'h';
            def_title[3] = ' '; def_title[4] = '[';
            def_title[5] = (char)('0' + (n_vid % 10));
            def_title[6] = ']'; def_title[7] = '\0';
            title = def_title;
        }
        if (scr->ribbon.count == 1 && scr->ribbon.wins[0].cols > 48) {
            scr->ribbon.wins[0].cols = 48;
        }
        int idx = ribbon_insert(&scr->ribbon, (uint8_t)(scr->ribbon.active_idx + 1u),
                                RIBBON_WIN_TERM, n_vid, cols, title);
        if (idx >= 0) {
            int t = screen_alloc_term(scr, n_vid);
            if (t >= 0 && title) {
                uint32_t tlen = cstr_len(title);
                vtterm_set_title(&scr->terms[t].vt, title, tlen);
            }
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '<') {
        /* Phase 44: Focus left window (Cmd+Left) */
        if (ribbon_focus_step(&scr->ribbon, -1)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '>') {
        /* Phase 44: Focus right window (Cmd+Right) */
        if (ribbon_focus_step(&scr->ribbon, 1)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '[') {
        /* Phase 44: Move active window left (Cmd+Ctrl+Left) */
        if (ribbon_move(&scr->ribbon, scr->ribbon.active_idx, -1)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == ']') {
        /* Phase 44: Move active window right (Cmd+Ctrl+Right) */
        if (ribbon_move(&scr->ribbon, scr->ribbon.active_idx, 1)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'J') {
        /* Phase 44: Jump directly to window N (Cmd+1..9) */
        uint8_t target = (n >= 2) ? req[1] : 0;
        if (ribbon_focus(&scr->ribbon, target)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'V') {
        /* Phase 44: Focus window by vterm ID */
        uint8_t f_vid = (n >= 2) ? req[1] : 0;
        int idx = ribbon_find_vterm(&scr->ribbon, f_vid);
        if (idx >= 0 && ribbon_focus(&scr->ribbon, (uint8_t)idx)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'R') {
        /* Phase 44: Query ribbon status */
        status = 0;
        reply[1] = scr->ribbon.count;
        reply[2] = scr->ribbon.active_idx;
        reply[3] = (uint8_t)(scr->ribbon.x_view & 0xff);
        reply[4] = (uint8_t)((scr->ribbon.x_view >> 8) & 0xff);
        reply[5] = (uint8_t)(scr->ribbon.x_target & 0xff);
        reply[6] = (uint8_t)((scr->ribbon.x_target >> 8) & 0xff);
        uint32_t off = 7;
        for (uint8_t i = 0; i < scr->ribbon.count && off + 7 <= SCREEN_REPLY_LEN; i++) {
            const ribbon_win_t *w = &scr->ribbon.wins[i];
            reply[off++] = w->type;
            reply[off++] = w->vterm_id;
            reply[off++] = w->cols;
            reply[off++] = (uint8_t)(w->rx0 & 0xff);
            reply[off++] = (uint8_t)((w->rx0 >> 8) & 0xff);
            reply[off++] = (uint8_t)(w->rx1 & 0xff);
            reply[off++] = (uint8_t)((w->rx1 >> 8) & 0xff);
        }
    } else if (op == 'C') {
        /* Phase 44: Close active window (Cmd+W) */
        uint8_t act = scr->ribbon.active_idx;
        if (act < scr->ribbon.count) {
            uint8_t closed_type = scr->ribbon.wins[act].type;
            uint8_t closed_id = scr->ribbon.wins[act].vterm_id;
            if (scr->ribbon.count > 1 && ribbon_remove(&scr->ribbon, act)) {
                if (closed_type == RIBBON_WIN_TERM && closed_id > 0) {
                    screen_free_term(scr, closed_id);
                } else if (closed_type == RIBBON_WIN_CANVAS) {
                    screen_free_canvas(scr, closed_id);
                }
                if (ribbon_find_canvas(&scr->ribbon) < 0) {
                    scr->layout = SCREEN_LAYOUT_TEXT;
                }
                if (scr->ribbon.count == 1 && scr->ribbon.wins[0].type == RIBBON_WIN_TERM) {
                    scr->ribbon.wins[0].cols = scr->full_cols;
                }
                ribbon_layout(&scr->ribbon);
                scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
                scr->ribbon.x_view = scr->ribbon.x_target;
                (void)place(scr, scr->layout);
                draw_all_from(scr, true);
                status = 0;
                reply[10] = closed_type;
                reply[11] = closed_id;
            } else {
                status = 1;
            }
        } else {
            status = 1;
        }
    } else if (op == 'K') {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid >= 0) {
            scr->canvases[cid].locked = (n >= 2 && req[1]) ? true : false;
        }
        scr->locked = (n >= 2 && req[1]) ? 1 : 0;
    } else if (op == 'Z') {
        scr->store_ok = 0;              /* 38.8 */
        scr->redrawing = 0;
    } else if (op == 'D') {
        scr->redrawing = (n >= 2 && req[1]) ? 1 : 0;
    } else if (op == 'S' || op == 0) {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
            scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_CANVAS) {
            cid = (int)scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
        }
        if (cid < 0) {
            int c_any = ribbon_find_canvas(&scr->ribbon);
            if (c_any >= 0) cid = (int)scr->ribbon.wins[c_any].vterm_id;
        }
        int cw_w = 0, cw_h = 0;
        if (cid >= 0 && scr->canvases[cid].in_use) {
            int c_win = ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid);
            if (c_win >= 0) {
                const ribbon_win_t *w = &scr->ribbon.wins[c_win];
                cw_w = (int)(w->rx1 - w->rx0 - 1);
                cw_h = (int)(SCREEN_TEXT_Y + 16 * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
            }
        }
        reply[2] = (uint8_t)cw_w; reply[3] = (uint8_t)(cw_w >> 8);
        reply[4] = (uint8_t)cw_h; reply[5] = (uint8_t)(cw_h >> 8);
    } else if (!has_canvas(scr)) {
        status = 1;
    } else {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
            scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_CANVAS) {
            cid = (int)scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
        }
        if (cid < 0) {
            int c_any = ribbon_find_canvas(&scr->ribbon);
            if (c_any >= 0) cid = (int)scr->ribbon.wins[c_any].vterm_id;
        }

        if (cid < 0 || cid >= (int)SCREEN_MAX_CANVASES || !scr->canvases[cid].in_use) {
            status = 1;
        } else {
            canvas1_t *cc_back = scr->store ? &scr->canvases[cid].backing : 0;
            canvas1_t cc_screen;
            canvas1_t *cc_scr = 0;
            int cw_w = 0, cw_h = 0;

            int c_win = ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid);
            if (c_win >= 0) {
                const ribbon_win_t *w = &scr->ribbon.wins[c_win];
                int32_t cx0 = w->rx0 - scr->ribbon.x_view;
                int32_t cx1 = w->rx1 - scr->ribbon.x_view;
                cw_w = (int)(w->rx1 - w->rx0 - 1);
                cw_h = (int)(SCREEN_TEXT_Y + 16 * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
                if (cx1 >= 0 && cx0 < (int32_t)scr->cv.w && cw_w > 0 && cw_h > 0) {
                    canvas1_window(&cc_screen, &scr->cv, (int)cx0 + 1, SCREEN_TILE_Y + 17,
                                   (unsigned)cw_w, (unsigned)cw_h);
                    cc_scr = &cc_screen;
                }
            }
            if (cw_w <= 0) cw_w = (int)scr->cc.w;
            if (cw_h <= 0) cw_h = (int)scr->cc.h;

            int a = arg16(req, n, 0), b = arg16(req, n, 1), c = arg16(req, n, 2);
            int d = arg16(req, n, 3), e = arg16(req, n, 4);
            if (op == 'F') {
                if (cc_back) canvas1_fill(cc_back, 0, 0, cw_w - 1, cw_h - 1, colour(a));
                if (cc_scr) canvas1_fill(cc_scr, 0, 0, cw_w - 1, cw_h - 1, colour(a));
            } else if (op == 'p') {
                if (cc_back) canvas1_fill(cc_back, a, b, a, b, colour(c));
                if (cc_scr) canvas1_fill(cc_scr, a, b, a, b, colour(c));
            } else if (op == 'l') {
                if (cc_back) canvas1_line(cc_back, a, b, c, d, colour(e));
                if (cc_scr) canvas1_line(cc_scr, a, b, c, d, colour(e));
            } else if (op == 'r') {
                if (c > 0 && d > 0) {
                    if (cc_back) canvas1_fill(cc_back, a, b, a + c - 1, b + d - 1, colour(e));
                    if (cc_scr) canvas1_fill(cc_scr, a, b, a + c - 1, b + d - 1, colour(e));
                }
            } else if (op == 'o') {
                if (c > 0 && d > 0) {
                    unsigned k = colour(e);
                    if (cc_back) {
                        canvas1_hline(cc_back, a, a + c - 1, b, k);
                        canvas1_hline(cc_back, a, a + c - 1, b + d - 1, k);
                        canvas1_vline(cc_back, a, b, b + d - 1, k);
                        canvas1_vline(cc_back, a + c - 1, b, b + d - 1, k);
                    }
                    if (cc_scr) {
                        canvas1_hline(cc_scr, a, a + c - 1, b, k);
                        canvas1_hline(cc_scr, a, a + c - 1, b + d - 1, k);
                        canvas1_vline(cc_scr, a, b, b + d - 1, k);
                        canvas1_vline(cc_scr, a + c - 1, b, b + d - 1, k);
                    }
                }
            } else if (op == 'c') {
                if (cc_back) canvas1_circle(cc_back, a, b, c, colour(d));
                if (cc_scr) canvas1_circle(cc_scr, a, b, c, colour(d));
            } else if (op == 'i') {
                if (c > 0 && d > 0) {
                    if (cc_back) canvas1_invert(cc_back, a, b, a + c - 1, b + d - 1);
                    if (cc_scr) canvas1_invert(cc_scr, a, b, a + c - 1, b + d - 1);
                }
            } else if (op == 't') {
                if (n >= 6) {
                    const char *s = (const char *)req + 6;
                    uint32_t len = n - 6u;
                    unsigned tlen = canvas1_text_len(s, len);
                    if (cc_back) canvas1_text(cc_back, a, b, s, len, tlen, 0, FONT8X16_H, req[5] != 0);
                    if (cc_scr) canvas1_text(cc_scr, a, b, s, len, tlen, 0, FONT8X16_H, req[5] != 0);
                }
            } else if (op == 'b') {
                unsigned cnt = (unsigned)(c < 0 ? 0 : c);
                if (n >= 9u + (cnt + 7u) / 8u) {
                    unsigned sc = (unsigned)(d < 1 ? 1 : d);
                    if (cc_back) canvas1_row(cc_back, a, b, req + 9, cnt, sc);
                    if (cc_scr) canvas1_row(cc_scr, a, b, req + 9, cnt, sc);
                } else {
                    status = 1;
                }
            } else if (op == 'g') {
                if (cc_back) pixel = (uint8_t)canvas1_get(cc_back, a, b);
                else if (cc_scr) pixel = (uint8_t)canvas1_get(cc_scr, a, b);
            } else {
                status = 1;
            }

            if (op != 'g' && status == 0 && !scr->redrawing) scr->draw_gen++;

            reply[2] = (uint8_t)cw_w; reply[3] = (uint8_t)(cw_w >> 8);
            reply[4] = (uint8_t)cw_h; reply[5] = (uint8_t)(cw_h >> 8);
        }
    }
    if (op == 'R') return;
    reply[0] = status;
    if (op != 'g' && scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
        reply[1] = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    } else {
        reply[1] = pixel;
    }
    reply[6] = (uint8_t)scr->damage; reply[7] = (uint8_t)(scr->damage >> 8);
    reply[8] = scr->layout;
    reply[9] = scr->swapped;
}

LCDTERM_UTEXT void screen_canvas(screen_t *scr, const uint8_t *req, uint32_t n, uint8_t *reply) {
    uint8_t vid = 0;
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
        scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
        vid = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    }
    screen_canvas_vterm(scr, vid, req, n, reply);
}
