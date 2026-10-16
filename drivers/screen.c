/*
 * The screen: a menu bar, the desktop, and the terminal and a canvas in
 * framed tiles -- 37.1/37.1a/37.3b, 
 * §1. See drivers/screen.h for the geometry and the canvas protocol.
 *
 * Everything but screen_init() runs in the U-mode `lcdterm` task on the
 * RP2350-LCD-7, so it is LCDTERM_UTEXT, uses no libc and no string literals
 * (a literal is .rodata, outside the task's domain). screen_init() runs once,
 * in the kernel, before the task exists.
 */

#include <stddef.h>
#include "drivers/screen.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"

#if defined(CONFIG_BOARD_RP2350)
#define CANVAS_SUPPORT_RGB565 0
#else
#define CANVAS_SUPPORT_RGB565 1
#endif

#if CANVAS_SUPPORT_RGB565
#include "kernel/palloc.h"
static void *canvas_alloc_psram(uint32_t pages) {
    return palloc_pages_bulk(pages);
}
static void canvas_free_psram(void *ptr, uint32_t pages) {
    palloc_free(ptr, pages);
}

static void canvas565_fill(uint16_t *fb, uint32_t stride, int x0, int y0,
                           int x1, int y1, uint16_t c) {
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb + (size_t)y * stride;
        for (int x = x0; x <= x1; x++) row[x] = c;
    }
}

static void canvas565_pixel(uint16_t *fb, uint32_t stride, int w, int h,
                            int x, int y, uint16_t c) {
    if (x >= 0 && x < w && y >= 0 && y < h) {
        fb[(size_t)y * stride + (size_t)x] = c;
    }
}

static void canvas565_line(uint16_t *fb, uint32_t stride, int w, int h,
                           int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            fb[(size_t)y0 * stride + (size_t)x0] = c;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void canvas565_rect(uint16_t *fb, uint32_t stride, int w, int h,
                           int x, int y, int rw, int rh, uint16_t c) {
    if (rw <= 0 || rh <= 0) return;
    int x1 = x + rw - 1;
    int y1 = y + rh - 1;
    for (int ix = x; ix <= x1; ix++) {
        if (ix >= 0 && ix < w) {
            if (y >= 0 && y < h) fb[(size_t)y * stride + (size_t)ix] = c;
            if (y1 >= 0 && y1 < h) fb[(size_t)y1 * stride + (size_t)ix] = c;
        }
    }
    for (int iy = y; iy <= y1; iy++) {
        if (iy >= 0 && iy < h) {
            if (x >= 0 && x < w) fb[(size_t)iy * stride + (size_t)x] = c;
            if (x1 >= 0 && x1 < w) fb[(size_t)iy * stride + (size_t)x1] = c;
        }
    }
}

static void canvas565_circle(uint16_t *fb, uint32_t stride, int w, int h,
                             int cx, int cy, int r, uint16_t c) {
    if (r < 0) return;
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (x <= y) {
        int pts[8][2] = {
            {cx + x, cy + y}, {cx - x, cy + y}, {cx + x, cy - y}, {cx - x, cy - y},
            {cx + y, cy + x}, {cx - y, cy + x}, {cx + y, cy - x}, {cx - y, cy - x}
        };
        for (int i = 0; i < 8; i++) {
            int px = pts[i][0], py = pts[i][1];
            if (px >= 0 && px < w && py >= 0 && py < h) {
                fb[(size_t)py * stride + (size_t)px] = c;
            }
        }
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

static void canvas565_invert(uint16_t *fb, uint32_t stride, int w, int h,
                             int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= h) y1 = h - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int y = y0; y <= y1; y++) {
        uint16_t *row = fb + (size_t)y * stride;
        for (int x = x0; x <= x1; x++) row[x] = (uint16_t)~row[x];
    }
}

static void canvas565_text(uint16_t *fb, uint32_t stride, int w, int h,
                           int x, int y, const char *s, uint32_t len,
                           uint16_t fg, uint16_t bg, bool transparent, bool bold) {
    (void)bg;
    for (uint32_t i = 0; i < len; i++) {
        char ch = s[i];
        if ((uint8_t)ch < FONT8X16_FIRST) ch = '?';
        const uint8_t *glyph = font8x16_glyphs[(uint8_t)ch - FONT8X16_FIRST];
        for (int gy = 0; gy < 16; gy++) {
            int py = y + gy;
            if (py < 0 || py >= h) continue;
            uint8_t bits = glyph[gy];
            if (bold) bits |= (bits >> 1);
            for (int gx = 0; gx < 8; gx++) {
                int px = x + (int)i * 8 + gx;
                if (px < 0 || px >= w) continue;
                if ((bits >> gx) & 1u) {
                    fb[(size_t)py * stride + (size_t)px] = fg;
                } else if (!transparent) {
                    fb[(size_t)py * stride + (size_t)px] = bg;
                }
            }
        }
    }
}
#endif

LCDTERM_UTEXT static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

LCDTERM_UTEXT static bool cstr_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == *b;
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

LCDTERM_UTEXT static int screen_find_canvas_by_name(const screen_t *scr, const char *name) {
    if (!name || !*name) return -1;
    for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
        if (scr->canvases[i].in_use && cstr_eq(scr->canvases[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static int screen_find_canvas_by_vterm(const screen_t *scr, uint8_t vterm_id) {
    for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
        if (scr->canvases[i].in_use && scr->canvases[i].vterm_id == vterm_id) {
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static int screen_alloc_canvas_named_fmt(screen_t *scr, const char *name, uint8_t vterm_id, uint8_t format) {
    int idx = -1;
    if (name && *name) {
        idx = screen_find_canvas_by_name(scr, name);
    } else {
        idx = screen_find_canvas_by_vterm(scr, vterm_id);
    }
    if (idx >= 0) {
#if CANVAS_SUPPORT_RGB565
        if (format == CANVAS_FMT_RGB565 && scr->canvases[idx].format != CANVAS_FMT_RGB565) {
            uint32_t w = scr->cv.w;
            uint32_t h = scr->cv.h;
            uint32_t bytes = w * h * sizeof(uint16_t);
            uint32_t pages = (bytes + 4095u) / 4096u;
            uint16_t *buf = (uint16_t *)canvas_alloc_psram(pages);
            if (buf) {
                scr->canvases[idx].buf565 = buf;
                scr->canvases[idx].stride565 = w;
                scr->canvases[idx].w565 = (uint16_t)w;
                scr->canvases[idx].h565 = (uint16_t)h;
                scr->canvases[idx].format = CANVAS_FMT_RGB565;
                canvas565_fill(buf, w, 0, 0, (int)w - 1, (int)h - 1, 0xFFFF);
            }
        }
#endif
        return idx;
    }
    for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
        if (!scr->canvases[i].in_use) {
            scr->canvases[i].in_use = true;
            scr->canvases[i].locked = false;
            scr->canvases[i].drawn_w = 0;
            scr->canvases[i].vterm_id = vterm_id;
            scr->canvases[i].format = format;
            scr->canvases[i].buf565 = NULL;
            scr->canvases[i].stride565 = 0;
            scr->canvases[i].w565 = 0;
            scr->canvases[i].h565 = 0;
            if (name && *name) {
                uint32_t nl = 0;
                while (nl + 1 < sizeof(scr->canvases[i].name) && name[nl]) {
                    scr->canvases[i].name[nl] = name[nl];
                    scr->canvases[i].title[nl] = name[nl];
                    nl++;
                }
                scr->canvases[i].name[nl] = '\0';
                scr->canvases[i].title[nl] = '\0';
            } else {
                scr->canvases[i].name[0] = 'c'; scr->canvases[i].name[1] = 'a';
                scr->canvases[i].name[2] = 'n'; scr->canvases[i].name[3] = 'v';
                scr->canvases[i].name[4] = 'a'; scr->canvases[i].name[5] = 's';
                scr->canvases[i].name[6] = '\0';
                scr->canvases[i].title[0] = 'C'; scr->canvases[i].title[1] = 'a';
                scr->canvases[i].title[2] = 'n'; scr->canvases[i].title[3] = 'v';
                scr->canvases[i].title[4] = 'a'; scr->canvases[i].title[5] = 's';
                scr->canvases[i].title[6] = '\0';
            }
#if CANVAS_SUPPORT_RGB565
            if (format == CANVAS_FMT_RGB565) {
                uint32_t w = scr->cv.w;
                uint32_t h = scr->cv.h;
                uint32_t bytes = w * h * sizeof(uint16_t);
                uint32_t pages = (bytes + 4095u) / 4096u;
                uint16_t *buf = (uint16_t *)canvas_alloc_psram(pages);
                if (buf) {
                    scr->canvases[i].buf565 = buf;
                    scr->canvases[i].stride565 = w;
                    scr->canvases[i].w565 = (uint16_t)w;
                    scr->canvases[i].h565 = (uint16_t)h;
                    canvas565_fill(buf, w, 0, 0, (int)w - 1, (int)h - 1, 0xFFFF);
                } else {
                    scr->canvases[i].format = CANVAS_FMT_1BPP;
                }
            }
#else
            scr->canvases[i].format = CANVAS_FMT_1BPP;
#endif
            if (scr->store) {
                canvas1_init(&scr->canvases[i].backing,
                             scr->store + i * scr->store_slot,
                             scr->cv.w / 8u, scr->cv.w, scr->cv.h);
                canvas1_fill(&scr->canvases[i].backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
            }
            return (int)i;
        }
    }
    return -1;
}

__attribute__((unused)) LCDTERM_UTEXT static int screen_alloc_canvas_named(screen_t *scr, const char *name, uint8_t vterm_id) {
    return screen_alloc_canvas_named_fmt(scr, name, vterm_id, CANVAS_FMT_1BPP);
}

__attribute__((unused)) LCDTERM_UTEXT static int screen_alloc_canvas(screen_t *scr, uint8_t vterm_id) {
    return screen_alloc_canvas_named_fmt(scr, NULL, vterm_id, CANVAS_FMT_1BPP);
}

LCDTERM_UTEXT static void screen_free_canvas(screen_t *scr, uint8_t cid) {
    if (cid < SCREEN_MAX_CANVASES) {
#if CANVAS_SUPPORT_RGB565
        if (scr->canvases[cid].buf565) {
            uint32_t bytes = (uint32_t)scr->canvases[cid].w565 * (uint32_t)scr->canvases[cid].h565 * sizeof(uint16_t);
            uint32_t pages = (bytes + 4095u) / 4096u;
            canvas_free_psram(scr->canvases[cid].buf565, pages);
            scr->canvases[cid].buf565 = NULL;
            scr->canvases[cid].stride565 = 0;
            scr->canvases[cid].w565 = 0;
            scr->canvases[cid].h565 = 0;
        }
#endif
        scr->canvases[cid].format = CANVAS_FMT_1BPP;
        scr->canvases[cid].in_use = false;
        scr->canvases[cid].locked = false;
        scr->canvases[cid].vterm_id = 0;
        scr->canvases[cid].drawn_w = 0;
        scr->canvases[cid].name[0] = '\0';
        scr->canvases[cid].title[0] = '\0';
    }
}

LCDTERM_UTEXT static int screen_alloc_reader(screen_t *scr, const char *path) {
    for (uint32_t i = 0; i < SCREEN_MAX_READERS; i++) {
        if (!scr->readers[i].in_use) {
            scr->readers[i].in_use = true;
            if (path && *path) {
                uint32_t pl = 0;
                while (pl + 1 < sizeof(scr->readers[i].path) && path[pl]) {
                    scr->readers[i].path[pl] = path[pl];
                    pl++;
                }
                scr->readers[i].path[pl] = '\0';
            } else {
                scr->readers[i].path[0] = '\0';
            }
            scr->readers[i].title[0] = 'R'; scr->readers[i].title[1] = 'e';
            scr->readers[i].title[2] = 'a'; scr->readers[i].title[3] = 'd';
            scr->readers[i].title[4] = 'e'; scr->readers[i].title[5] = 'r';
            scr->readers[i].title[6] = '\0';
            if (scr->store) {
                uint32_t s_idx = SCREEN_MAX_CANVASES + i;
                canvas1_init(&scr->readers[i].backing,
                             scr->store + s_idx * scr->store_slot,
                             scr->cv.w / 8u, scr->cv.w, scr->cv.h);
                canvas1_fill(&scr->readers[i].backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
            }
            return (int)i;
        }
    }
    return -1;
}

LCDTERM_UTEXT static void screen_free_reader(screen_t *scr, uint8_t rid) {
    if (rid < SCREEN_MAX_READERS) {
        scr->readers[rid].in_use = false;
        scr->readers[rid].path[0] = '\0';
        scr->readers[rid].title[0] = '\0';
    }
}

/* Top bar indicator icons: SD card (11x12), 9P network badge (14x10), and Wi-Fi (12x10) */
LCDTERM_URODATA static const uint16_t icon_sd[12] = {
    0xff80, 0x80c0, 0xaa60, 0xaa20, 0x8020, 0x9b20,
    0x92a0, 0x9aa0, 0x8aa0, 0x9b20, 0x8020, 0xffe0
};

LCDTERM_URODATA static const uint16_t icon_p9[10] = {
    0x7ff8, 0xfffc, 0xce7c, 0xd14c, 0xd17c,
    0xcf4c, 0xc14c, 0xce4c, 0xfffc, 0x7ff8
};

LCDTERM_URODATA static const uint16_t icon_wifi[10] = {
    0x3fc0, 0x6060, 0xc030, 0x0000, 0x0f00, 0x1980,
    0x0000, 0x0600, 0x0f00, 0x0600
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
/* 47.12: the menu bar's layout -- where the clock starts (rx), the indicator
 * icons' left edge, and where the ribbon's minimap starts (sx) -- and one
 * minimap block's width. Shared by draw_menu() and a tap's hit test
 * (minimap_hit()), so that a finger lands on what is drawn. */
LCDTERM_UTEXT static void menu_geometry(const screen_t *scr, int *rx_out, int *icon_left_out, int *sx_out) {
    int w = scr->cv.w;
    uint32_t rn = cstr_len(scr->right);
    unsigned rlen = canvas1_text_len(scr->right, rn);
    int rx = w - 16 - 8 * (int)rlen;
    if (rx < 8) rx = 8;
    int icon_left = rx;
    if (scr->status_flags & SCREEN_STATUS_P9) icon_left -= (14 + 6);
    if (scr->status_flags & SCREEN_STATUS_WIFI) icon_left -= (12 + 6);
    if (scr->status_flags & SCREEN_STATUS_SD) icon_left -= (11 + 6);
    *rx_out = rx;
    *icon_left_out = icon_left;
    *sx_out = 8 + 8 * (int)cstr_len(scr->name) + 16;
}

LCDTERM_UTEXT static int minimap_block_w(const ribbon_win_t *win) {
    return (win->cols >= 90) ? 22 : (win->cols >= 60) ? 15 : (win->cols >= 45) ? 11 : 8;
}

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
    if (scr->status_flags & SCREEN_STATUS_WIFI) {
        icon_left -= (12 + 6);
        draw_icon16(cv, icon_left, 4, 12, 10, icon_wifi);
    }
    if (scr->status_flags & SCREEN_STATUS_SD) {
        icon_left -= (11 + 6);
        draw_icon16(cv, icon_left, 3, 11, 12, icon_sd);
    }

    int room = (icon_left - 16 - 8) / 8;
    if (room > 0)
        canvas1_text(cv, 8, 2, scr->name, cstr_len(scr->name), (unsigned)room, 0, FONT8X16_H, true);

    /* Ribbon visualization stripe in the menu bar */
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
                int bw = minimap_block_w(win);
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
 * remain plain white inside with the title (original classic Macintosh style).
 * If can_close is true, draws a classic Macintosh close box on the left. */
LCDTERM_UTEXT static void draw_titlebar(const canvas1_t *cv, int x0, int y0, int x1, const char *t, bool active, bool can_close) {
    int vx0 = x0 < 0 ? 0 : x0;
    int vx1 = x1 >= (int)cv->w ? (int)cv->w - 1 : x1;
    if (vx0 >= vx1) return;

    canvas1_fill(cv, vx0 + 1, y0 + 1, vx1 - 1, y0 + 15, CANVAS1_WHITE);
    canvas1_hline(cv, vx0, vx1, y0, CANVAS1_BLACK);
    canvas1_hline(cv, vx0, vx1, y0 + 16, CANVAS1_BLACK);

    /* Classic Macintosh close box: 11x11 square at (vx0 + 8, y0 + 3) */
    bool has_box = can_close && (vx0 + 24 < vx1);
    int bx0 = vx0 + 8;
    int by0 = y0 + 3;

    if (active) {
        for (int y = y0 + 3; y <= y0 + 13; y += 2) {
            if (has_box) {
                /* Zebra stripes to the left of the close-box (original Macintosh style) */
                if (vx0 + 2 <= bx0 - 3) {
                    canvas1_hline(cv, vx0 + 2, bx0 - 3, y, CANVAS1_BLACK);
                }
                /* Zebra stripes to the right of the close-box */
                if (bx0 + 13 <= vx1 - 2) {
                    canvas1_hline(cv, bx0 + 13, vx1 - 2, y, CANVAS1_BLACK);
                }
            } else {
                if (vx0 + 2 <= vx1 - 2) {
                    canvas1_hline(cv, vx0 + 2, vx1 - 2, y, CANVAS1_BLACK);
                }
            }
        }
    }

    if (has_box) {
        canvas1_fill(cv, bx0, by0, bx0 + 10, by0 + 10, CANVAS1_WHITE);
        canvas1_hline(cv, bx0, bx0 + 10, by0, CANVAS1_BLACK);
        canvas1_hline(cv, bx0, bx0 + 10, by0 + 10, CANVAS1_BLACK);
        canvas1_vline(cv, bx0, by0, by0 + 10, CANVAS1_BLACK);
        canvas1_vline(cv, bx0 + 10, by0, by0 + 10, CANVAS1_BLACK);
    }

    uint32_t n = cstr_len(t);
    unsigned len = canvas1_text_len(t, n);
    int max = (vx1 - vx0 - 1 - (has_box ? 56 : 40)) / 8;        /* leave some stripes either side */
    if (max < 0) max = 0;
    if (len > (unsigned)max) len = (unsigned)max;
    if (len > 0) {
        int bw = 8 * (int)len + 12;
        int cx = (vx0 + vx1 + 1) / 2 - bw / 2;
        if (has_box && cx < bx0 + 17) cx = bx0 + 17;
        else if (cx < vx0 + 2) cx = vx0 + 2;
        canvas1_fill(cv, cx, y0 + 1, cx + bw - 1, y0 + 15, CANVAS1_WHITE);
        canvas1_text(cv, cx + 6, y0 + 1, t, n, len, 1, 15, true);
    }
}

/* A tile: its frame and shadow, its interior white, and its title bar. */
LCDTERM_UTEXT static void draw_tile(const canvas1_t *cv, int x0, int y0, int x1, int y1, const char *t, bool active, bool can_close) {
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
        /* The two corners the shadow leaves out are desktop. Painted here:
         * clear_ribbon_background() counts the shadow's column and row as
         * the window's, so a repaint left whatever those pixels held. */
        canvas1_fill(cv, x1 + 1, y0, x1 + 1, y0, CANVAS1_GREY);
    }
    if (x0 >= 0) canvas1_fill(cv, x0, y1 + 1, x0, y1 + 1, CANVAS1_GREY);
    draw_titlebar(cv, x0, y0, x1, t, active, can_close);
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

LCDTERM_UTEXT static uint8_t window_vterm(const screen_t *scr, uint8_t idx);

/* The terminal the keyboard is at: the focused window's, or its canvas's
 * owner's. */
LCDTERM_UTEXT static vtterm_t *active_vt(screen_t *scr) {
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
        int t = screen_find_term(scr, window_vterm(scr, scr->ribbon.active_idx));
        if (t >= 0) return &scr->terms[t].vt;
    }
    return &scr->terms[0].vt;
}

/* That terminal's title bar, at its own window -- which is not the focused
 * one when a canvas is: drawn at the text frame regardless, it striped a
 * second bar across whatever was there. */
LCDTERM_UTEXT static void draw_text_title(screen_t *scr) {
    vtterm_t *avt = active_vt(scr);
    if (has_text(scr)) {
        uint8_t act = scr->ribbon.active_idx;
        int idx = ribbon_find_vterm(&scr->ribbon, window_vterm(scr, act));
        if (idx >= 0) {
            int32_t x0 = scr->ribbon.wins[idx].rx0 - scr->ribbon.x_view;
            int32_t x1 = scr->ribbon.wins[idx].rx1 - scr->ribbon.x_view;
            if (x1 >= 0 && x0 < (int32_t)scr->cv.w) {
                bool can_close = !(scr->ribbon.wins[idx].type == RIBBON_WIN_TERM && scr->ribbon.wins[idx].vterm_id == 0);
                draw_titlebar(&scr->cv, (int)x0, SCREEN_TILE_Y, (int)x1, avt->title, idx == (int)act, can_close);
            }
        } else if (scr->ribbon.count == 0) {
            draw_titlebar(&scr->cv, scr->fx0, scr->fy0, scr->fx1, avt->title, true, false);
        }
    }
    scr->title_drawn = avt->title_seq;
}

/* --- 38.8: the canvas stores ----------------------------------------------
 *
 * A slot holds the canvas tile's drawable area, row by row, (w + 7) / 8
 * bytes a row, pixel x of a row in bit x % 8 of byte x / 8 -- the
 * framebuffer's own order, but from the tile's left edge rather than the
 * screen's, so the same slot restores a split at either side. */

/* the layout stores and the canvases' backings share these four
 * slots -- layout L's store is slot L - 1, canvas i's backing is slot i --
 * so the two can only both be right while canvas 0 is the only canvas.
 * With a second one, saving canvas 0 in a split wrote it over the other
 * canvas's backing, and restoring copied a layout's slot into whichever
 * canvas came first. The stores step aside then; a canvas comes back from
 * its backing, or blank with damage, as without a store. */
LCDTERM_UTEXT static bool stores_usable(const screen_t *scr, unsigned cid) {
    if (!scr->store || cid != 0) return false;
    for (uint32_t i = 1; i < SCREEN_MAX_CANVASES; i++)
        if (scr->canvases[i].in_use) return false;
    return true;
}

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
    if (scr->store && !stores_usable(scr, cid)) return;
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
    if (scr->store && !stores_usable(scr, cid)) return false;
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
        for (uint32_t i = 0; i < SCREEN_MAX_READERS; i++) {
            uint32_t s_idx = SCREEN_MAX_CANVASES + i;
            canvas1_init(&scr->readers[i].backing, scr->store + s_idx * scr->store_slot, scr->cv.w / 8u, scr->cv.w, scr->cv.h);
            canvas1_fill(&scr->readers[i].backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
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
    if (h > SCREEN_MENU_H) {
        canvas1_fill(cv, 0, SCREEN_MENU_H, w - 1, h - 1, CANVAS1_GREY);
    }
}

/* During animation: clear only desktop areas outside visible windows,
 * strictly preserving window interiors so the screen never blackens out. */
LCDTERM_UTEXT static void clear_ribbon_gaps(const screen_t *scr) {
    const canvas1_t *cv = &scr->cv;
    int w = (int)cv->w;
    int h = (int)cv->h;
    if (h <= SCREEN_MENU_H) return;
    int tile_y1 = SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(cv->h) + 1;
    if (tile_y1 >= h) tile_y1 = h - 1;

    /* The thin strip between menu bar and window tiles (rows 18..21) */
    if (SCREEN_TILE_Y > SCREEN_MENU_H) {
        canvas1_fill(cv, 0, SCREEN_MENU_H, w - 1, SCREEN_TILE_Y - 1, CANVAS1_GREY);
    }

    int32_t xv = scr->ribbon.x_view;
    struct { int x0, x1; } spans[RIBBON_MAX_WINDOWS];
    uint8_t nspans = 0;
    for (uint8_t i = 0; i < scr->ribbon.count && nspans < RIBBON_MAX_WINDOWS; i++) {
        int32_t x0 = scr->ribbon.wins[i].rx0 - xv;
        int32_t x1 = scr->ribbon.wins[i].rx1 - xv + 1; /* include shadow */
        if (x1 < 0 || x0 >= w) continue;
        if (x0 < 0) x0 = 0;
        if (x1 >= w) x1 = w - 1;
        spans[nspans].x0 = (int)x0;
        spans[nspans].x1 = (int)x1;
        nspans++;
    }

    /* Insertion sort by x0 */
    for (uint8_t i = 1; i < nspans; i++) {
        int k0 = spans[i].x0, k1 = spans[i].x1;
        int j = (int)i - 1;
        while (j >= 0 && spans[j].x0 > k0) {
            spans[j + 1] = spans[j];
            j--;
        }
        spans[j + 1].x0 = k0;
        spans[j + 1].x1 = k1;
    }

    /* Fill horizontal gaps between window spans from SCREEN_TILE_Y to tile_y1 + 1 */
    int fill_y1 = tile_y1 + 1 < h ? tile_y1 + 1 : h - 1;
    int cur_x = 0;
    for (uint8_t i = 0; i < nspans; i++) {
        if (spans[i].x0 > cur_x) {
            canvas1_fill(cv, cur_x, SCREEN_TILE_Y, spans[i].x0 - 1, fill_y1, CANVAS1_GREY);
        }
        if (spans[i].x1 + 1 > cur_x) {
            cur_x = spans[i].x1 + 1;
        }
    }
    if (cur_x < w) {
        canvas1_fill(cv, cur_x, SCREEN_TILE_Y, w - 1, fill_y1, CANVAS1_GREY);
    }

    /* Bottom margin below window tiles, if any */
    if (tile_y1 + 2 < h) {
        canvas1_fill(cv, 0, tile_y1 + 2, w - 1, h - 1, CANVAS1_GREY);
    }
}

LCDTERM_UTEXT static bool place(screen_t *scr, unsigned layout);

/* Every pixel again. The canvas comes back blank, so its damage moves on --
 * unless `restore` and its layout's store has it (38.8). */
LCDTERM_UTEXT static void draw_all_from(screen_t *scr, bool restore) {
    const canvas1_t *cv = &scr->cv;
    (void)place(scr, scr->layout);
    if (scr->animating) {
        clear_ribbon_gaps(scr);
    } else {
        clear_ribbon_background(scr);
    }

    if (scr->ribbon.count > 0) {
        int32_t xv = scr->ribbon.x_view;
        int tile_y1 = SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(cv->h) + 1;

        /* Draw window tiles: inactive windows first, then active window on top */
        for (uint8_t pass = 0; pass < 2; pass++) {
            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                bool is_active = (i == scr->ribbon.active_idx);
                if ((pass == 0 && is_active) || (pass == 1 && !is_active)) continue;
                const ribbon_win_t *w = &scr->ribbon.wins[i];
                int32_t x0 = w->rx0 - xv;
                int32_t x1 = w->rx1 - xv;
                if (x1 + 1 >= 0 && x0 < (int32_t)cv->w) {
                    const char *title = w->title;
                    bool can_close = !(w->type == RIBBON_WIN_TERM && w->vterm_id == 0);
                    if (w->type == RIBBON_WIN_CANVAS) {
                        uint8_t cid = w->vterm_id;
                        if (cid < SCREEN_MAX_CANVASES && scr->canvases[cid].name[0]) {
                            title = scr->canvases[cid].name;
                        } else if (scr->ctitle[0]) {
                            title = scr->ctitle;
                        }
                    } else if (w->type == RIBBON_WIN_TERM) {
                        int t = screen_find_term(scr, w->vterm_id);
                        if (t >= 0 && scr->terms[t].vt.title[0]) {
                            title = scr->terms[t].vt.title;
                        }
                    } else if (w->type == RIBBON_WIN_READER) {
                        uint8_t rid = w->vterm_id;
                        if (rid < SCREEN_MAX_READERS && scr->readers[rid].title[0]) {
                            title = scr->readers[rid].title;
                        }
                    }
                    draw_tile(cv, (int)x0, SCREEN_TILE_Y, (int)x1, tile_y1, title, is_active, can_close);
                }
            }
        }

        /* Blit backing stores for visible canvas and reader windows: inactive first, then active on top */
        for (uint8_t pass = 0; pass < 2; pass++) {
            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                bool is_active = (i == scr->ribbon.active_idx);
                if ((pass == 0 && is_active) || (pass == 1 && !is_active)) continue;
                const ribbon_win_t *w = &scr->ribbon.wins[i];
                int32_t cx0 = w->rx0 - xv;
                int32_t cx1 = w->rx1 - xv;
                if (cx1 >= 0 && cx0 < (int32_t)cv->w) {
                    int cw_w = (int)(w->rx1 - w->rx0 - 1);
                    int cw_h = (int)(tile_y1 - (SCREEN_TILE_Y + 17));
                    if (cw_w > 0 && cw_h > 0) {
                        if (w->type == RIBBON_WIN_CANVAS) {
                            uint8_t cid = w->vterm_id;
                            bool restored = false;
                            bool backed = cid < SCREEN_MAX_CANVASES && scr->canvases[cid].in_use && scr->store;
                            if (restore) {
                                restored = store_restore(scr);
                            }
                            if (restored || scr->animating || (backed && (!restore || scr->canvases[cid].drawn_w == (uint16_t)cw_w))) {
                                blit_canvas_backing(scr, cid, (int)cx0 + 1, SCREEN_TILE_Y + 17, cw_w, cw_h);
                                if (backed && !scr->animating) scr->canvases[cid].drawn_w = (uint16_t)cw_w;
                            } else {
                                canvas1_fill(cv, (int)cx0 + 1, SCREEN_TILE_Y + 17, (int)cx1 - 1, tile_y1 - 1, CANVAS1_WHITE);
                                if (backed) {
                                    canvas1_fill(&scr->canvases[cid].backing, 0, 0, scr->cv.w - 1, scr->cv.h - 1, CANVAS1_WHITE);
                                    scr->canvases[cid].drawn_w = (uint16_t)cw_w;
                                }
                                scr->damage++;
                            }
                        } else if (w->type == RIBBON_WIN_READER) {
                            uint8_t rid = w->vterm_id;
                            bool backed = rid < SCREEN_MAX_READERS && scr->readers[rid].in_use && scr->store;
                            if (backed) {
                                blit_canvas_backing(scr, SCREEN_MAX_CANVASES + rid, (int)cx0 + 1, SCREEN_TILE_Y + 17, cw_w, cw_h);
                            } else {
                                canvas1_fill(cv, (int)cx0 + 1, SCREEN_TILE_Y + 17, (int)cx1 - 1, tile_y1 - 1, CANVAS1_WHITE);
                            }
                        }
                    }
                }
            }
        }
    } else {
        if (has_text(scr)) draw_tile(cv, scr->fx0, scr->fy0, scr->fx1, scr->fy1, scr->vt.title, true, false);
        if (has_canvas(scr)) {
            draw_tile(cv, scr->cx0, scr->cy0, scr->cx1, scr->cy1, scr->ctitle, false, true);
            if (!restore || !store_restore(scr)) {
                canvas1_fill(cv, scr->cx0 + 1, scr->cy0 + 17, scr->cx1 - 1, scr->cy1 - 1, CANVAS1_WHITE);
                scr->damage++;
            }
        }
    }
    vtterm_t *avt = active_vt(scr);
    scr->title_drawn = avt->title_seq;
    if (!scr->animating) {
        draw_menu(scr);
    }
    if (scr->ribbon.count > 0) {
        int active_vid = window_vterm(scr, scr->ribbon.active_idx);
        for (uint32_t t = 0; t < SCREEN_MAX_TERMS; t++) {
            if (scr->terms[t].in_use && !scr->terms[t].vt.hidden && scr->terms[t].vterm_id != active_vid) {
                vtterm_repaint(&scr->terms[t].vt);
            }
        }
        for (uint32_t t = 0; t < SCREEN_MAX_TERMS; t++) {
            if (scr->terms[t].in_use && !scr->terms[t].vt.hidden && scr->terms[t].vterm_id == active_vid) {
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
    int C = scr->cv.w / (int)FONT_CELL_W;
    unsigned rows = SCREEN_TEXT_ROWS(scr->cv.h);
    int y1 = SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)rows + 1;
    int32_t xv = scr->ribbon.x_view;
    scr->layout = (uint8_t)layout;

    /* A terminal without a window draws nowhere. Its vt still pointed at
     * the place its window had last been, and wrote over whatever window
     * was there now -- the root terminal's, after Super+W closed its window
     * (now refused), and any whose window a closed vterm left behind. */
    for (uint32_t t = 0; t < SCREEN_MAX_TERMS; t++) {
        if (scr->terms[t].in_use && ribbon_find_vterm(&scr->ribbon, scr->terms[t].vterm_id) < 0)
            vtterm_set_hidden(&scr->terms[t].vt, true);
    }

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
            c0 = (int)((x0 + 4) / (int)FONT_CELL_W);
            col_off = 0;
            int max_c = C - 1 - c0;
            if (max_c < 1) {                /* only its frame is on screen */
                vtterm_set_hidden(&scr->terms[t].vt, true);
                continue;
            }
            if (win_c > max_c) win_c = max_c;
        } else {
            c0 = 1;
            col_off = (int)((4 - x0) / (int)FONT_CELL_W);
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
        fbtext_init(&text, scr->cv.fb + SCREEN_TEXT_Y * scr->cv.stride + (uint32_t)c0 * FONT_CELL_BYTES, scr->cv.stride,
                    (unsigned)win_c, rows);
        text.xbyte = (uint16_t)(c0 * FONT_CELL_BYTES);
        text.whole_rows = (win_c == scr->full_cols && c0 == 1);
        vtterm_resize(&scr->terms[t].vt, &text);
        vtterm_set_hidden(&scr->terms[t].vt, false);

        if (w->vterm_id == window_vterm(scr, scr->ribbon.active_idx)) {
            scr->fx0 = (int16_t)x0;
            scr->fx1 = (int16_t)(x0 + (int)FONT_CELL_W * w->cols + 6);
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

#if defined(CONFIG_ENABLE_RIBBON_ANIMATION) && CONFIG_ENABLE_RIBBON_ANIMATION && \
    ((defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LCD_PCLK_GPIO)) || \
     (defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))))
#define SCREEN_HAS_ANIMATION 1
#else
#define SCREEN_HAS_ANIMATION 0
#endif

#ifndef SYS_DELAY_US
#define SYS_DELAY_US 24
#endif

#ifndef SYS_LCD_SYNC
#define SYS_LCD_SYNC 29
#endif

LCDTERM_UTEXT static inline void lcdterm_delay_us(uint32_t us) {
#if defined(__riscv)
    register long r_a0 __asm__("a0") = SYS_DELAY_US;
    register long r_a1 __asm__("a1") = (long)us;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
#else
    (void)us;
#endif
}

#if defined(CONFIG_BOARD_RP2350)
LCDTERM_UTEXT static inline void lcdterm_sync_pre(void) {
#if defined(__riscv)
    register long r_a0 __asm__("a0") = SYS_LCD_SYNC;
    register long r_a1 __asm__("a1") = 0;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
#endif
}

LCDTERM_UTEXT static inline void lcdterm_sync_post(void) {
#if defined(__riscv)
    register long r_a0 __asm__("a0") = SYS_LCD_SYNC;
    register long r_a1 __asm__("a1") = 1;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1) : "memory");
#endif
}
#else
#include "drivers/lcdterm.h"
#endif

LCDTERM_UTEXT static void screen_animate_ribbon(screen_t *scr,
                                                uint8_t win_a, int32_t a_start_rx0, int32_t a_start_rx1,
                                                uint8_t win_b, int32_t b_start_rx0, int32_t b_start_rx1,
                                                int32_t start_xv) {
    int32_t target_xv = scr->ribbon.x_target;
    int32_t a_target_rx0 = (win_a < scr->ribbon.count) ? scr->ribbon.wins[win_a].rx0 : 0;
    int32_t a_target_rx1 = (win_a < scr->ribbon.count) ? scr->ribbon.wins[win_a].rx1 : 0;
    int32_t b_target_rx0 = (win_b < scr->ribbon.count) ? scr->ribbon.wins[win_b].rx0 : 0;
    int32_t b_target_rx1 = (win_b < scr->ribbon.count) ? scr->ribbon.wins[win_b].rx1 : 0;

    bool has_win_anim = (win_a < scr->ribbon.count && win_b < scr->ribbon.count && win_a != win_b);
    bool has_xv_anim = (start_xv != target_xv);

    if (!SCREEN_HAS_ANIMATION || (!has_win_anim && !has_xv_anim)) {
        scr->ribbon.x_view = target_xv;
        (void)place(scr, scr->layout);
        draw_all_from(scr, true);
        return;
    }

    const int STEPS = 5;
    scr->animating = true;
    for (int s = 0; s < STEPS; s++) {
        /* Ease-out weights (scaled by 1000) for smooth deceleration */
        int w = (s == 0) ? 280 : (s == 1) ? 540 : (s == 2) ? 750 : (s == 3) ? 900 : 1000;
        if (has_win_anim) {
            int32_t d_a0 = a_target_rx0 - a_start_rx0;
            int32_t d_a1 = a_target_rx1 - a_start_rx1;
            int32_t d_b0 = b_target_rx0 - b_start_rx0;
            int32_t d_b1 = b_target_rx1 - b_start_rx1;
            scr->ribbon.wins[win_a].rx0 = a_start_rx0 + (d_a0 * w) / 1000;
            scr->ribbon.wins[win_a].rx1 = a_start_rx1 + (d_a1 * w) / 1000;
            scr->ribbon.wins[win_b].rx0 = b_start_rx0 + (d_b0 * w) / 1000;
            scr->ribbon.wins[win_b].rx1 = b_start_rx1 + (d_b1 * w) / 1000;
        }
        if (has_xv_anim) {
            int32_t d_xv = target_xv - start_xv;
            scr->ribbon.x_view = start_xv + (d_xv * w) / 1000;
        } else {
            scr->ribbon.x_view = target_xv;
        }

        lcdterm_sync_pre();
        (void)place(scr, scr->layout);
        draw_all_from(scr, true);
        lcdterm_sync_post();
    }

    /* Final canonical positioning */
    scr->animating = false;
    if (has_win_anim) {
        scr->ribbon.wins[win_a].rx0 = a_target_rx0;
        scr->ribbon.wins[win_a].rx1 = a_target_rx1;
        scr->ribbon.wins[win_b].rx0 = b_target_rx0;
        scr->ribbon.wins[win_b].rx1 = b_target_rx1;
    }
    scr->ribbon.x_view = target_xv;
    lcdterm_sync_pre();
    (void)place(scr, scr->layout);
    draw_all_from(scr, true);
    lcdterm_sync_post();
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
    scr->animating = false;
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
        scr->canvases[c].drawn_w = 0;
        scr->canvases[c].title[0] = '\0';
        scr->canvases[c].name[0] = '\0';
        scr->canvases[c].buf565 = NULL;
        scr->canvases[c].stride565 = 0;
        scr->canvases[c].w565 = 0;
        scr->canvases[c].h565 = 0;
        scr->canvases[c].format = CANVAS_FMT_1BPP;
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

void screen_reorient(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h) {
    canvas1_init(&scr->cv, fb, stride, w, h);
    scr->full_cols = (uint8_t)SCREEN_TEXT_COLS(w);
    scr->ribbon.screen_w = (uint16_t)w;
    scr->ribbon.screen_h = (uint16_t)h;

    uint32_t text_rows = (uint32_t)SCREEN_TEXT_ROWS(h);
    for (uint32_t t = 0; t < SCREEN_MAX_TERMS; t++) {
        fbtext_init(&scr->terms[t].vt.text, (uint8_t *)fb + SCREEN_TEXT_Y * stride + SCREEN_TEXT_X / 8, stride,
                    scr->full_cols, text_rows);
    }
    fbtext_init(&scr->vt.text, (uint8_t *)fb + SCREEN_TEXT_Y * stride + SCREEN_TEXT_X / 8, stride,
                scr->full_cols, text_rows);

    if (w < h) {
        /* Portrait mode: full-width windows (88 cols at 720px) */
        uint8_t port_cols = (uint8_t)RIBBON_COLS_FULL(w);
        for (uint8_t i = 0; i < scr->ribbon.count; i++) {
            scr->ribbon.wins[i].cols = port_cols;
        }
        int reader_idx = -1;
        for (uint8_t i = 0; i < scr->ribbon.count; i++) {
            if (scr->ribbon.wins[i].type == RIBBON_WIN_READER) {
                reader_idx = (int)i;
                break;
            }
        }
        if (reader_idx >= 0) {
            ribbon_focus(&scr->ribbon, (uint8_t)reader_idx);
        }
    } else {
        /* Landscape mode: side-by-side pair if terminal + other window */
        bool has_other = false;
        for (uint8_t i = 0; i < scr->ribbon.count; i++) {
            if (scr->ribbon.wins[i].type != RIBBON_WIN_TERM) {
                has_other = true;
                break;
            }
        }
        if (has_other) {
            uint8_t h_cols = RIBBON_COLS_HALF(w);
            uint8_t other_cols = (uint8_t)(scr->full_cols - h_cols - 2u);
            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                if (scr->ribbon.wins[i].type == RIBBON_WIN_TERM) {
                    scr->ribbon.wins[i].cols = h_cols;
                } else {
                    scr->ribbon.wins[i].cols = other_cols;
                }
            }
        } else {
            uint8_t full_cols = (uint8_t)RIBBON_COLS_FULL(w);
            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                scr->ribbon.wins[i].cols = full_cols;
            }
        }
    }

    ribbon_layout(&scr->ribbon);
    scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
    scr->ribbon.x_view = scr->ribbon.x_target;

    if (scr->store) {
        canvas1_init(&scr->c_backing, scr->store, w / 8u, w, h);
        for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) {
            canvas1_init(&scr->canvases[i].backing, scr->store + i * scr->store_slot, w / 8u, w, h);
        }
        for (uint32_t i = 0; i < SCREEN_MAX_READERS; i++) {
            uint32_t s_idx = SCREEN_MAX_CANVASES + i;
            canvas1_init(&scr->readers[i].backing, scr->store + s_idx * scr->store_slot, w / 8u, w, h);
            canvas1_fill(&scr->readers[i].backing, 0, 0, w - 1, h - 1, CANVAS1_WHITE);
        }
    }

    (void)place(scr, scr->layout);
    draw_all_from(scr, true);
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

LCDTERM_UTEXT bool screen_set_layout_vterm_named_fmt(screen_t *scr, uint8_t vid, unsigned layout, const char *name, uint8_t format) {
    int cid_check = (name && *name) ? screen_find_canvas_by_name(scr, name) : screen_find_canvas_by_vterm(scr, vid);
    if (cid_check >= 0 && scr->canvases[cid_check].locked && layout != scr->layout && layout != SCREEN_LAYOUT_TEXT) return false;

    if (has_canvas(scr)) store_save(scr);

    if (layout == SCREEN_LAYOUT_TEXT) {
        int cid = -1;
        if (name && *name) {
            cid = screen_find_canvas_by_name(scr, name);
        } else {
            cid = screen_find_canvas_by_vterm(scr, vid);
            if (cid < 0 && scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
                scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_CANVAS) {
                cid = (int)scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
            }
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
    /* D7: the text side's share of the pair (drivers/ribbon.h). */
    if (layout == SCREEN_LAYOUT_SPLIT_WIDE) {
        text_cols = RIBBON_COLS_TWO_FIFTHS(scr->cv.w);
    } else if (layout == SCREEN_LAYOUT_SPLIT_HALF) {
        text_cols = RIBBON_COLS_HALF(scr->cv.w);
    } else if (layout != SCREEN_LAYOUT_CANVAS) {
        text_cols = RIBBON_COLS_TWO_THIRDS(scr->cv.w);
    }
    if (text_cols && text_cols < RIBBON_TERM_MIN_COLS) text_cols = RIBBON_TERM_MIN_COLS;
    if (layout != SCREEN_LAYOUT_CANVAS) {
        if (8 * (C - 1 - (int)text_cols) - 14 < 4 + 24) return false;   /* no room for a canvas */
    }

    uint8_t canvas_cols = (layout == SCREEN_LAYOUT_CANVAS) ? scr->full_cols : (uint8_t)(scr->full_cols - text_cols - 2u);

    int cid = -1;
    if (name && *name) {
        cid = screen_find_canvas_by_name(scr, name);
    } else {
        cid = screen_find_canvas_by_vterm(scr, vid);
    }
    int c_win = (cid >= 0) ? ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid) : -1;
    int t_win = ribbon_find_vterm(&scr->ribbon, vid);

    if (c_win >= 0) {
#if CANVAS_SUPPORT_RGB565
        if (cid >= 0 && format == CANVAS_FMT_RGB565 && scr->canvases[cid].format != CANVAS_FMT_RGB565) {
            uint32_t w = scr->cv.w;
            uint32_t h = scr->cv.h;
            uint32_t bytes = w * h * sizeof(uint16_t);
            uint32_t pages = (bytes + 4095u) / 4096u;
            uint16_t *buf = (uint16_t *)canvas_alloc_psram(pages);
            if (buf) {
                scr->canvases[cid].buf565 = buf;
                scr->canvases[cid].stride565 = w;
                scr->canvases[cid].w565 = (uint16_t)w;
                scr->canvases[cid].h565 = (uint16_t)h;
                scr->canvases[cid].format = CANVAS_FMT_RGB565;
                canvas565_fill(buf, w, 0, 0, (int)w - 1, (int)h - 1, 0xFFFF);
            }
        }
#endif
        /* Canvas already in ribbon: update column widths and bring into view */
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
        if (name && *name) {
            cid = screen_alloc_canvas_named_fmt(scr, name, vid, format);
        } else {
            cid = screen_alloc_canvas_named_fmt(scr, NULL, vid, format);
        }
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
            } else {
                scr->ribbon.active_idx = (insert_idx <= act) ? (uint8_t)(act + 1u) : act;
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

LCDTERM_UTEXT bool screen_set_layout_vterm_named(screen_t *scr, uint8_t vid, unsigned layout, const char *name) {
    return screen_set_layout_vterm_named_fmt(scr, vid, layout, name, CANVAS_FMT_1BPP);
}

LCDTERM_UTEXT bool screen_set_layout_vterm(screen_t *scr, uint8_t vid, unsigned layout) {
    return screen_set_layout_vterm_named(scr, vid, layout, NULL);
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

/* 47.12: which ribbon window a tap at menu-bar x means -- the minimap block
 * nearest to it, if x is within a finger's width of the minimap at all. The
 * blocks are 8-22 px wide and a fingertip is ~40, so "nearest" rather than
 * "inside". -1 for none. */
LCDTERM_UTEXT static int minimap_hit(const screen_t *scr, int x) {
    int rx, icon_left, sx;
    menu_geometry(scr, &rx, &icon_left, &sx);
    if (scr->ribbon.count == 0 || icon_left - 16 - sx <= 20) return -1;
    int best = -1, best_d = 0, cur = sx, last_x1 = sx;
    for (uint8_t i = 0; i < scr->ribbon.count; i++) {
        int bw = minimap_block_w(&scr->ribbon.wins[i]);
        if (cur + bw > rx - 16) break;
        int d = x - (cur + bw / 2);
        if (d < 0) d = -d;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
        last_x1 = cur + bw - 1;
        cur += bw + 3;
    }
    if (x < sx - 20 || x > last_x1 + 20) return -1;
    return best;
}

/* 47.12: the ribbon window under screen x (below the menu bar), or -1. */
LCDTERM_UTEXT static int window_at(const screen_t *scr, int x) {
    int32_t rx = (int32_t)x + scr->ribbon.x_view;
    for (uint8_t i = 0; i < scr->ribbon.count; i++)
        if (rx >= scr->ribbon.wins[i].rx0 && rx <= scr->ribbon.wins[i].rx1) return i;
    return -1;
}

LCDTERM_UTEXT static int arg16(const uint8_t *req, uint32_t n, uint32_t i) {
    uint32_t at = 1u + 2u * i;
    if (at + 2u > n) return 0;
    return (int)(int16_t)(uint16_t)(req[at] | ((uint16_t)req[at + 1u] << 8));
}

#if CANVAS_SUPPORT_RGB565
static inline uint16_t uarg16(const uint8_t *req, uint32_t n, uint32_t i) {
    uint32_t at = 1u + 2u * i;
    if (at + 2u > n) return 0;
    return (uint16_t)(req[at] | ((uint16_t)req[at + 1u] << 8));
}
#endif

LCDTERM_UTEXT static unsigned colour(int c) {
    return (c == 0 || c == 0xffff || c == -1) ? CANVAS1_WHITE : c == 2 ? CANVAS1_GREY : CANVAS1_BLACK;
}

#if CANVAS_SUPPORT_RGB565
static inline unsigned mono_fallback(uint16_t c565) {
    if (c565 == 0xFFFF) return CANVAS1_WHITE;
    if (c565 == 0) return CANVAS1_BLACK;
    uint32_t r = (c565 >> 11) & 0x1f;
    uint32_t g = (c565 >> 5) & 0x3f;
    uint32_t b = c565 & 0x1f;
    uint32_t lum = (r * 77u + (g >> 1) * 150u + b * 29u) >> 5;
    return lum > 128u ? CANVAS1_WHITE : CANVAS1_BLACK;
}
#endif

/* The canvas a request from terminal `vid` means: its own, else the focused
 * window's, else the first in the ribbon; -1 for none. */
LCDTERM_UTEXT static int canvas_for(const screen_t *scr, uint8_t vid) {
    int cid = screen_find_canvas_by_vterm(scr, vid);
    if (cid < 0 && scr->ribbon.active_idx < scr->ribbon.count &&
        scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_CANVAS) {
        cid = (int)scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    }
    if (cid < 0) {
        int c_any = ribbon_find_canvas(&scr->ribbon);
        if (c_any >= 0) cid = (int)scr->ribbon.wins[c_any].vterm_id;
    }
    return (cid >= 0 && cid < (int)SCREEN_MAX_CANVASES) ? cid : -1;
}

/* Canvas `cid`'s drawable size in its window; 0 x 0 without one. */
LCDTERM_UTEXT static void canvas_size(const screen_t *scr, int cid, int *w, int *h) {
    *w = *h = 0;
    if (cid < 0 || !scr->canvases[cid].in_use) return;
    int c_win = ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid);
    if (c_win < 0) return;
    const ribbon_win_t *win = &scr->ribbon.wins[c_win];
    *w = (int)(win->rx1 - win->rx0 - 1);
    *h = (int)(SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
    if (*w < 0 || *h < 0) *w = *h = 0;
}

/* The terminal keyboard input belongs to while window `idx` has the focus:
 * a terminal's own, a canvas's owner's. A canvas window's vterm_id field is
 * its canvas slot, and handing that on as a terminal sent the keys of a
 * focused canvas to whichever terminal had the slot's number. */
LCDTERM_UTEXT static uint8_t window_vterm(const screen_t *scr, uint8_t idx) {
    if (idx >= scr->ribbon.count) return 0;
    const ribbon_win_t *w = &scr->ribbon.wins[idx];
    if (w->type == RIBBON_WIN_CANVAS)
        return (w->vterm_id < SCREEN_MAX_CANVASES) ? scr->canvases[w->vterm_id].vterm_id : 0;
    return w->vterm_id;
}

/* Closes window `idx` ('C', 'Q'). The root terminal's window stays: its
 * shell is the boot shell, and without a window it had nowhere to draw.
 * Neither does the last window go. A terminal takes its canvas with it: left
 * behind, it stayed in the ribbon with no terminal to draw on it or close it,
 * and the next terminal given that vterm number inherited it, picture and all
 * (47.8). */
LCDTERM_UTEXT static bool close_window(screen_t *scr, uint8_t idx, uint8_t *reply) {
    if (idx >= scr->ribbon.count || scr->ribbon.count <= 1) return false;
    uint8_t closed_type = scr->ribbon.wins[idx].type;
    uint8_t closed_id = scr->ribbon.wins[idx].vterm_id;
    if (closed_type == RIBBON_WIN_TERM && closed_id == 0) return false;
    if (!ribbon_remove(&scr->ribbon, idx)) return false;
    if (closed_type == RIBBON_WIN_TERM) {
        int cid = screen_find_canvas_by_vterm(scr, closed_id);
        if (cid >= 0) {
            int c_idx = ribbon_find_canvas_slot(&scr->ribbon, (uint8_t)cid);
            if (c_idx >= 0) (void)ribbon_remove(&scr->ribbon, (uint8_t)c_idx);
            screen_free_canvas(scr, (uint8_t)cid);
        }
        screen_free_term(scr, closed_id);
    } else if (closed_type == RIBBON_WIN_CANVAS) {
        screen_free_canvas(scr, closed_id);
    } else if (closed_type == RIBBON_WIN_READER) {
        screen_free_reader(scr, closed_id);
    }
    if (ribbon_find_canvas(&scr->ribbon) < 0) scr->layout = SCREEN_LAYOUT_TEXT;
    if (scr->ribbon.count == 1 && scr->ribbon.wins[0].type == RIBBON_WIN_TERM) {
        scr->ribbon.wins[0].cols = scr->full_cols;
    }
    ribbon_layout(&scr->ribbon);
    scr->ribbon.x_target = ribbon_compute_viewport(&scr->ribbon, scr->ribbon.active_idx, scr->ribbon.x_view);
    scr->ribbon.x_view = scr->ribbon.x_target;
    (void)place(scr, scr->layout);
    draw_all_from(scr, true);
    reply[10] = closed_type;
    reply[11] = closed_id;
    return true;
}

LCDTERM_UTEXT void screen_canvas_vterm(screen_t *scr, uint8_t vid, const uint8_t *req, uint32_t n, uint8_t *reply) {
    uint8_t status = 0, pixel = 0;
    uint8_t op = n ? req[0] : 0;
    if (op == 'L') {
        char name_buf[SCREEN_CTITLE_MAX];
        const char *name = NULL;
        if (n >= 3 && req[2] != '\0') {
            uint32_t nl = 0;
            while (2u + nl < n && nl + 1u < sizeof(name_buf) && req[2u + nl]) {
                name_buf[nl] = (char)req[2u + nl];
                nl++;
            }
            name_buf[nl] = '\0';
            name = name_buf;
        }
        uint8_t raw_layout = (n >= 2) ? req[1] : 0;
        uint8_t layout = raw_layout & 0x7fu;
        uint8_t format = (raw_layout & 0x80u) ? CANVAS_FMT_RGB565 : CANVAS_FMT_1BPP;
        status = (n >= 2 && screen_set_layout_vterm_named_fmt(scr, vid, layout, name, format)) ? 0 : 1;
    } else if (op == 'T') {
        if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
            scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_READER) {
            uint8_t rid = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
            if (rid < SCREEN_MAX_READERS) {
                uint32_t len = n - 1u;
                if (len > SCREEN_CTITLE_MAX - 1u) {
                    len = SCREEN_CTITLE_MAX - 1u;
                    while (len > 0 && (req[1u + len] & 0xc0u) == 0x80u) len--;
                }
                for (uint32_t i = 0; i < len; i++) scr->readers[rid].title[i] = (char)req[1u + i];
                scr->readers[rid].title[len] = '\0';
                ribbon_set_title(&scr->ribbon, scr->ribbon.active_idx, scr->readers[rid].title);
                int32_t cx0 = scr->ribbon.wins[scr->ribbon.active_idx].rx0 - scr->ribbon.x_view;
                int32_t cx1 = scr->ribbon.wins[scr->ribbon.active_idx].rx1 - scr->ribbon.x_view;
                if (cx1 >= 0 && cx0 < (int32_t)scr->cv.w) {
                    draw_titlebar(&scr->cv, (int)cx0, SCREEN_TILE_Y, (int)cx1, scr->readers[rid].title, true, true);
                }
            }
            status = 0;
        } else {
            int cid = canvas_for(scr, vid);
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
                    draw_titlebar(&scr->cv, (int)cx0, SCREEN_TILE_Y, (int)cx1, scr->canvases[cid].title, active, true);
                }
            }
            status = 0;
        }
    } else if (op == 'M') {
        if (ribbon_toggle_maximize(&scr->ribbon)) {
            scr->ribbon.x_view = scr->ribbon.x_target;
            (void)place(scr, scr->layout);
            draw_all_from(scr, true);
            status = 0;
        } else {
            status = 1;
        }
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
        /* Insert a new window in the ribbon (Cmd+Enter) */
        uint8_t n_vid = (n >= 2) ? req[1] : 0;
        uint8_t cols = (n >= 3 && req[2]) ? req[2] : RIBBON_COLS_HALF(scr->cv.w);
        /* The title is the request's tail, n - 3 bytes with no NUL of its
         * own: in the lcdterm task it was read on into whatever the
         * buffer held from the request before -- usually a terminal's
         * output, which then became the title. */
        char tbuf[RIBBON_TITLE_MAX];
        uint32_t tl = 0;
        while (3u + tl < n && tl + 1u < sizeof(tbuf) && req[3u + tl]) {
            tbuf[tl] = (char)req[3u + tl];
            tl++;
        }
        tbuf[tl] = '\0';
        const char *title = tl ? tbuf : 0;
        char def_title[16];
        if (!title) {
            def_title[0] = 'l'; def_title[1] = 's'; def_title[2] = 'h';
            def_title[3] = ' '; def_title[4] = '[';
            def_title[5] = (char)('0' + (n_vid % 10));
            def_title[6] = ']'; def_title[7] = '\0';
            title = def_title;
        }
        if (scr->ribbon.count == 1 && scr->ribbon.wins[0].cols > RIBBON_COLS_HALF(scr->cv.w)) {
            scr->ribbon.wins[0].cols = RIBBON_COLS_HALF(scr->cv.w);
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
        /* Focus left window (Cmd+Left) */
        int32_t start_xv = scr->ribbon.x_view;
        if (ribbon_focus_step(&scr->ribbon, -1)) {
            screen_animate_ribbon(scr, 255, 0, 0, 255, 0, 0, start_xv);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '>') {
        /* Focus right window (Cmd+Right) */
        int32_t start_xv = scr->ribbon.x_view;
        if (ribbon_focus_step(&scr->ribbon, 1)) {
            screen_animate_ribbon(scr, 255, 0, 0, 255, 0, 0, start_xv);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == '[') {
        /* Move active window left (Cmd+Ctrl+Left) */
        uint8_t from_idx = scr->ribbon.active_idx;
        if (from_idx > 0 && from_idx < scr->ribbon.count) {
            uint8_t to_idx = (uint8_t)(from_idx - 1u);
            int32_t a_start_rx0 = scr->ribbon.wins[from_idx].rx0;
            int32_t a_start_rx1 = scr->ribbon.wins[from_idx].rx1;
            int32_t b_start_rx0 = scr->ribbon.wins[to_idx].rx0;
            int32_t b_start_rx1 = scr->ribbon.wins[to_idx].rx1;
            int32_t start_xv = scr->ribbon.x_view;
            if (ribbon_move(&scr->ribbon, from_idx, -1)) {
                screen_animate_ribbon(scr, to_idx, a_start_rx0, a_start_rx1,
                                           from_idx, b_start_rx0, b_start_rx1,
                                           start_xv);
                status = 0;
            } else {
                status = 1;
            }
        } else {
            status = 1;
        }
    } else if (op == ']') {
        /* Move active window right (Cmd+Ctrl+Right) */
        uint8_t from_idx = scr->ribbon.active_idx;
        if (from_idx + 1 < scr->ribbon.count) {
            uint8_t to_idx = (uint8_t)(from_idx + 1u);
            int32_t a_start_rx0 = scr->ribbon.wins[from_idx].rx0;
            int32_t a_start_rx1 = scr->ribbon.wins[from_idx].rx1;
            int32_t b_start_rx0 = scr->ribbon.wins[to_idx].rx0;
            int32_t b_start_rx1 = scr->ribbon.wins[to_idx].rx1;
            int32_t start_xv = scr->ribbon.x_view;
            if (ribbon_move(&scr->ribbon, from_idx, 1)) {
                screen_animate_ribbon(scr, to_idx, a_start_rx0, a_start_rx1,
                                           from_idx, b_start_rx0, b_start_rx1,
                                           start_xv);
                status = 0;
            } else {
                status = 1;
            }
        } else {
            status = 1;
        }
    } else if (op == 'J') {
        /* Jump directly to window N (Cmd+1..9) */
        uint8_t target = (n >= 2) ? req[1] : 0;
        int32_t start_xv = scr->ribbon.x_view;
        if (ribbon_focus(&scr->ribbon, target)) {
            screen_animate_ribbon(scr, 255, 0, 0, 255, 0, 0, start_xv);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'P') {
        /* 47.12: a tap at (x, y) in screen pixels. On the menu bar: the
         * minimap block nearest to it; below: the window under it. Focuses
         * that window; tapping the focused one is a no-op that succeeds. */
        int px = arg16(req, n, 0), py = arg16(req, n, 1);
        int idx = py < SCREEN_MENU_H ? minimap_hit(scr, px) : window_at(scr, px);
        /* Check for Macintosh close box tap on title bar */
        if (idx >= 0 && py >= SCREEN_TILE_Y && py <= SCREEN_TILE_Y + 16) {
            int32_t vx0 = scr->ribbon.wins[idx].rx0 - scr->ribbon.x_view;
            if (px >= vx0 + 4 && px <= vx0 + 22) {
                if (close_window(scr, (uint8_t)idx, reply)) {
                    reply[0] = 0;
                    reply[1] = window_vterm(scr, scr->ribbon.active_idx);
                    reply[8] = 0xCC; /* Window closed flag */
                    return;
                }
            }
        }
        if (idx >= 0 && idx == (int)scr->ribbon.active_idx) {
            const ribbon_win_t *w = &scr->ribbon.wins[idx];
            if (w->type == RIBBON_WIN_READER) {
                int cx = px - (w->rx0 - scr->ribbon.x_view);
                int cy = py - (SCREEN_TILE_Y + 17);
                reply[0] = 0;
                reply[1] = w->vterm_id;
                reply[2] = (uint8_t)(cx & 0xff);
                reply[3] = (uint8_t)((cx >> 8) & 0xff);
                reply[4] = (uint8_t)(cy & 0xff);
                reply[5] = (uint8_t)((cy >> 8) & 0xff);
                return;
            }
            status = 0;
        } else if (idx >= 0) {
            int32_t start_xv = scr->ribbon.x_view;
            if (ribbon_focus(&scr->ribbon, (uint8_t)idx)) {
                screen_animate_ribbon(scr, 255, 0, 0, 255, 0, 0, start_xv);
                status = 0;
            } else {
                status = 1;
            }
        } else {
            status = 1;
        }
    } else if (op == 'O') {
        /* Open or focus reader window:
         * req[1] = desired cols (0 = default)
         * req[2..n] = optional path
         * Returns: reply[1]=rid, reply[2..3]=width, reply[4..5]=height */
        char path_buf[128];
        const char *rpath = NULL;
        if (n >= 3 && req[2] != '\0') {
            uint32_t pl = 0;
            while (2u + pl < n && pl + 1u < sizeof(path_buf) && req[2u + pl]) {
                path_buf[pl] = (char)req[2u + pl];
                pl++;
            }
            path_buf[pl] = '\0';
            rpath = path_buf;
        }

        /* Check if reader with this path is already open in ribbon */
        if (rpath && *rpath) {
            for (uint8_t i = 0; i < scr->ribbon.count; i++) {
                if (scr->ribbon.wins[i].type == RIBBON_WIN_READER) {
                    uint8_t r_id = scr->ribbon.wins[i].vterm_id;
                    if (r_id < SCREEN_MAX_READERS && scr->readers[r_id].in_use &&
                        cstr_eq(scr->readers[r_id].path, rpath)) {
                        ribbon_focus(&scr->ribbon, i);
                        scr->ribbon.x_view = scr->ribbon.x_target;
                        (void)place(scr, scr->layout);
                        draw_all_from(scr, true);
                        reply[0] = 0;
                        reply[1] = r_id;
                        const ribbon_win_t *rw = &scr->ribbon.wins[i];
                        int r_w = (int)(rw->rx1 - rw->rx0 - 1);
                        int r_h = (int)(SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
                        reply[2] = (uint8_t)(r_w & 0xff); reply[3] = (uint8_t)((r_w >> 8) & 0xff);
                        reply[4] = (uint8_t)(r_h & 0xff); reply[5] = (uint8_t)((r_h >> 8) & 0xff);
                        return;
                    }
                }
            }
        }

        int rid = screen_alloc_reader(scr, rpath);
        if (rid < 0) {
            status = 1;
        } else {
            uint8_t text_cols = (scr->cv.w < scr->cv.h) ? (uint8_t)RIBBON_COLS_FULL(scr->cv.w) : (uint8_t)RIBBON_COLS_HALF(scr->cv.w);
            uint8_t reader_cols = (n >= 2 && req[1]) ? req[1] : (uint8_t)((scr->cv.w < scr->cv.h) ? (uint8_t)RIBBON_COLS_FULL(scr->cv.w) : (uint8_t)(scr->full_cols - text_cols - 2u));
            int t_win = ribbon_find_vterm(&scr->ribbon, vid);
            if (t_win >= 0) {
                scr->ribbon.wins[t_win].cols = text_cols;
            } else if (scr->ribbon.active_idx < scr->ribbon.count) {
                scr->ribbon.wins[scr->ribbon.active_idx].cols = text_cols;
                t_win = (int)scr->ribbon.active_idx;
            }
            uint8_t insert_idx = (t_win >= 0) ? (uint8_t)(t_win + 1) : scr->ribbon.count;
            char def_rtitle[8];
            def_rtitle[0] = 'R'; def_rtitle[1] = 'e'; def_rtitle[2] = 'a';
            def_rtitle[3] = 'd'; def_rtitle[4] = 'e'; def_rtitle[5] = 'r';
            def_rtitle[6] = '\0';
            const char *rtitle = scr->readers[rid].title[0] ? scr->readers[rid].title : def_rtitle;
            int idx = ribbon_insert(&scr->ribbon, insert_idx, RIBBON_WIN_READER, (uint8_t)rid, reader_cols, rtitle);
            if (idx >= 0) {
                ribbon_focus(&scr->ribbon, (uint8_t)idx);
                scr->ribbon.x_view = scr->ribbon.x_target;
                (void)place(scr, scr->layout);
                draw_all_from(scr, true);
                status = 0;
                reply[0] = 0;
                reply[1] = (uint8_t)rid;
                const ribbon_win_t *rw = &scr->ribbon.wins[idx];
                int r_w = (int)(rw->rx1 - rw->rx0 - 1);
                int r_h = (int)(SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
                reply[2] = (uint8_t)(r_w & 0xff); reply[3] = (uint8_t)((r_w >> 8) & 0xff);
                reply[4] = (uint8_t)(r_h & 0xff); reply[5] = (uint8_t)((r_h >> 8) & 0xff);
                return;
            } else {
                screen_free_reader(scr, (uint8_t)rid);
                status = 1;
            }
        }
    } else if (op == 'V') {
        /* Focus window by vterm ID */
        uint8_t f_vid = (n >= 2) ? req[1] : 0;
        int idx = ribbon_find_vterm(&scr->ribbon, f_vid);
        int32_t start_xv = scr->ribbon.x_view;
        if (idx >= 0 && ribbon_focus(&scr->ribbon, (uint8_t)idx)) {
            screen_animate_ribbon(scr, 255, 0, 0, 255, 0, 0, start_xv);
            status = 0;
        } else {
            status = 1;
        }
    } else if (op == 'R') {
        /* the ribbon. Bare: the window count, the focus and the
         * viewport. With an index: that window -- one per request, because
         * the reply is SCREEN_REPLY_LEN bytes and seven of them after the
         * header left no room for even one window, so `ribbon` listed none. */
        if (n >= 2) {
            uint8_t i = req[1];
            if (i >= scr->ribbon.count) {
                reply[0] = 1;
                return;
            }
            const ribbon_win_t *w = &scr->ribbon.wins[i];
            reply[0] = 0;
            reply[1] = w->type;
            reply[2] = w->vterm_id;
            reply[3] = w->cols;
            reply[4] = (uint8_t)(w->rx0 & 0xff);
            reply[5] = (uint8_t)((w->rx0 >> 8) & 0xff);
            reply[6] = (uint8_t)(w->rx1 & 0xff);
            reply[7] = (uint8_t)((w->rx1 >> 8) & 0xff);
            reply[8] = window_vterm(scr, i);
            uint8_t fmt = CANVAS_FMT_1BPP;
            if (w->type == RIBBON_WIN_CANVAS && w->vterm_id < SCREEN_MAX_CANVASES) {
                fmt = scr->canvases[w->vterm_id].format;
            }
            reply[9] = fmt;
            int r_h = (int)(SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
            reply[10] = (uint8_t)(r_h & 0xff);
            reply[11] = (uint8_t)((r_h >> 8) & 0xff);
            return;
        }
        reply[0] = 0;
        reply[1] = scr->ribbon.count;
        reply[2] = scr->ribbon.active_idx;
        reply[3] = (uint8_t)(scr->ribbon.x_view & 0xff);
        reply[4] = (uint8_t)((scr->ribbon.x_view >> 8) & 0xff);
        reply[5] = (uint8_t)(scr->ribbon.x_target & 0xff);
        reply[6] = (uint8_t)((scr->ribbon.x_target >> 8) & 0xff);
        return;
    } else if (op == 'C') {
        /* Close active window (Cmd+W) */
        status = close_window(scr, scr->ribbon.active_idx, reply) ? 0 : 1;
    } else if (op == 'Q') {
        /* close terminal `vid`'s window -- its vterm is gone
         * (vterm_destroy()), however it went: `exit`, `vterm close`. */
        int idx = (n >= 2) ? ribbon_find_vterm(&scr->ribbon, req[1]) : -1;
        status = (idx >= 0 && close_window(scr, (uint8_t)idx, reply)) ? 0 : 1;
    } else if (op == 'K') {
        int cid = screen_find_canvas_by_vterm(scr, vid);
        if (cid >= 0) {
            scr->canvases[cid].locked = (n >= 2 && req[1]) ? true : false;
        }
        scr->locked = (n >= 2 && req[1]) ? 1 : 0;
    } else if (op == 'Z') {
        scr->store_ok = 0;              /* 38.8 */
        scr->redrawing = 0;
        for (uint32_t i = 0; i < SCREEN_MAX_CANVASES; i++) scr->canvases[i].drawn_w = 0;
    } else if (op == 'D') {
        scr->redrawing = (n >= 2 && req[1]) ? 1 : 0;
    } else if (op == 'S' || op == 0) {
        /* The size, which every reply carries (below). */
    } else if (!has_canvas(scr)) {
        status = 1;
    } else {
        int cid = canvas_for(scr, vid);

        if (cid < 0 || !scr->canvases[cid].in_use) {
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
                cw_h = (int)(SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
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

#if CANVAS_SUPPORT_RGB565
            screen_canvas_slot_t *slot = &scr->canvases[cid];
            bool is_rgb565 = (slot->format == CANVAS_FMT_RGB565 && slot->buf565 != NULL);
            if (is_rgb565) {
                uint16_t *c565 = slot->buf565;
                uint32_t c565_stride = slot->stride565;
                if (op == 'F') {
                    uint16_t col = uarg16(req, n, 0);
                    canvas565_fill(c565, c565_stride, 0, 0, cw_w - 1, cw_h - 1, col);
                    if (cc_back) canvas1_fill(cc_back, 0, 0, cw_w - 1, cw_h - 1, mono_fallback(col));
                    if (cc_scr) canvas1_fill(cc_scr, 0, 0, cw_w - 1, cw_h - 1, mono_fallback(col));
                } else if (op == 'p') {
                    uint16_t col = uarg16(req, n, 2);
                    canvas565_pixel(c565, c565_stride, cw_w, cw_h, a, b, col);
                    if (cc_back) canvas1_fill(cc_back, a, b, a, b, mono_fallback(col));
                    if (cc_scr) canvas1_fill(cc_scr, a, b, a, b, mono_fallback(col));
                } else if (op == 'l') {
                    uint16_t col = uarg16(req, n, 4);
                    canvas565_line(c565, c565_stride, cw_w, cw_h, a, b, c, d, col);
                    if (cc_back) canvas1_line(cc_back, a, b, c, d, mono_fallback(col));
                    if (cc_scr) canvas1_line(cc_scr, a, b, c, d, mono_fallback(col));
                } else if (op == 'r') {
                    if (c > 0 && d > 0) {
                        uint16_t col = uarg16(req, n, 4);
                        canvas565_fill(c565, c565_stride, a, b, a + c - 1, b + d - 1, col);
                        if (cc_back) canvas1_fill(cc_back, a, b, a + c - 1, b + d - 1, mono_fallback(col));
                        if (cc_scr) canvas1_fill(cc_scr, a, b, a + c - 1, b + d - 1, mono_fallback(col));
                    }
                } else if (op == 'o') {
                    if (c > 0 && d > 0) {
                        uint16_t col = uarg16(req, n, 4);
                        canvas565_rect(c565, c565_stride, cw_w, cw_h, a, b, c, d, col);
                        unsigned k = mono_fallback(col);
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
                    uint16_t col = uarg16(req, n, 3);
                    canvas565_circle(c565, c565_stride, cw_w, cw_h, a, b, c, col);
                    if (cc_back) canvas1_circle(cc_back, a, b, c, mono_fallback(col));
                    if (cc_scr) canvas1_circle(cc_scr, a, b, c, mono_fallback(col));
                } else if (op == 'i') {
                    if (c > 0 && d > 0) {
                        canvas565_invert(c565, c565_stride, cw_w, cw_h, a, b, a + c - 1, b + d - 1);
                        if (cc_back) canvas1_invert(cc_back, a, b, a + c - 1, b + d - 1);
                        if (cc_scr) canvas1_invert(cc_scr, a, b, a + c - 1, b + d - 1);
                    }
                } else if (op == 't') {
                    if (n >= 6) {
                        const char *s = (const char *)req + 6;
                        uint32_t len = n - 6u;
                        unsigned tlen = canvas1_text_len(s, len);
                        canvas565_text(c565, c565_stride, cw_w, cw_h, a, b, s, len, 0x0000, 0xFFFF, true, req[5] != 0);
                        if (cc_back) canvas1_text(cc_back, a, b, s, len, tlen, 0, FONT8X16_H, req[5] != 0);
                        if (cc_scr) canvas1_text(cc_scr, a, b, s, len, tlen, 0, FONT8X16_H, req[5] != 0);
                    }
                } else if (op == 'x') {
                    if (n >= 10) {
                        uint16_t fg = uarg16(req, n, 2);
                        uint16_t bg = uarg16(req, n, 3);
                        uint8_t flags = req[9];
                        bool transparent = (flags & 1u) != 0;
                        bool bold = (flags & 2u) != 0;
                        const char *s = (const char *)req + 10;
                        uint32_t len = n - 10u;
                        unsigned tlen = canvas1_text_len(s, len);
                        canvas565_text(c565, c565_stride, cw_w, cw_h, a, b, s, len, fg, bg, transparent, bold);
                        if (cc_back) canvas1_text(cc_back, a, b, s, len, tlen, 0, FONT8X16_H, bold);
                        if (cc_scr) canvas1_text(cc_scr, a, b, s, len, tlen, 0, FONT8X16_H, bold);
                    } else {
                        status = 1;
                    }
                } else if (op == 'B') {
                    if (c > 0 && d > 0 && n >= 9u + (uint32_t)c * (uint32_t)d * sizeof(uint16_t)) {
                        const uint16_t *src_pix = (const uint16_t *)(req + 9);
                        for (int row = 0; row < d; row++) {
                            int py = b + row;
                            if (py >= 0 && py < cw_h) {
                                int x0 = a < 0 ? 0 : a;
                                int x1 = (a + c - 1) >= cw_w ? cw_w - 1 : a + c - 1;
                                if (x0 <= x1) {
                                    const uint16_t *src_line = src_pix + (size_t)row * (size_t)c + (size_t)(x0 - a);
                                    uint16_t *dst_line = c565 + (size_t)py * c565_stride + (size_t)x0;
                                    int copy_w = x1 - x0 + 1;
                                    for (int k = 0; k < copy_w; k++) {
                                        dst_line[k] = src_line[k];
                                    }
                                }
                            }
                        }
                    } else {
                        status = 1;
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
                    if (a >= 0 && a < cw_w && b >= 0 && b < cw_h) {
                        pixel = (c565[(size_t)b * c565_stride + (size_t)a] != 0xFFFF) ? 1 : 0;
                    }
                } else {
                    status = 1;
                }
            } else
#endif
            {
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
            }

            if (op != 'g' && status == 0 && !scr->redrawing) scr->draw_gen++;
            if (op != 'g' && status == 0 && cc_back) scr->canvases[cid].drawn_w = (uint16_t)cw_w;
        }
    }
    reply[0] = status;
    if (op != 'g' && scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
        reply[1] = window_vterm(scr, scr->ribbon.active_idx);
    } else {
        reply[1] = pixel;
    }
    /* The canvas's size, after whatever this request did to it. Every
     * reply, as screen.h promises: only 'S' and the drawing requests used
     * to fill it, so a layout change answered with the size the previous
     * reply had left in the caller's buffer, and a request with no canvas
     * with whatever was there. */
    int sw = 0, sh = 0;
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
        const ribbon_win_t *aw = &scr->ribbon.wins[scr->ribbon.active_idx];
        if (aw->type == RIBBON_WIN_READER) {
            sw = (int)(aw->rx1 - aw->rx0 - 1);
            sh = (int)(SCREEN_TEXT_Y + (int)FONT_CELL_H * (int)SCREEN_TEXT_ROWS(scr->cv.h) + 1 - (SCREEN_TILE_Y + 17));
            if (sw < 0) sw = 0;
            if (sh < 0) sh = 0;
        } else {
            canvas_size(scr, canvas_for(scr, vid), &sw, &sh);
        }
    } else {
        canvas_size(scr, canvas_for(scr, vid), &sw, &sh);
    }
    reply[2] = (uint8_t)sw; reply[3] = (uint8_t)(sw >> 8);
    reply[4] = (uint8_t)sh; reply[5] = (uint8_t)(sh >> 8);
    reply[6] = (uint8_t)scr->damage; reply[7] = (uint8_t)(scr->damage >> 8);
    reply[8] = scr->layout;
    reply[9] = scr->swapped;
    if (op != 'C') {
        if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count) {
            reply[10] = scr->ribbon.wins[scr->ribbon.active_idx].type;
            reply[11] = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
        } else {
            reply[10] = 0;
            reply[11] = 0;
        }
    }
}

LCDTERM_UTEXT void screen_canvas(screen_t *scr, const uint8_t *req, uint32_t n, uint8_t *reply) {
    uint8_t vid = 0;
    if (scr->ribbon.count > 0 && scr->ribbon.active_idx < scr->ribbon.count &&
        scr->ribbon.wins[scr->ribbon.active_idx].type == RIBBON_WIN_TERM) {
        vid = scr->ribbon.wins[scr->ribbon.active_idx].vterm_id;
    }
    screen_canvas_vterm(scr, vid, req, n, reply);
}
