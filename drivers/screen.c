/*
 * The screen: a menu bar, the desktop, and the terminal in a framed tile --
 * 37.1/37.1a, plan/phase37_screen_layouts_and_apps.md §1.1. See
 * drivers/screen.h for the geometry.
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
}

/* The title bar: the frame's top border, a white row, six stripes with
 * white rows between, a white row, a border -- 17 rows -- and the title in
 * bold, centred to the pixel in a white box. The glyph's rows 1..14 are
 * drawn from one pixel below the border, which leaves capitals two white
 * rows above and three below (the owner's adjustment of the C2 mockup). */
LCDTERM_UTEXT static void draw_titlebar(screen_t *scr) {
    const canvas1_t *cv = &scr->cv;
    int x0 = scr->fx0, x1 = scr->fx1, y0 = scr->fy0;
    canvas1_fill(cv, x0 + 1, y0 + 1, x1 - 1, y0 + 15, CANVAS1_WHITE);
    canvas1_hline(cv, x0, x1, y0, CANVAS1_BLACK);
    canvas1_hline(cv, x0, x1, y0 + 16, CANVAS1_BLACK);
    for (int y = y0 + 3; y <= y0 + 13; y += 2) canvas1_hline(cv, x0 + 2, x1 - 2, y, CANVAS1_BLACK);
    const char *t = scr->vt.title;
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
    scr->title_drawn = scr->vt.title_seq;
}

/* The desktop, the tile's frame and shadow, its white interior, and both
 * bars. The text cells are the terminal's to draw (screen_repaint()). */
LCDTERM_UTEXT static void draw_chrome(screen_t *scr) {
    const canvas1_t *cv = &scr->cv;
    int x0 = scr->fx0, y0 = scr->fy0, x1 = scr->fx1, y1 = scr->fy1;
    canvas1_fill(cv, 0, SCREEN_MENU_H, cv->w - 1, cv->h - 1, CANVAS1_GREY);
    canvas1_fill(cv, x0, y0, x1, y1, CANVAS1_WHITE);
    canvas1_hline(cv, x0, x1, y1, CANVAS1_BLACK);
    canvas1_vline(cv, x0, y0, y1, CANVAS1_BLACK);
    canvas1_vline(cv, x1, y0, y1, CANVAS1_BLACK);
    canvas1_hline(cv, x0 + 1, x1 + 1, y1 + 1, CANVAS1_BLACK);   /* the shadow */
    canvas1_vline(cv, x1 + 1, y0 + 1, y1 + 1, CANVAS1_BLACK);
    draw_titlebar(scr);
    draw_menu(scr);
}

void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h) {
    canvas1_init(&scr->cv, fb, stride, w, h);
    unsigned cols = SCREEN_TEXT_COLS(w), rows = SCREEN_TEXT_ROWS(h);
    scr->fx0 = SCREEN_TEXT_X - 4;
    scr->fy0 = SCREEN_TILE_Y;
    scr->fx1 = (int16_t)(SCREEN_TEXT_X + 8 * cols + 2);
    scr->fy1 = (int16_t)(SCREEN_TEXT_Y + 16 * rows + 1);
    const char *name = "LugalOS";
    unsigned i = 0;
    for (; name[i] && i < SCREEN_NAME_MAX - 1u; i++) scr->name[i] = name[i];
    scr->name[i] = '\0';
    scr->right[0] = '\0';
    fbtext_t text;
    fbtext_init(&text, (uint8_t *)fb + SCREEN_TEXT_Y * stride + SCREEN_TEXT_X / 8, stride, cols, rows);
    /* The tile spans the screen, so beside its text are only the frame and
     * the desktop, which repeat every two rows: a scroll moves whole rows. */
    text.xbyte = SCREEN_TEXT_X / 8;
    text.whole_rows = true;
    vtterm_init(&scr->vt, &text, scr->shadow);
    vtterm_set_title(&scr->vt, name, 7);
    screen_repaint(scr);
}

LCDTERM_UTEXT void screen_write(screen_t *scr, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) vtterm_putc(&scr->vt, s[i]);
    if (scr->vt.title_seq != scr->title_drawn) draw_titlebar(scr);
}

LCDTERM_UTEXT void screen_set_title(screen_t *scr, const char *s, uint32_t n) {
    vtterm_set_title(&scr->vt, s, n);
    if (scr->vt.title_seq != scr->title_drawn) draw_titlebar(scr);
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
    draw_chrome(scr);
    vtterm_repaint(&scr->vt);
}

LCDTERM_UTEXT void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows) {
    *cols = scr->vt.text.cols;
    *rows = scr->vt.text.rows;
}
