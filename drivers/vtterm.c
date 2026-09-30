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
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"
#include "kernel/console.h"
#include "kernel/scratch.h"


enum { ST_GROUND, ST_ESC, ST_CSI };

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

LCDTERM_UTEXT void vtterm_init(vtterm_t *vt, const fbtext_t *text) {
    /* Field by field, not memset() and a struct copy: this is in the U-mode
     * section, and should not lean on libc that the section cannot reach. */
    vt->text.fb = text->fb;
    vt->text.stride = text->stride;
    vt->text.cols = text->cols;
    vt->text.rows = text->rows;
    vt->col = vt->row = 0;
    vt->pending_wrap = vt->inverse = vt->cursor_drawn = vt->private_mode = false;
    vt->state = vt->nparams = vt->utf8_need = 0;
    vt->utf8_cp = vt->unknown = 0;
    for (unsigned i = 0; i < 8u; i++) vt->params[i] = 0;
    vt->cursor_on = true;
    fbtext_clear_rows(&vt->text, 0, vt->text.rows);
    cursor_show(vt);
}

LCDTERM_UTEXT static void line_feed(vtterm_t *vt) {
    if (vt->row + 1u < vt->text.rows) vt->row++;
    else fbtext_scroll_up(&vt->text, 1);
}

LCDTERM_UTEXT static void put_cp(vtterm_t *vt, uint32_t cp) {
    if (vt->pending_wrap) {
        vt->col = 0;
        line_feed(vt);
        vt->pending_wrap = false;
    }
    fbtext_putcp(&vt->text, vt->col, vt->row, cp, vt->inverse);
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
        if (m == 0) fbtext_clear_span(&vt->text, vt->row, vt->col, cols);
        else if (m == 1) fbtext_clear_span(&vt->text, vt->row, 0, vt->col + 1u);
        else fbtext_clear_span(&vt->text, vt->row, 0, cols);
        break;
    }
    case 'J': {
        unsigned m = vt->nparams ? vt->params[0] : 0;
        if (m == 0) {
            fbtext_clear_span(&vt->text, vt->row, vt->col, cols);
            fbtext_clear_rows(&vt->text, vt->row + 1u, rows - vt->row - 1u);
        } else if (m == 1) {
            fbtext_clear_rows(&vt->text, 0, vt->row);
            fbtext_clear_span(&vt->text, vt->row, 0, vt->col + 1u);
        } else {
            fbtext_clear_rows(&vt->text, 0, rows);
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
        put_cp(vt, '?');                   /* handle this byte on its own    */
    }
    if (c >= 0x80u) {
        if ((c & 0xe0u) == 0xc0u)      { vt->utf8_cp = c & 0x1fu; vt->utf8_need = 1; }
        else if ((c & 0xf0u) == 0xe0u) { vt->utf8_cp = c & 0x0fu; vt->utf8_need = 2; }
        else if ((c & 0xf8u) == 0xf0u) { vt->utf8_cp = c & 0x07u; vt->utf8_need = 3; }
        else put_cp(vt, '?');
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

LCDTERM_UTEXT void vtterm_putc(vtterm_t *vt, char ch) {
    unsigned char c = (unsigned char)ch;
    cursor_hide(vt);
    switch (vt->state) {
    case ST_GROUND:
        ground(vt, c);
        break;
    case ST_ESC:
        if (c == '[') {
            vt->state = ST_CSI;
            vt->nparams = 0;
            vt->private_mode = false;
            for (unsigned i = 0; i < 8u; i++) vt->params[i] = 0;   /* no libc in U-mode */
        } else {
            vt->state = ST_GROUND;         /* ESC x: two bytes, no effect */
            vt->unknown++;
        }
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
    uint8_t *buf;
    int fail, cases;
} st_ctx_t;

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
    vtterm_init(vt, &t);
    vtterm_write(vt, "\033[?25l", 6);
}

static void st_check(st_ctx_t *cx, const char *name, bool ok) {
    cx->cases++;
    if (!ok) cx->fail++;
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", name);
}

#define FEED(vt, s) vtterm_write((vt), (s), (uint32_t)(sizeof(s) - 1u))

int vtterm_selftest(void) {
    /* 1280 bytes, on demand rather than in .bss (kernel/scratch.h's rule). */
    scratch_t sc;
    if (!scratch_acquire(&sc, ST_COLS * ST_ROWS * FONT8X16_H)) {
        cprintf("VTTERM_SELFTEST_FAIL (no memory for the test grid)\n");
        return 1;
    }
    vtterm_t vt;
    st_ctx_t cx = { (uint8_t *)sc.base, 0, 0 };
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
    st_check(&cx, "UTF-8: box drawing, unknown code point and bad byte as '?'",
             cell_is(&vt, 0, 0, 0x2500, false) && cell_is(&vt, 1, 0, 0x2502, false) &&
             cell_is(&vt, 2, 0, '?', false) && cell_is(&vt, 3, 0, '|', false) &&
             cell_is(&vt, 4, 0, '?', false) && cell_is(&vt, 5, 0, '|', false) && vt.col == 6);

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
