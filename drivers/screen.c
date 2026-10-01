/*
 * The screen: a status bar above the terminal -- 37.1,
 * plan/phase37_screen_layouts_and_apps.md §1. See drivers/screen.h.
 *
 * Everything but screen_init() runs in the U-mode `lcdterm` task on the
 * RP2350-LCD-7, so it is LCDTERM_UTEXT, uses no libc and no string literals
 * (a literal is .rodata, outside the task's domain). screen_init() runs once,
 * in the kernel, before the task exists.
 */

#include "drivers/screen.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"

/* One code point from s[*i..n), advancing *i. A malformed or truncated
 * sequence is one replacement character, and *i moves past what it took. */
LCDTERM_UTEXT static uint32_t utf8_next(const char *s, uint32_t n, uint32_t *i) {
    unsigned char c = (unsigned char)s[(*i)++];
    if (c < 0x80u) return c;
    uint32_t cp;
    unsigned need;
    if ((c & 0xe0u) == 0xc0u)      { cp = c & 0x1fu; need = 1; }
    else if ((c & 0xf0u) == 0xe0u) { cp = c & 0x0fu; need = 2; }
    else if ((c & 0xf8u) == 0xf0u) { cp = c & 0x07u; need = 3; }
    else return FONT8X16_REPLACEMENT;
    while (need--) {
        if (*i >= n || ((unsigned char)s[*i] & 0xc0u) != 0x80u) return FONT8X16_REPLACEMENT;
        cp = (cp << 6) | ((unsigned char)s[(*i)++] & 0x3fu);
    }
    return cp;
}

LCDTERM_UTEXT static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

LCDTERM_UTEXT static unsigned cp_count(const char *s) {
    uint32_t n = cstr_len(s), i = 0;
    unsigned k = 0;
    while (i < n) { (void)utf8_next(s, n, &i); k++; }
    return k;
}

/* At most `max` characters of `s`, inverted, from `col` of the bar. */
LCDTERM_UTEXT static void bar_text(fbtext_t *bar, unsigned col, unsigned max, const char *s) {
    uint32_t n = cstr_len(s), i = 0;
    for (unsigned k = 0; k < max && i < n; k++)
        fbtext_putcp(bar, col + k, 0, utf8_next(s, n, &i), true);
}

/* The bar: a space, the title, and the indicators ending one cell short of
 * the right edge -- the panel's last pixel column is under the bezel (phase
 * 36 §1.5), so a glyph there would lose its edge. The indicators win when the
 * two do not fit; the title is cut, never the clock. */
LCDTERM_UTEXT static void draw_status(screen_t *scr) {
    fbtext_t bar;
    fbtext_init(&bar, scr->fb, scr->stride, scr->cols, 1);
    for (unsigned col = 0; col < scr->cols; col++) fbtext_putcode(&bar, col, 0, ' ', true);
    unsigned rlen = cp_count(scr->right);
    if (rlen > scr->cols - 2u) rlen = scr->cols - 2u;
    unsigned rstart = scr->cols - 1u - rlen;
    bar_text(&bar, rstart, rlen, scr->right);
    unsigned tmax = rstart > 3u ? rstart - 3u : 0;   /* col 1 .. two cells before the right */
    bar_text(&bar, 1, tmax, scr->vt.title);
    scr->title_drawn = scr->vt.title_seq;
}

void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned cols, unsigned rows) {
    scr->fb = (uint8_t *)fb;
    scr->stride = stride;
    scr->cols = (uint16_t)cols;
    scr->rows = (uint16_t)rows;
    scr->right[0] = '\0';
    fbtext_t text;
    fbtext_init(&text, scr->fb + FONT8X16_H * stride, stride, cols, rows - 1u);
    vtterm_init(&scr->vt, &text, scr->shadow);
    vtterm_set_title(&scr->vt, "LugalOS", 7);
    draw_status(scr);
}

LCDTERM_UTEXT void screen_write(screen_t *scr, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) vtterm_putc(&scr->vt, s[i]);
    if (scr->vt.title_seq != scr->title_drawn) draw_status(scr);
}

LCDTERM_UTEXT void screen_set_title(screen_t *scr, const char *s, uint32_t n) {
    vtterm_set_title(&scr->vt, s, n);
    if (scr->vt.title_seq != scr->title_drawn) draw_status(scr);
}

LCDTERM_UTEXT void screen_set_right(screen_t *scr, const char *s, uint32_t n) {
    if (n > SCREEN_RIGHT_MAX - 1u) {
        n = SCREEN_RIGHT_MAX - 1u;
        while (n > 0 && ((unsigned char)s[n] & 0xc0u) == 0x80u) n--;   /* not mid-character */
    }
    for (uint32_t i = 0; i < n; i++) scr->right[i] = s[i];
    scr->right[n] = '\0';
    draw_status(scr);
}

LCDTERM_UTEXT void screen_repaint(screen_t *scr) {
    draw_status(scr);
    vtterm_repaint(&scr->vt);
}

LCDTERM_UTEXT void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows) {
    *cols = scr->vt.text.cols;
    *rows = scr->vt.text.rows;
}
