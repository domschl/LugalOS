/*
 * A terminal emulator for the 1-bpp screen -- 36.6,
 * plan/phase36_rp2350_lcd7_terminal.md §4.3. See drivers/vtterm.h for what it
 * understands and why it is exactly that much.
 *
 * The cursor is an XOR underline (fbtext_cursor_xor()). It is taken off the
 * screen before anything draws and put back afterwards, so it can never be
 * left inside a glyph or survive a scroll in the wrong row.
 */

#include "drivers/vtterm.h"
#include "drivers/screen.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"
#include "kernel/console.h"
#include "kernel/scratch.h"


enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC };

/* --- The grid, with its shadow (37.1) ---------------------------------------
 *
 * Every draw goes through these four, so the shadow can never disagree with
 * the pixels: each does the fbtext operation and then the same thing to the
 * shadow, with the same bounds. */
LCDTERM_UTEXT static void cell_put(vtterm_t *vt, unsigned col, unsigned row, uint8_t code, bool inverse) {
    fbtext_putcode(&vt->text, col, row, code, inverse);
    if (vt->shadow && col < vt->text.cols && row < vt->text.rows)
        vt->shadow[row * vt->text.cols + col] = (uint16_t)(code | (inverse ? VT_CELL_INVERSE : 0u));
}

LCDTERM_UTEXT static void shadow_blank(vtterm_t *vt, unsigned from, unsigned n) {
    for (unsigned i = 0; i < n; i++) vt->shadow[from + i] = ' ';
}

LCDTERM_UTEXT static void span_clear(vtterm_t *vt, unsigned row, unsigned col0, unsigned col1) {
    fbtext_clear_span(&vt->text, row, col0, col1);
    if (!vt->shadow || row >= vt->text.rows) return;
    if (col1 > vt->text.cols) col1 = vt->text.cols;
    if (col0 < col1) shadow_blank(vt, row * vt->text.cols + col0, col1 - col0);
}

LCDTERM_UTEXT static void rows_clear(vtterm_t *vt, unsigned row, unsigned n) {
    fbtext_clear_rows(&vt->text, row, n);
    if (!vt->shadow || row >= vt->text.rows) return;
    if (n > (unsigned)vt->text.rows - row) n = vt->text.rows - row;
    shadow_blank(vt, row * vt->text.cols, n * vt->text.cols);
}

LCDTERM_UTEXT static void grid_scroll(vtterm_t *vt) {
    fbtext_scroll_up(&vt->text, 1);
    if (!vt->shadow) return;
    unsigned cols = vt->text.cols, cells = (unsigned)(vt->text.rows - 1u) * cols;
    for (unsigned i = 0; i < cells; i++) vt->shadow[i] = vt->shadow[i + cols];
    shadow_blank(vt, cells, cols);
}

LCDTERM_UTEXT static void cursor_hide(vtterm_t *vt) {
    if (vt->cursor_drawn) {
        fbtext_cursor_xor(&vt->text, vt->col, vt->row);
        vt->cursor_drawn = false;
    }
}

LCDTERM_UTEXT static void cursor_show(vtterm_t *vt) {
    if (vt->cursor_on && !vt->cursor_drawn) {
        fbtext_cursor_xor(&vt->text, vt->col, vt->row);
        vt->cursor_drawn = true;
    }
}

LCDTERM_UTEXT void vtterm_init(vtterm_t *vt, const fbtext_t *text, uint16_t *shadow) {
    /* Field by field, not memset() and a struct copy: this is in the U-mode
     * section, and should not lean on libc that the section cannot reach. */
    vt->text.fb = text->fb;
    vt->text.stride = text->stride;
    vt->text.cols = text->cols;
    vt->text.rows = text->rows;
    vt->shadow = shadow;
    vt->col = vt->row = 0;
    vt->pending_wrap = vt->inverse = vt->cursor_drawn = vt->private_mode = false;
    vt->state = vt->nparams = vt->utf8_need = 0;
    vt->utf8_cp = vt->unknown = 0;
    for (unsigned i = 0; i < 8u; i++) vt->params[i] = 0;
    vt->osc_len = 0;
    vt->osc_esc = false;
    vt->title[0] = '\0';
    vt->title_seq = 0;
    vt->cursor_on = true;
    rows_clear(vt, 0, vt->text.rows);
    cursor_show(vt);
}

LCDTERM_UTEXT void vtterm_repaint(vtterm_t *vt) {
    if (!vt->shadow) return;
    vt->cursor_drawn = false;              /* its pixels are about to be redrawn */
    unsigned cols = vt->text.cols;
    for (unsigned row = 0; row < vt->text.rows; row++) {
        for (unsigned col = 0; col < cols; col++) {
            uint16_t v = vt->shadow[row * cols + col];
            fbtext_putcode(&vt->text, col, row, (uint8_t)v, (v & VT_CELL_INVERSE) != 0);
        }
    }
    cursor_show(vt);
}

LCDTERM_UTEXT void vtterm_set_title(vtterm_t *vt, const char *s, uint32_t n) {
    if (n > VT_TITLE_MAX - 1u) {
        n = VT_TITLE_MAX - 1u;
        while (n > 0 && ((unsigned char)s[n] & 0xc0u) == 0x80u) n--;   /* not mid-character */
    }
    bool same = vt->title[n] == '\0';
    for (uint32_t i = 0; i < n && same; i++) same = (vt->title[i] == s[i]);
    if (same) return;
    for (uint32_t i = 0; i < n; i++) vt->title[i] = s[i];
    vt->title[n] = '\0';
    vt->title_seq++;
}

LCDTERM_UTEXT static void line_feed(vtterm_t *vt) {
    if (vt->row + 1u < vt->text.rows) vt->row++;
    else grid_scroll(vt);
}

LCDTERM_UTEXT static void put_cp(vtterm_t *vt, uint32_t cp) {
    if (vt->pending_wrap) {
        vt->col = 0;
        line_feed(vt);
        vt->pending_wrap = false;
    }
    cell_put(vt, vt->col, vt->row, fbtext_code(cp), vt->inverse);
    if (vt->col + 1u < vt->text.cols) vt->col++;
    else vt->pending_wrap = true;
}

LCDTERM_UTEXT static unsigned param(const vtterm_t *vt, unsigned i, unsigned dflt) {
    return (i < vt->nparams && vt->params[i] != 0) ? vt->params[i] : dflt;
}

LCDTERM_UTEXT static unsigned clampu(unsigned v, unsigned hi) {
    return v > hi ? hi : v;
}

LCDTERM_UTEXT static void sgr(vtterm_t *vt) {
    if (vt->nparams == 0) {
        vt->inverse = false;
        return;
    }
    for (unsigned i = 0; i < vt->nparams; i++) {
        unsigned p = vt->params[i];
        if (p == 0 || p == 27) vt->inverse = false;
        else if (p == 7) vt->inverse = true;
        else if (p == 38 || p == 48) {
            /* Extended colour: skip its arguments rather than reading them as
             * attributes -- a truecolour 7 must not turn on reverse video. */
            if (i + 1u < vt->nparams && vt->params[i + 1u] == 5) i += 2;
            else if (i + 1u < vt->nparams && vt->params[i + 1u] == 2) i += 4;
        }
        /* everything else -- bold, colours -- has no 1-bpp rendering */
    }
}

LCDTERM_UTEXT static void csi_final(vtterm_t *vt, char f) {
    unsigned cols = vt->text.cols, rows = vt->text.rows;
    if (vt->private_mode) {
        if ((f == 'h' || f == 'l') && vt->nparams >= 1 && vt->params[0] == 25) vt->cursor_on = (f == 'h');
        else vt->unknown++;
        return;
    }
    switch (f) {
    case 'A': vt->row = (uint16_t)(vt->row >= param(vt, 0, 1) ? vt->row - param(vt, 0, 1) : 0); break;
    case 'B': vt->row = (uint16_t)clampu(vt->row + param(vt, 0, 1), rows - 1u); break;
    case 'C': vt->col = (uint16_t)clampu(vt->col + param(vt, 0, 1), cols - 1u); break;
    case 'D': vt->col = (uint16_t)(vt->col >= param(vt, 0, 1) ? vt->col - param(vt, 0, 1) : 0); break;
    case 'H':
    case 'f':
        vt->row = (uint16_t)clampu(param(vt, 0, 1) - 1u, rows - 1u);
        vt->col = (uint16_t)clampu(param(vt, 1, 1) - 1u, cols - 1u);
        break;
    case 'K': {
        unsigned m = vt->nparams ? vt->params[0] : 0;
        if (m == 0) span_clear(vt, vt->row, vt->col, cols);
        else if (m == 1) span_clear(vt, vt->row, 0, vt->col + 1u);
        else span_clear(vt, vt->row, 0, cols);
        break;
    }
    case 'J': {
        unsigned m = vt->nparams ? vt->params[0] : 0;
        if (m == 0) {
            span_clear(vt, vt->row, vt->col, cols);
            rows_clear(vt, vt->row + 1u, rows - vt->row - 1u);
        } else if (m == 1) {
            rows_clear(vt, 0, vt->row);
            span_clear(vt, vt->row, 0, vt->col + 1u);
        } else {
            rows_clear(vt, 0, rows);
        }
        break;
    }
    case 'm': sgr(vt); return;             /* SGR leaves a pending wrap alone */
    default:  vt->unknown++; return;
    }
    vt->pending_wrap = false;              /* every cursor movement cancels it */
}

LCDTERM_UTEXT static void ground(vtterm_t *vt, unsigned char c) {
    if (vt->utf8_need) {
        if ((c & 0xc0u) == 0x80u) {
            vt->utf8_cp = (vt->utf8_cp << 6) | (c & 0x3fu);
            if (--vt->utf8_need == 0) put_cp(vt, vt->utf8_cp);
            return;
        }
        vt->utf8_need = 0;                 /* broken sequence: mark it, then */
        put_cp(vt, FONT8X16_REPLACEMENT);  /* handle this byte on its own    */
    }
    if (c >= 0x80u) {
        if ((c & 0xe0u) == 0xc0u)      { vt->utf8_cp = c & 0x1fu; vt->utf8_need = 1; }
        else if ((c & 0xf0u) == 0xe0u) { vt->utf8_cp = c & 0x0fu; vt->utf8_need = 2; }
        else if ((c & 0xf8u) == 0xf0u) { vt->utf8_cp = c & 0x07u; vt->utf8_need = 3; }
        else put_cp(vt, FONT8X16_REPLACEMENT);
        return;
    }
    switch (c) {
    case '\r': vt->col = 0; vt->pending_wrap = false; return;
    case '\n': line_feed(vt); vt->pending_wrap = false; return;
    case '\b': if (vt->col > 0) vt->col--; vt->pending_wrap = false; return;
    case '\t':
        vt->col = (uint16_t)clampu((vt->col / 8u + 1u) * 8u, vt->text.cols - 1u);
        vt->pending_wrap = false;
        return;
    case 0x1b: vt->state = ST_ESC; return;
    default:
        if (c < 0x20u || c == 0x7fu) return;   /* BEL and other controls: nothing */
        put_cp(vt, c);
    }
}

/* An OSC is over (BEL, ST, or an ESC that starts something else). OSC 0 and
 * 2 are the title; any other is consumed and counted. */
LCDTERM_UTEXT static void osc_end(vtterm_t *vt) {
    unsigned ps = 0, i = 0;
    while (i < vt->osc_len && vt->osc[i] >= '0' && vt->osc[i] <= '9' && ps < 1000u)
        ps = ps * 10u + (unsigned)(vt->osc[i++] - '0');
    if (i > 0 && i < vt->osc_len && vt->osc[i] == ';' && (ps == 0 || ps == 2))
        vtterm_set_title(vt, vt->osc + i + 1u, vt->osc_len - i - 1u);
    else
        vt->unknown++;
    vt->state = ST_GROUND;
}

LCDTERM_UTEXT static void esc_byte(vtterm_t *vt, unsigned char c) {
    if (c == '[') {
        vt->state = ST_CSI;
        vt->nparams = 0;
        vt->private_mode = false;
        for (unsigned i = 0; i < 8u; i++) vt->params[i] = 0;   /* no libc in U-mode */
    } else if (c == ']') {
        vt->state = ST_OSC;
        vt->osc_len = 0;
        vt->osc_esc = false;
    } else {
        vt->state = ST_GROUND;             /* ESC x: two bytes, no effect */
        vt->unknown++;
    }
}

LCDTERM_UTEXT static void osc_byte(vtterm_t *vt, unsigned char c) {
    if (vt->osc_esc) {
        vt->osc_esc = false;
        osc_end(vt);                       /* ESC ends it, whatever follows */
        if (c != '\\') esc_byte(vt, c);   /* not ST: that ESC starts the next */
        return;
    }
    if (c == 0x07u) osc_end(vt);
    else if (c == 0x1bu) vt->osc_esc = true;
    else if (c == 0x18u || c == 0x1au) { vt->state = ST_GROUND; vt->unknown++; }   /* CAN, SUB */
    else if (c >= 0x20u && vt->osc_len < VT_OSC_MAX) vt->osc[vt->osc_len++] = (char)c;
    /* other controls, and bytes past VT_OSC_MAX, are dropped */
}

LCDTERM_UTEXT void vtterm_putc(vtterm_t *vt, char ch) {
    unsigned char c = (unsigned char)ch;
    cursor_hide(vt);
    switch (vt->state) {
    case ST_GROUND:
        ground(vt, c);
        break;
    case ST_ESC:
        esc_byte(vt, c);
        break;
    case ST_OSC:
        osc_byte(vt, c);
        break;
    case ST_CSI:
        if (c == '?' && vt->nparams == 0 && !vt->private_mode) {
            vt->private_mode = true;
        } else if (c >= '0' && c <= '9') {
            if (vt->nparams == 0) vt->nparams = 1;
            uint16_t *p = &vt->params[vt->nparams - 1u];
            if (*p < 10000u) *p = (uint16_t)(*p * 10u + (c - '0'));
        } else if (c == ';') {
            if (vt->nparams == 0) vt->nparams = 1;    /* a leading ';' means an empty first */
            if (vt->nparams < 8u) vt->nparams++;
        } else if (c >= 0x40u && c <= 0x7eu) {
            csi_final(vt, (char)c);
            vt->state = ST_GROUND;
        } else if (c < 0x20u) {
            /* A control inside a sequence acts and the sequence goes on (VT100). */
            ground(vt, c);
            vt->state = ST_CSI;
        } else {
            vt->unknown++;                 /* intermediates we do not use */
        }
        break;
    }
    cursor_show(vt);
}

LCDTERM_UTEXT void vtterm_write(vtterm_t *vt, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) vtterm_putc(vt, s[i]);
}

/* --- vtselftest --------------------------------------------------------------
 *
 * A 20 x 4 grid in RAM (160 x 64 pixels, 20 bytes a row). Every case starts
 * from a fresh emulator with the cursor switched off, feeds bytes, and compares
 * cells against fbtext's own rendering of what should be there. Pixels, not an
 * internal shadow: what is checked is what would be seen. */
#define ST_COLS 20u
#define ST_ROWS 4u

/* The self-test's state lives on its stack, not in statics: a command run
 * rarely should cost every persona nothing while it is not running. */
typedef struct {
    uint8_t  *buf;          /* the grid: ST_COLS x (ST_ROWS + 1) cells of pixels */
    uint8_t  *copy;         /* as much again, to compare a repaint against */
    uint16_t *shadow;       /* ST_COLS x ST_ROWS cells */
    screen_t *scr;          /* SCREEN_BYTES(ST_COLS, ST_ROWS + 1) */
    int fail, cases;
} st_ctx_t;

#define ST_GRID_BYTES (ST_COLS * (ST_ROWS + 1u) * FONT8X16_H)

static bool cell_is(vtterm_t *vt, unsigned col, unsigned row, uint32_t cp, bool inverse) {
    const uint8_t *g = fbtext_glyph(cp);
    const uint8_t *p = vt->text.fb + (uint32_t)row * FONT8X16_H * vt->text.stride + col;
    uint8_t x = inverse ? 0xffu : 0u;
    for (unsigned r = 0; r < FONT8X16_H; r++, p += vt->text.stride)
        if (*p != (uint8_t)(g[r] ^ x)) return false;
    return true;
}

static bool row_is(vtterm_t *vt, unsigned row, const char *s) {
    for (unsigned col = 0; col < ST_COLS; col++) {
        char c = *s ? *s++ : ' ';
        if (!cell_is(vt, col, row, (unsigned char)c, false)) return false;
    }
    return true;
}

static void st_fresh(st_ctx_t *cx, vtterm_t *vt) {
    fbtext_t t;
    fbtext_init(&t, cx->buf, ST_COLS, ST_COLS, ST_ROWS);
    vtterm_init(vt, &t, cx->shadow);
    vtterm_write(vt, "\033[?25l", 6);
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

/* A cell of an arbitrary grid (the screen's bar), against a code point. */
static bool grid_cell_is(const fbtext_t *t, unsigned col, unsigned row, uint32_t cp, bool inverse) {
    const uint8_t *g = fbtext_glyph(cp);
    const uint8_t *p = t->fb + (uint32_t)row * FONT8X16_H * t->stride + col;
    uint8_t x = inverse ? 0xffu : 0u;
    for (unsigned r = 0; r < FONT8X16_H; r++, p += t->stride)
        if (*p != (uint8_t)(g[r] ^ x)) return false;
    return true;
}

static void st_check(st_ctx_t *cx, const char *name, bool ok) {
    cx->cases++;
    if (!ok) cx->fail++;
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", name);
}

#define FEED(vt, s) vtterm_write((vt), (s), (uint32_t)(sizeof(s) - 1u))

int vtterm_selftest(void) {
    /* ~4 KB, on demand rather than in .bss (kernel/scratch.h's rule). */
    uint32_t shadow_off = 2u * ST_GRID_BYTES;
    uint32_t scr_off = (shadow_off + ST_COLS * ST_ROWS * 2u + 7u) & ~7u;
    scratch_t sc;
    if (!scratch_acquire(&sc, scr_off + SCREEN_BYTES(ST_COLS, ST_ROWS + 1u))) {
        cprintf("VTTERM_SELFTEST_FAIL (no memory for the test grid)\n");
        return 1;
    }
    vtterm_t vt;
    uint8_t *base = (uint8_t *)sc.base;
    st_ctx_t cx = { base, base + ST_GRID_BYTES, (uint16_t *)(void *)(base + shadow_off),
                    (screen_t *)(void *)(base + scr_off), 0, 0 };
    cprintf("vtterm selftest (%ux%u grid):\n", ST_COLS, ST_ROWS);

    st_fresh(&cx, &vt); FEED(&vt, "hello\r\nworld");
    st_check(&cx, "text, CR LF", row_is(&vt, 0, "hello") && row_is(&vt, 1, "world") && vt.row == 1 && vt.col == 5);

    st_fresh(&cx, &vt); FEED(&vt, "abcdefghijklmnopqrst");
    st_check(&cx, "last column leaves a pending wrap, not a new line",
             vt.row == 0 && vt.col == ST_COLS - 1u && vt.pending_wrap && row_is(&vt, 1, ""));

    st_fresh(&cx, &vt); FEED(&vt, "abcdefghijklmnopqrst\r\033[K");
    st_check(&cx, "CR after a full line: same row, and ESC[K clears it (line editor redraw)",
             vt.row == 0 && vt.col == 0 && row_is(&vt, 0, "") && row_is(&vt, 1, ""));

    st_fresh(&cx, &vt); FEED(&vt, "abcdefghijklmnopqrstU");
    st_check(&cx, "the next printable wraps", row_is(&vt, 1, "U") && vt.row == 1 && vt.col == 1);

    st_fresh(&cx, &vt); FEED(&vt, "1\r\n2\r\n3\r\n4\r\n5");
    st_check(&cx, "LF at the bottom scrolls", row_is(&vt, 0, "2") && row_is(&vt, 3, "5") && vt.row == 3);

    st_fresh(&cx, &vt); FEED(&vt, "abcdef\033[3D\033[K");
    st_check(&cx, "ESC[nD then ESC[K", row_is(&vt, 0, "abc") && vt.col == 3);

    st_fresh(&cx, &vt); FEED(&vt, "x\033[99C\033[99B\033[99A\033[99D");
    st_check(&cx, "cursor moves clamp at every edge", vt.row == 0 && vt.col == 0);

    st_fresh(&cx, &vt); FEED(&vt, "\033[3;5HZ\033[H*");
    st_check(&cx, "CUP 3;5 and bare H", cell_is(&vt, 4, 2, 'Z', false) && cell_is(&vt, 0, 0, '*', false));

    st_fresh(&cx, &vt); FEED(&vt, "aaaa\r\nbbbb\r\ncccc\033[2;3H\033[J");
    st_check(&cx, "ESC[J clears from the cursor down",
             row_is(&vt, 0, "aaaa") && row_is(&vt, 1, "bb") && row_is(&vt, 2, ""));

    st_fresh(&cx, &vt); FEED(&vt, "aaaa\r\nbbbb\033[2J");
    st_check(&cx, "ESC[2J clears all, cursor stays", row_is(&vt, 0, "") && row_is(&vt, 1, "") && vt.row == 1);

    st_fresh(&cx, &vt); FEED(&vt, "a\033[7mb\033[0mc\033[7md\033[27me");
    st_check(&cx, "SGR 7 / 0 / 27 reverse video",
             cell_is(&vt, 0, 0, 'a', false) && cell_is(&vt, 1, 0, 'b', true) &&
             cell_is(&vt, 2, 0, 'c', false) && cell_is(&vt, 3, 0, 'd', true) && cell_is(&vt, 4, 0, 'e', false));

    st_fresh(&cx, &vt); FEED(&vt, "\033[1;36mK\033[48;2;7;7;7mL\033[38;5;7mM\033[0m");
    st_check(&cx, "colours and bold are consumed, truecolour 7 is not reverse",
             row_is(&vt, 0, "KLM") && vt.unknown == 0);

    st_fresh(&cx, &vt); FEED(&vt, "\033[5n\033=\033[?1049hX");
    st_check(&cx, "unknown sequences are swallowed and counted, never printed",
             row_is(&vt, 0, "X") && vt.unknown == 3);

    st_fresh(&cx, &vt); FEED(&vt, "\xe2\x94\x80\xe2\x94\x82\xe2\x98\x83|\xff|");
    st_check(&cx, "UTF-8: box drawing; unknown code point and bad byte as the replacement",
             cell_is(&vt, 0, 0, 0x2500, false) && cell_is(&vt, 1, 0, 0x2502, false) &&
             cell_is(&vt, 2, 0, FONT8X16_REPLACEMENT, false) && cell_is(&vt, 3, 0, '|', false) &&
             cell_is(&vt, 4, 0, FONT8X16_REPLACEMENT, false) && cell_is(&vt, 5, 0, '|', false) &&
             vt.col == 6 && !cell_is(&vt, 2, 0, '?', false));

    /* 37.1: Latin-1 at its own codes, the mapped extras, and the C1 range,
     * which is control characters and must not draw as Latin-1. */
    st_fresh(&cx, &vt); FEED(&vt, "Gr\xc3\xbc\xc3\x9f""e \xe2\x99\x94\xe2\x99\x9f\xe2\x82\xac\xc2\x85");
    st_check(&cx, "37.1: Latin-1, chess figurines, euro; U+0085 is the replacement",
             fbtext_code(0xfc) == 0xfc && fbtext_code(0x2654) >= 0x80u && fbtext_code(0x85) == FONT8X16_REPLACEMENT &&
             cell_is(&vt, 2, 0, 0xfc, false) && cell_is(&vt, 3, 0, 0xdf, false) &&
             cell_is(&vt, 6, 0, 0x2654, false) && cell_is(&vt, 7, 0, 0x265f, false) &&
             cell_is(&vt, 8, 0, 0x20ac, false) && cell_is(&vt, 9, 0, FONT8X16_REPLACEMENT, false) &&
             !cell_is(&vt, 6, 0, 0x265a, false) && vt.col == 10);

    /* The shadow: after scrolling, reverse video, erasing and cursor moves,
     * wiping the pixels and repainting gives back exactly the same bytes. */
    st_fresh(&cx, &vt);
    FEED(&vt, "one\r\n\033[7mtwo\033[0m\r\nthree\xc3\xa4\r\nfour\r\nfive\033[2;2H\033[K\033[4;3H\033[1K");
    for (uint32_t i = 0; i < ST_COLS * ST_ROWS * FONT8X16_H; i++) cx.copy[i] = cx.buf[i];
    for (uint32_t i = 0; i < ST_COLS * ST_ROWS * FONT8X16_H; i++) cx.buf[i] = 0x5a;
    vtterm_repaint(&vt);
    st_check(&cx, "37.1: repaint from the shadow restores every pixel",
             bytes_equal(cx.buf, cx.copy, ST_COLS * ST_ROWS * FONT8X16_H) &&
             cell_is(&vt, 0, 0, 't', true) && cell_is(&vt, 1, 1, ' ', false) &&
             cell_is(&vt, 2, 3, ' ', false) && cell_is(&vt, 3, 3, 'e', false));

    st_fresh(&cx, &vt); FEED(&vt, "\033]2;Gr\xc3\xbc\xc3\x9f""e\007a\033]0;x\033\\b\033]8;;u\007c");
    st_check(&cx, "37.1: OSC 2 and OSC 0 set the title (BEL and ST); other OSCs are swallowed",
             row_is(&vt, 0, "abc") && vt.title[0] == 'x' && vt.title[1] == '\0' && vt.title_seq == 2 &&
             vt.unknown == 1);

    st_fresh(&cx, &vt); FEED(&vt, "\033]2;t\033[7mz");
    st_check(&cx, "37.1: an ESC that is not ST ends the OSC and starts its own sequence",
             vt.title[0] == 't' && cell_is(&vt, 0, 0, 'z', true));

    {
        /* A window inside a wider buffer (37.3's split): scrolling and
         * erasing move only its own cells, and the bytes beside it survive. */
        for (uint32_t i = 0; i < ST_COLS * ST_ROWS * FONT8X16_H; i++) cx.buf[i] = 0xa5;
        fbtext_t t;
        fbtext_init(&t, cx.buf + 4, ST_COLS, 8, ST_ROWS);
        vtterm_init(&vt, &t, cx.shadow);
        FEED(&vt, "\033[?25l1\r\n2\r\n3\r\n4\r\n5\033[2J\033[HA");
        bool outside = true;
        for (uint32_t y = 0; y < ST_ROWS * FONT8X16_H; y++)
            for (uint32_t x = 0; x < ST_COLS; x++)
                if ((x < 4 || x >= 12) && cx.buf[y * ST_COLS + x] != 0xa5) outside = false;
        st_check(&cx, "37.1: a window narrower than the buffer scrolls and clears inside itself",
                 outside && cell_is(&vt, 0, 0, 'A', false));
    }

    {
        /* The screen: a status bar over the terminal. Output scrolls below
         * the bar and never into it; a title and the indicators are drawn in
         * reverse, the indicators ending one cell short of the edge. */
        screen_t *scr = cx.scr;
        screen_init(scr, cx.buf, ST_COLS, ST_COLS, ST_ROWS + 1u);
        fbtext_t bar;
        fbtext_init(&bar, cx.buf, ST_COLS, ST_COLS, 1);
        bool initial = grid_cell_is(&bar, 1, 0, 'L', true) && grid_cell_is(&bar, 0, 0, ' ', true);
        screen_write(scr, "\033]2;Lisp\007", 9);
        screen_set_right(scr, "12:34", 5);
        screen_write(scr, "1\r\n2\r\n3\r\n4\r\n5", 14);
        unsigned c, r;
        screen_text_size(scr, &c, &r);
        bool bar_ok = grid_cell_is(&bar, 1, 0, 'L', true) && grid_cell_is(&bar, 4, 0, 'p', true) &&
                      grid_cell_is(&bar, 5, 0, ' ', true) && grid_cell_is(&bar, 14, 0, '1', true) &&
                      grid_cell_is(&bar, 18, 0, '4', true) && grid_cell_is(&bar, 19, 0, ' ', true);
        bool text_ok = grid_cell_is(&scr->vt.text, 0, 0, '2', false) &&
                       grid_cell_is(&scr->vt.text, 0, 3, '5', false);
        st_check(&cx, "37.1: the screen's status bar: title, indicators, and text scrolling below it",
                 initial && bar_ok && text_ok && c == ST_COLS && r == ST_ROWS);

        screen_set_title(scr, "a very long title that cannot fit", 33);
        bool cut = grid_cell_is(&bar, 11, 0, 'g', true) && grid_cell_is(&bar, 12, 0, ' ', true) &&
                   grid_cell_is(&bar, 13, 0, ' ', true) &&
                   grid_cell_is(&bar, 14, 0, '1', true);
        for (uint32_t i = 0; i < ST_GRID_BYTES; i++) cx.copy[i] = cx.buf[i];
        for (uint32_t i = 0; i < ST_GRID_BYTES; i++) cx.buf[i] = 0;
        screen_repaint(scr);
        st_check(&cx, "37.1: a long title is cut before the indicators; the whole screen repaints",
                 cut && bytes_equal(cx.buf, cx.copy, ST_GRID_BYTES));
    }

    st_fresh(&cx, &vt); FEED(&vt, "ab\tc\bd\a");
    st_check(&cx, "TAB to column 8, BS back over it, BEL ignored",
             cell_is(&vt, 8, 0, 'd', false) && vt.col == 9 && row_is(&vt, 0, "ab      d"));

    st_fresh(&cx, &vt); FEED(&vt, "\033[?25hq");
    {
        /* The cursor is an XOR underline under the *next* cell, and removed
         * before that cell is drawn: turning it off again must leave exactly
         * the glyphs and nothing else. */
        bool drawn = vt.cursor_drawn && !cell_is(&vt, 1, 0, ' ', false);
        FEED(&vt, "\033[?25l");
        st_check(&cx, "cursor shows on ?25h, and ?25l removes it without a trace",
                 drawn && cell_is(&vt, 0, 0, 'q', false) && cell_is(&vt, 1, 0, ' ', false));
    }

    scratch_release(&sc);
    if (cx.fail == 0) cprintf("VTTERM_SELFTEST_OK (%d/%d)\n", cx.cases, cx.cases);
    else cprintf("VTTERM_SELFTEST_FAIL (%d of %d failed)\n", cx.fail, cx.cases);
    return cx.fail;
}
