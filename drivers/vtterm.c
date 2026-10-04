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
    if (!vt->hidden) {
        if (col >= (unsigned)vt->col_offset && (col - (unsigned)vt->col_offset) < (unsigned)vt->text.cols) {
            fbtext_putcode(&vt->text, col - (unsigned)vt->col_offset, row, code, inverse);
        }
    }
    if (vt->shadow && col < vt->shadow_cols && row < vt->text.rows)
        vt->shadow[row * vt->shadow_cols + col] = (uint16_t)(code | (inverse ? VT_CELL_INVERSE : 0u));
}

LCDTERM_UTEXT static void shadow_blank(vtterm_t *vt, unsigned from, unsigned n) {
    for (unsigned i = 0; i < n; i++) vt->shadow[from + i] = ' ';
}

/* 37.5a: a clear that reaches the window's right edge clears the shadow's
 * whole row: a line cleared "to the end" must not bring back text that sat
 * past a narrower window's edge when the window widens again. */
LCDTERM_UTEXT static void span_clear(vtterm_t *vt, unsigned row, unsigned col0, unsigned col1) {
    if (!vt->hidden) {
        int sc0 = (int)col0 - (int)vt->col_offset;
        int sc1 = (int)col1 - (int)vt->col_offset;
        if (sc0 < 0) sc0 = 0;
        if (sc1 > (int)vt->text.cols) sc1 = (int)vt->text.cols;
        if (sc0 < sc1) fbtext_clear_span(&vt->text, row, (unsigned)sc0, (unsigned)sc1);
    }
    if (!vt->shadow || row >= vt->text.rows) return;
    if (col1 >= vt->shadow_cols) col1 = vt->shadow_cols;
    if (col0 < col1) shadow_blank(vt, row * vt->shadow_cols + col0, col1 - col0);
}

LCDTERM_UTEXT static void rows_clear(vtterm_t *vt, unsigned row, unsigned n) {
    if (!vt->hidden) fbtext_clear_rows(&vt->text, row, n);
    if (!vt->shadow || row >= vt->text.rows) return;
    if (n > (unsigned)vt->text.rows - row) n = vt->text.rows - row;
    shadow_blank(vt, row * vt->shadow_cols, n * vt->shadow_cols);
}

LCDTERM_UTEXT static void grid_scroll(vtterm_t *vt) {
    if (!vt->hidden) fbtext_scroll_up(&vt->text, 1);
    if (!vt->shadow) return;
    unsigned cols = vt->shadow_cols, cells = (unsigned)(vt->text.rows - 1u) * cols;
    if (((uintptr_t)vt->shadow & 3u) == 0 && (cols & 1u) == 0) {
        uint32_t *d = (uint32_t *)(void *)vt->shadow;          /* two cells a word */
        for (unsigned i = 0; i < cells / 2u; i++) d[i] = d[i + cols / 2u];
    } else {
        for (unsigned i = 0; i < cells; i++) vt->shadow[i] = vt->shadow[i + cols];
    }
    shadow_blank(vt, cells, cols);
}

/* 37.5b: rows top..bot (inclusive) move up (`up`) or down by n; the rows
 * that open are cleared. The whole screen up by one is grid_scroll(), the
 * fast path every line of output takes. */
LCDTERM_UTEXT static void region_scroll(vtterm_t *vt, unsigned top, unsigned bot, unsigned n, bool up) {
    if (bot >= vt->text.rows || top > bot || n == 0) return;
    unsigned h = bot - top + 1u;
    if (up && n == 1 && top == 0 && h == vt->text.rows) {
        grid_scroll(vt);
        return;
    }
    if (n > h) n = h;
    unsigned keep = h - n, cols = vt->shadow_cols;
    if (!vt->hidden && keep) fbtext_move_rows(&vt->text, up ? top : top + n, up ? top + n : top, keep);
    if (vt->shadow && keep) {
        unsigned d = (up ? top : top + n) * cols, s = (up ? top + n : top) * cols, cells = keep * cols;
        if (up) { for (unsigned i = 0; i < cells; i++) vt->shadow[d + i] = vt->shadow[s + i]; }
        else    { for (unsigned i = cells; i-- > 0;) vt->shadow[d + i] = vt->shadow[s + i]; }
    }
    rows_clear(vt, up ? bot + 1u - n : top, n);
}

LCDTERM_UTEXT static void cursor_hide(vtterm_t *vt) {
    if (vt->cursor_drawn) {
        if (vt->col >= vt->col_offset && (vt->col - vt->col_offset) < vt->text.cols) {
            fbtext_cursor_xor(&vt->text, vt->col - vt->col_offset, vt->row);
        }
        vt->cursor_drawn = false;
    }
}

LCDTERM_UTEXT static void cursor_show(vtterm_t *vt) {
    if (vt->cursor_on && !vt->cursor_drawn && !vt->hidden) {
        if (vt->col >= vt->col_offset && (vt->col - vt->col_offset) < vt->text.cols) {
            fbtext_cursor_xor(&vt->text, vt->col - vt->col_offset, vt->row);
            vt->cursor_drawn = true;
        }
    }
}

LCDTERM_UTEXT void vtterm_init(vtterm_t *vt, const fbtext_t *text, uint16_t *shadow) {
    /* Field by field, not memset() and a struct copy: this is in the U-mode
     * section, and should not lean on libc that the section cannot reach. */
    vt->text.fb = text->fb;
    vt->text.stride = text->stride;
    vt->text.cols = text->cols;
    vt->text.rows = text->rows;
    vt->text.xbyte = text->xbyte;
    vt->text.whole_rows = text->whole_rows;
    vt->shadow = shadow;
    vt->shadow_cols = text->cols;           /* 37.5a: fixed from here on */
    vt->col_offset = 0;
    vt->logical_cols = text->cols;
    vt->hidden = false;
    vt->col = vt->row = 0;
    vt->top = 0;
    vt->bot = (uint16_t)(text->rows - 1u);
    vt->pending_wrap = vt->inverse = vt->cursor_drawn = vt->private_mode = false;
    vt->bg_dark = false;
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
    if (!vt->shadow || vt->hidden) return;
    vt->cursor_drawn = false;              /* its pixels are about to be redrawn */
    unsigned cols = vt->text.cols;
    unsigned off = vt->col_offset;
    for (unsigned row = 0; row < vt->text.rows; row++) {
        for (unsigned col = 0; col < cols; col++) {
            unsigned sh_col = col + off;
            if (sh_col < vt->shadow_cols) {
                uint16_t v = vt->shadow[row * vt->shadow_cols + sh_col];
                fbtext_putcode(&vt->text, col, row, (uint8_t)v, (v & VT_CELL_INVERSE) != 0);
            } else {
                fbtext_putcode(&vt->text, col, row, ' ', false);
            }
        }
    }
    cursor_show(vt);
}

LCDTERM_UTEXT void vtterm_set_col_offset(vtterm_t *vt, uint16_t offset) {
    if (vt->col_offset != offset) {
        if (vt->cursor_drawn) cursor_hide(vt);
        vt->col_offset = offset;
    }
}

LCDTERM_UTEXT void vtterm_set_logical_cols(vtterm_t *vt, uint16_t logical_cols) {
    if (logical_cols > vt->shadow_cols) logical_cols = vt->shadow_cols;
    if (logical_cols < 1) logical_cols = 1;
    vt->logical_cols = logical_cols;
    if (vt->col >= logical_cols) vt->col = (uint16_t)(logical_cols - 1u);
}

LCDTERM_UTEXT void vtterm_resize(vtterm_t *vt, const fbtext_t *text) {
    unsigned nc = text->cols, rows = text->rows;
    if (nc > vt->shadow_cols) nc = vt->shadow_cols;         /* never past the shadow */
    vt->text.fb = text->fb;
    vt->text.stride = text->stride;
    vt->text.cols = (uint16_t)nc;
    vt->text.rows = text->rows;
    vt->text.xbyte = text->xbyte;
    vt->text.whole_rows = text->whole_rows;
    if (vt->logical_cols == 0) vt->logical_cols = (uint16_t)nc;
    if (vt->col >= vt->logical_cols) vt->col = (uint16_t)(vt->logical_cols - 1u);
    if (vt->row >= rows) vt->row = (uint16_t)(rows - 1u);
    if (vt->bot >= rows || vt->top >= vt->bot) {    /* 37.5b: a region that no longer fits */
        vt->top = 0;
        vt->bot = (uint16_t)(rows - 1u);
    }
    vt->pending_wrap = false;
    vt->cursor_drawn = false;              /* the caller repaints */
}

LCDTERM_UTEXT void vtterm_set_hidden(vtterm_t *vt, bool hidden) {
    if (hidden && vt->cursor_drawn) cursor_hide(vt);
    vt->hidden = hidden;
    vt->cursor_drawn = false;
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
    if (vt->row == vt->bot) region_scroll(vt, vt->top, vt->bot, 1, true);
    else if (vt->row + 1u < vt->text.rows) vt->row++;
}

LCDTERM_UTEXT static void put_cp(vtterm_t *vt, uint32_t cp) {
    if (vt->pending_wrap) {
        vt->col = 0;
        line_feed(vt);
        vt->pending_wrap = false;
    }
    bool rev = vt->inverse != vt->bg_dark;
    uint8_t code = fbtext_code(cp);
    if (rev && code >= FONT8X16_FIG_WHITE && code < FONT8X16_FIG_BLACK + 6u)   /* 37.4 */
        code = (uint8_t)(code < FONT8X16_FIG_BLACK ? code + 6u : code - 6u);
    cell_put(vt, vt->col, vt->row, code, rev);
    unsigned wrap_at = vt->logical_cols ? vt->logical_cols : vt->text.cols;
    if (vt->col + 1u < wrap_at) vt->col++;
    else vt->pending_wrap = true;
}

LCDTERM_UTEXT static unsigned param(const vtterm_t *vt, unsigned i, unsigned dflt) {
    return (i < vt->nparams && vt->params[i] != 0) ? vt->params[i] : dflt;
}

LCDTERM_UTEXT static unsigned clampu(unsigned v, unsigned hi) {
    return v > hi ? hi : v;
}

/* 37.4: the brightness (0..255, Rec. 601 weights) of the 16 ANSI colours as
 * xterm draws them -- black, red, green, yellow, blue, magenta, cyan, white,
 * then the bright eight. In the task's own read-only section: a plain
 * const table would be .rodata, outside its domain. */
LCDTERM_URODATA static const uint8_t k_ansi_luma[16] = {
    0, 61, 120, 181, 27, 84, 143, 229, 127, 76, 149, 226, 110, 105, 178, 255,
};

LCDTERM_UTEXT static unsigned luma(unsigned r, unsigned g, unsigned b) {
    return (77u * (r > 255u ? 255u : r) + 150u * (g > 255u ? 255u : g) + 29u * (b > 255u ? 255u : b)) >> 8;
}

/* A 256-colour palette index's brightness: the 16 ANSI colours, the 6x6x6
 * cube (levels 0, 95, 135, 175, 215, 255), and the 24-step grey ramp. */
LCDTERM_UTEXT static unsigned luma256(unsigned n) {
    if (n < 16u) return k_ansi_luma[n];
    if (n < 232u) {
        unsigned i = n - 16u, r = i / 36u, g = (i / 6u) % 6u, b = i % 6u;
        r = r ? 55u + 40u * r : 0u; g = g ? 55u + 40u * g : 0u; b = b ? 55u + 40u * b : 0u;
        return luma(r, g, b);
    }
    return n <= 255u ? 8u + 10u * (n - 232u) : 255u;
}

LCDTERM_UTEXT static void sgr(vtterm_t *vt) {
    if (vt->nparams == 0) {
        vt->inverse = vt->bg_dark = false;
        return;
    }
    for (unsigned i = 0; i < vt->nparams; i++) {
        unsigned p = vt->params[i];
        if (p == 0) vt->inverse = vt->bg_dark = false;
        else if (p == 27) vt->inverse = false;
        else if (p == 7) vt->inverse = true;
        else if (p >= 40u && p <= 47u) vt->bg_dark = k_ansi_luma[p - 40u] < VT_DARK_LUMA;
        else if (p >= 100u && p <= 107u) vt->bg_dark = k_ansi_luma[p - 100u + 8u] < VT_DARK_LUMA;
        else if (p == 49) vt->bg_dark = false;
        else if (p == 38 || p == 48) {
            /* Extended colour: its arguments are consumed here, never read as
             * attributes -- a truecolour 7 must not turn on reverse video.
             * A background's brightness decides reverse video (37.4); a
             * foreground's is ignored. */
            if (i + 2u < vt->nparams && vt->params[i + 1u] == 5) {
                if (p == 48) vt->bg_dark = luma256(vt->params[i + 2u]) < VT_DARK_LUMA;
                i += 2;
            } else if (i + 4u < vt->nparams && vt->params[i + 1u] == 2) {
                if (p == 48)
                    vt->bg_dark = luma(vt->params[i + 2u], vt->params[i + 3u], vt->params[i + 4u]) < VT_DARK_LUMA;
                i += 4;
            } else if (i + 1u < vt->nparams && (vt->params[i + 1u] == 5 || vt->params[i + 1u] == 2)) {
                i = vt->nparams;            /* truncated: nothing more to read */
            }
        }
        /* everything else -- bold, foreground colours -- has no 1-bpp rendering */
    }
}

LCDTERM_UTEXT static void csi_final(vtterm_t *vt, char f) {
    unsigned cols = vt->logical_cols ? vt->logical_cols : vt->text.cols;
    unsigned rows = vt->text.rows;
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
    case 'r': {                            /* 37.5b: DECSTBM, then home */
        unsigned t = param(vt, 0, 1) - 1u, b = param(vt, 1, rows) - 1u;
        if (b >= rows) b = rows - 1u;
        if (t >= b) break;
        vt->top = (uint16_t)t;
        vt->bot = (uint16_t)b;
        vt->row = vt->col = 0;
        break;
    }
    case 'L':
    case 'M':                              /* insert / delete lines, inside the region */
        if (vt->row >= vt->top && vt->row <= vt->bot)
            region_scroll(vt, vt->row, vt->bot, param(vt, 0, 1), f == 'M');
        vt->col = 0;
        break;
    case 'S':
    case 'T':
        region_scroll(vt, vt->top, vt->bot, param(vt, 0, 1), f == 'S');
        break;
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
    case '\t': {
        unsigned wrap_at = vt->logical_cols ? vt->logical_cols : vt->text.cols;
        vt->col = (uint16_t)clampu((vt->col / 8u + 1u) * 8u, wrap_at - 1u);
        vt->pending_wrap = false;
        return;
    }
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
    uint8_t  *buf;          /* the grid, or the test screen: ST_BUF_BYTES */
    uint8_t  *copy;         /* as much again, to compare a repaint against */
    uint16_t *shadow;       /* ST_COLS x ST_ROWS cells */
    screen_t *scr;          /* SCREEN_BYTES(ST_SW, ST_SH) */
    int fail, cases;
} st_ctx_t;

/* The screen test's screen: 192 x 110 px, which gives a 22 x 4 text window
 * in its tile -- small, but every piece of the panel's chrome is there. */
#define ST_SW 192u
#define ST_SH 110u
/* 37.3b's layout test needs a screen a split fits on: 400 x 110 px, 50
 * cells, which gives the wide split a 69 x 66 canvas beside 38 x 4 text. */
#define ST_LW 400u
#define ST_LH 110u
/* 37.5a's split-width test: 576 px, 72 cells -- the narrowest screen on
 * which the 64-column split still leaves a canvas. */
#define ST_WW 576u
#define ST_BUF_BYTES (ST_WW / 8u * ST_LH)

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

/* 37.3b: a canvas pixel through the protocol, as a program would read it. */
static unsigned st_get(screen_t *scr, int x, int y) {
    const uint8_t req[5] = { 'g', (uint8_t)x, (uint8_t)((unsigned)x >> 8), (uint8_t)y, (uint8_t)((unsigned)y >> 8) };
    uint8_t rp[SCREEN_REPLY_LEN];
    screen_canvas(scr, req, sizeof(req), rp);
    return rp[1];
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
    uint32_t shadow_off = 2u * ST_BUF_BYTES;
    uint32_t scr_off = (shadow_off + ST_COLS * ST_ROWS * 2u + 7u) & ~7u;
    scratch_t sc;
    uint32_t store_off = (scr_off + SCREEN_BYTES(ST_WW, ST_LH) + 7u) & ~7u;
    if (!scratch_acquire(&sc, store_off + SCREEN_STORE_BYTES(ST_WW, ST_LH))) {
        cprintf("VTTERM_SELFTEST_FAIL (no memory for the test grid)\n");
        return 1;
    }
    vtterm_t vt;
    uint8_t *base = (uint8_t *)sc.base;
    st_ctx_t cx = { base, base + ST_BUF_BYTES, (uint16_t *)(void *)(base + shadow_off),
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

    /* 37.5b: the editor's scrolling -- a region of rows 1-3, the fourth (a
     * status line) left alone by every scroll. */
    st_fresh(&cx, &vt); FEED(&vt, "\033[4;1HS\033[1;3r\033[3;1Ha\r\nb\r\nc");
    st_check(&cx, "37.5b: a scroll region: LF at its bottom scrolls rows 1-3 only",
             row_is(&vt, 0, "a") && row_is(&vt, 1, "b") && row_is(&vt, 2, "c") && row_is(&vt, 3, "S") && vt.row == 2);
    FEED(&vt, "\033[1;1H\033[L");
    bool il = row_is(&vt, 0, "") && row_is(&vt, 1, "a") && row_is(&vt, 2, "b") && row_is(&vt, 3, "S");
    FEED(&vt, "\033[2;1H\033[2M");
    bool dl = row_is(&vt, 0, "") && row_is(&vt, 1, "") && row_is(&vt, 2, "") && row_is(&vt, 3, "S");
    FEED(&vt, "\033[1;1Hx\033[2;1Hy\033[3;1Hz\033[2T");
    bool sd = row_is(&vt, 0, "") && row_is(&vt, 1, "") && row_is(&vt, 2, "x") && row_is(&vt, 3, "S");
    FEED(&vt, "\033[S");
    bool su = row_is(&vt, 1, "x") && row_is(&vt, 2, "") && row_is(&vt, 3, "S");
    FEED(&vt, "\033[r\033[4;1H\n");
    st_check(&cx, "37.5b: insert and delete lines, scroll down and up, inside the region; ESC[r resets it",
             il && dl && sd && su && row_is(&vt, 3, "") && row_is(&vt, 2, "S"));

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

    st_fresh(&cx, &vt); FEED(&vt, "\033[1;36mK\033[38;2;7;7;7m\033[48;2;200;200;200mL\033[38;5;7mM\033[0m");
    st_check(&cx, "foreground colours and bold are consumed, a truecolour 7 is not reverse, nor a light background",
             row_is(&vt, 0, "KLM") && vt.unknown == 0);

    /* 37.4: dark backgrounds are reverse video -- the console chess board's
     * own two square colours, then ANSI, 256-colour and SGR 7 on top. */
    st_fresh(&cx, &vt);
    FEED(&vt, "\033[48;2;240;217;181mA\033[0m\033[48;2;181;136;99mB\033[0m"
              "\033[40mC\033[47mD\033[49mE\033[48;5;232mF\033[48;5;255mG\033[0m"
              "\033[44m\033[7mH\033[0mI");
    st_check(&cx, "37.4: dark backgrounds reverse (truecolour, ANSI, 256); SGR 7 on dark reverses back",
             cell_is(&vt, 0, 0, 'A', false) && cell_is(&vt, 1, 0, 'B', true) &&
             cell_is(&vt, 2, 0, 'C', true) && cell_is(&vt, 3, 0, 'D', false) &&
             cell_is(&vt, 4, 0, 'E', false) && cell_is(&vt, 5, 0, 'F', true) &&
             cell_is(&vt, 6, 0, 'G', false) && cell_is(&vt, 7, 0, 'H', false) &&
             cell_is(&vt, 8, 0, 'I', false));

    st_fresh(&cx, &vt);
    FEED(&vt, "\xe2\x99\x94\033[40m\xe2\x99\x94\xe2\x99\x9f\033[0m\xe2\x99\x9f");
    st_check(&cx, "37.4: a figurine in a reversed cell is drawn with its opposite-colour glyph",
             fbtext_code(0x2654) == FONT8X16_FIG_WHITE && fbtext_code(0x265a) == FONT8X16_FIG_BLACK &&
             cell_is(&vt, 0, 0, 0x2654, false) && cell_is(&vt, 1, 0, 0x265a, true) &&
             cell_is(&vt, 2, 0, 0x2659, true) && cell_is(&vt, 3, 0, 0x265f, false));

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
        /* 37.1a, the screen (plan §1.1): the menu bar, the desktop, the
         * framed tile and its title bar, and the terminal inside it. Checked
         * at the pixels the geometry in drivers/screen.h promises. */
        screen_t *scr = cx.scr;
        screen_init(scr, cx.buf, ST_SW / 8u, ST_SW, ST_SH);
        const uint32_t stride = ST_SW / 8u;
#define PX(x, y) ((cx.buf[(uint32_t)(y) * stride + (uint32_t)(x) / 8u] >> ((x) % 8u)) & 1u)
        unsigned c, r;
        screen_text_size(scr, &c, &r);
        int x1 = 8 + 8 * 22 + 2, y1 = 40 + 16 * 4 + 1;        /* 186, 105 */
        bool rule = true, menu_blank = true;
        for (uint32_t b = 0; b < stride; b++) {
            if (cx.buf[19u * stride + b] != 0xffu) rule = false;
            if (cx.buf[1u * stride + b] != 0u) menu_blank = false;
        }
        bool geometry = c == 22 && r == 4 && scr->fx0 == 4 && scr->fy0 == 22 &&
                        scr->fx1 == x1 && scr->fy1 == y1;
        bool desktop = cx.buf[20u * stride] == 0xaau && cx.buf[21u * stride] == 0x55u &&
                       PX(190, 60) == ((190 + 60) & 1u) && PX(0, 109) == ((0 + 109) & 1u);
        bool frame = PX(4, 60) && !PX(5, 60) && !PX(7, 60) && PX(x1, 60) && !PX(x1 - 1, 60) &&
                     PX(x1 + 1, 60) && PX(x1 + 1, 23) && PX(100, y1) && PX(100, y1 + 1) &&
                     !PX(4, y1 + 1) && PX(5, y1 + 1);
        /* Stripes on rows y0+3..y0+13 (odd offsets), white between; the
         * title box is centred: "LugalOS" is 7 glyphs, a box 68 px wide. */
        int bw = 8 * 7 + 12, bx = (4 + x1 + 1) / 2 - bw / 2;
        bool stripes = PX(6, 22 + 3) && !PX(6, 22 + 4) && PX(6, 22 + 13) && !PX(6, 22 + 14) &&
                       !PX(6, 22 + 1) && !PX(6, 22 + 15) && PX(6, 22 + 16) && PX(6, 22) &&
                       !PX(5, 22 + 3) && PX(bx - 1, 22 + 3) && !PX(bx, 22 + 3) &&
                       !PX(bx + bw - 1, 22 + 3) && PX(bx + bw, 22 + 3);
        /* The title in bold from (bx + 6, y0 + 1): its glyph rows 1..14,
         * each pixel and its right neighbour. */
        bool title = true;
        const uint8_t *gL = fbtext_glyph('L');
        for (unsigned row = 1; row < 15; row++)
            for (unsigned b = 0; b < 8; b++) {
                bool on = ((gL[row] >> b) & 1u) || (b > 0 && ((gL[row] >> (b - 1)) & 1u));
                if ((bool)PX(bx + 6 + (int)b, 23 + (int)row) != on) title = false;
            }
        bool initial = geometry && rule && menu_blank && desktop && frame && stripes && title;

        screen_write(scr, "\033]2;Lisp\007", 9);
        screen_set_right(scr, 0, "12:34", 5);
        screen_write(scr, "1\r\n2\r\n3\r\n4\r\n5", 14);
        fbtext_t clock;                       /* the clock: at x = 192 - 16 - 40 = 136, y = 2 */
        fbtext_init(&clock, cx.buf + 2u * stride, stride, ST_SW / 8u, 1);
        bool menu = grid_cell_is(&clock, 17, 0, '1', false) && grid_cell_is(&clock, 21, 0, '4', false);
        int bw2 = 8 * 4 + 12, bx2 = (4 + x1 + 1) / 2 - bw2 / 2;
        bool retitled = PX(bx2 - 1, 22 + 3) && !PX(bx2, 22 + 3) && PX(bx2 + bw2, 22 + 3);
        bool text_ok = grid_cell_is(&scr->vt.text, 0, 0, '2', false) &&
                       grid_cell_is(&scr->vt.text, 0, 3, '5', false) && rule && PX(4, 60);
        st_check(&cx, "37.1a: the screen's chrome: menu bar, desktop, frame, shadow, title bar",
                 initial);
        st_check(&cx, "37.1a: a new title is re-centred, the clock sits 16 px from the edge, text scrolls in its tile",
                 menu && retitled && text_ok);

        const uint32_t sbytes = ST_SW / 8u * ST_SH;
        for (uint32_t i = 0; i < sbytes; i++) cx.copy[i] = cx.buf[i];
        for (uint32_t i = 0; i < sbytes; i++) cx.buf[i] = 0x3c;
        screen_repaint(scr);
        st_check(&cx, "37.1a: the whole screen repaints to the same bytes",
                 bytes_equal(cx.buf, cx.copy, sbytes));
#undef PX
    }

    {
        /* 37.3b: layouts and the canvas protocol, on a screen a split fits. */
        screen_t *scr = cx.scr;
        screen_init(scr, cx.buf, ST_LW / 8u, ST_LW, ST_LH);
        uint8_t rp[SCREEN_REPLY_LEN];
#define REQ(...) do { const uint8_t r_[] = { __VA_ARGS__ }; screen_canvas(scr, r_, sizeof(r_), rp); } while (0)
#define I16(v) (uint8_t)((v) & 0xff), (uint8_t)(((unsigned)(v) >> 8) & 0xff)
#define RW (unsigned)(rp[2] | (rp[3] << 8))
#define RH (unsigned)(rp[4] | (rp[5] << 8))
#define RD (unsigned)(rp[6] | (rp[7] << 8))
        REQ('p', I16(1), I16(1), I16(1));
        bool no_canvas = rp[0] == 1 && RW == 0 && RH == 0 && rp[8] == SCREEN_LAYOUT_TEXT;
        screen_write(scr, "0123456789012345678901234567890123456789012345\r\nxyz", 51);
        unsigned d0 = RD;
        REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
        unsigned c, r;
        screen_text_size(scr, &c, &r);
        bool split = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_SPLIT_WIDE && RW == 69 && RH == 66 &&
                     RD == d0 + 1 && c == 38 && r == 4 && scr->cx1 == 74 && scr->fx0 == 84 &&
                     grid_cell_is(&scr->vt.text, 0, 0, '0', false) &&
                     grid_cell_is(&scr->vt.text, 37, 0, '7', false) &&
                     scr->vt.text.fb == cx.buf + 40u * (ST_LW / 8u) + 11u;
        REQ('L', 9);
        bool bad = rp[0] == 1 && rp[8] == SCREEN_LAYOUT_SPLIT_WIDE;
        st_check(&cx, "37.3b: the wide split: canvas size, text 38 wide keeping each row's left part, damage moves",
                 no_canvas && split && bad);

        const uint32_t stride = ST_LW / 8u;
#define PXL(x, y) ((cx.buf[(uint32_t)(y) * stride + (uint32_t)(x) / 8u] >> ((x) % 8u)) & 1u)
#define GET(x, y) st_get(scr, (x), (y))
        REQ('F', I16(0));
        REQ('l', I16(0), I16(0), I16(10), I16(5), I16(1));
        bool line = GET(0, 0) && GET(10, 5) && GET(2, 1) && !GET(10, 0) && !GET(0, 5);
        REQ('c', I16(30), I16(30), I16(10), I16(1));
        bool circle = GET(40, 30) && GET(20, 30) && GET(30, 20) && GET(30, 40) && !GET(30, 30);
        REQ('b', I16(0), I16(50), I16(5), I16(1), 0x16);         /* 0b10110: pixels 1, 2, 4 */
        bool row = !GET(0, 50) && GET(1, 50) && GET(2, 50) && !GET(3, 50) && GET(4, 50);
        REQ('b', I16(10), I16(52), I16(2), I16(3), 0x01);        /* 3x3 cells: one on, one off */
        bool scaled = GET(10, 52) && GET(12, 54) && !GET(13, 52) && !GET(15, 54);
        REQ('r', I16(50), I16(50), I16(4), I16(4), I16(1));
        REQ('i', I16(50), I16(50), I16(2), I16(4));
        bool inv = !GET(50, 50) && !GET(51, 53) && GET(52, 50) && GET(53, 53);
        REQ('t', I16(40), I16(0), I16(0), 0, 'H');
        bool text = GET(40, 2) && GET(41, 6);                      /* 'H''s left stem */
        /* Clipping: a line across the whole buffer stays inside the tile --
         * the frame, its shadow and the desktop beside it are untouched. */
        REQ('l', I16(-100), I16(-50), I16(500), I16(200), I16(1));
        bool clip = PXL(4, 60) && !PXL(5, 22 + 17 + 1) && PXL(75, 60) &&
                    PXL(76, 60) == ((76 + 60) & 1u) && PXL(4, 22) && !PXL(84 + 2, 60 + 40);
        st_check(&cx, "37.3b: canvas line, circle, row (and scaled), rectangle, invert, text; all clipped to the tile",
                 line && circle && row && scaled && inv && text && clip);

        /* The full canvas hides the text, which keeps being written to its
         * shadow, and comes back with it; a repaint clears the canvas and
         * moves the damage on. */
        REQ('L', SCREEN_LAYOUT_CANVAS);
        bool full = rp[0] == 0 && RW == 389u && RH == 66u && scr->vt.hidden;   /* frame x 4..394 */
        screen_write(scr, "\r\nhid", 6);
        REQ('p', I16(5), I16(5), I16(1));
        unsigned d1 = RD;
        screen_repaint(scr);
        REQ('S');
        bool cleared = RD == d1 + 1 && !GET(5, 5);
        REQ('L', SCREEN_LAYOUT_TEXT);
        screen_text_size(scr, &c, &r);
        bool back = rp[0] == 0 && RW == 0 && c == 48 && !scr->vt.hidden &&
                    grid_cell_is(&scr->vt.text, 0, 2, 'h', false) &&
                    grid_cell_is(&scr->vt.text, 37, 0, '7', false) &&
                    grid_cell_is(&scr->vt.text, 38, 0, '8', false) &&     /* 37.5a: kept */
                    grid_cell_is(&scr->vt.text, 45, 0, '5', false);
        REQ('T', 'B', 'o', 'a', 'r', 'd');
        bool titled = scr->ctitle[0] == 'B' && scr->ctitle[5] == '\0';
        st_check(&cx, "37.3b: the full canvas hides text that keeps arriving; a repaint clears the canvas; "
                      "back to text with the line past the narrow split's edge intact (37.5a)",
                 full && cleared && back && titled);

        /* 37.5a: the divider's five places, stepped left and right, the
         * swap, and the lock -- on a 72-cell screen, where all three splits
         * leave a canvas. Canvas on the left: left means more text. */
        screen_init(scr, cx.buf, ST_WW / 8u, ST_WW, ST_LH);
        REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
        unsigned wide_w = RW;
        REQ('w', -1);
        screen_text_size(scr, &c, &r);
        bool to_half = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_SPLIT_HALF && c == 48;
        REQ('w', -1);
        screen_text_size(scr, &c, &r);
        bool to_narrow = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_SPLIT_NARROW && c == 64;
        REQ('w', -1);
        bool closed = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_TEXT && RW == 0;
        REQ('w', -1);
        bool left_end = rp[0] == 1 && rp[8] == SCREEN_LAYOUT_TEXT;
        REQ('w', 1);
        bool reopened = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_SPLIT_NARROW;
        REQ('w', 1); REQ('w', 1); REQ('w', 1);
        bool all_canvas = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_CANVAS && scr->vt.hidden;
        REQ('w', 1);
        bool right_end = rp[0] == 1 && rp[8] == SCREEN_LAYOUT_CANVAS;
        st_check(&cx, "37.5a: the divider steps through text only, three splits and canvas only, and stops",
                 to_half && to_narrow && closed && left_end && reopened && all_canvas && right_end);

        /* Swapped: text left, the canvas right and exactly as wide; left
         * now means less text. */
        REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
        REQ('X');
        bool swapped = rp[0] == 0 && rp[9] == 1 && scr->fx0 == 4 && scr->cx0 > scr->fx1 &&
                       RW == wide_w && scr->vt.text.fb == cx.buf + 40u * (ST_WW / 8u) + 1u;
        REQ('w', 1);
        screen_text_size(scr, &c, &r);
        bool mirrored = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_SPLIT_HALF && c == 48;
        REQ('K', 1);
        REQ('w', 1);
        bool lk1 = rp[0] == 1 && rp[8] == SCREEN_LAYOUT_SPLIT_HALF;
        REQ('X');
        bool lk2 = rp[0] == 1 && rp[9] == 1;
        REQ('L', SCREEN_LAYOUT_TEXT);           /* releases the lock */
        REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
        REQ('X');
        bool unlocked = rp[0] == 0 && rp[9] == 0;
        REQ('L', SCREEN_LAYOUT_TEXT);
        REQ('X');
        bool to_canvas = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_CANVAS && scr->vt.hidden;
        REQ('X');
        bool to_text = rp[0] == 0 && rp[8] == SCREEN_LAYOUT_TEXT && !scr->vt.hidden;
        st_check(&cx, "37.5a: swapped panes mirror the divider; a lock holds both until text; "
                      "with one pane full, the swap shows the other one full",
                 swapped && mirrored && lk1 && lk2 && unlocked && to_canvas && to_text);

        /* 38.8: the canvas stores. Leaving a layout keeps its canvas, and
         * coming back shows it with no damage -- also swapped, at the other
         * side; the pixel in the tile's last column comes back without
         * touching the frame beside it. Drawing in another layout makes the
         * old store stale, and 'Z' forgets them all: both come back blank,
         * with damage, as without a store. */
        screen_init(scr, cx.buf, ST_WW / 8u, ST_WW, ST_LH);
        screen_set_store(scr, (uint8_t *)sc.base + store_off, SCREEN_STORE_BYTES(ST_WW, ST_LH));
        {
            const uint32_t ws = ST_WW / 8u;
#define PXW(x, y) ((cx.buf[(uint32_t)(y) * ws + (uint32_t)(x) / 8u] >> ((x) % 8u)) & 1u)
#define GET(x, y) st_get(scr, (x), (y))
            REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
            unsigned w = RW, h = RH;
            REQ('F', I16(0));
            REQ('l', I16(0), I16(0), I16(20), I16(10), I16(1));
            REQ('p', I16(3), I16(3), I16(1));
            REQ('p', I16(w - 1), I16(h - 1), I16(1));
            unsigned d = RD;
            REQ('L', SCREEN_LAYOUT_TEXT);
            REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
            bool kept = RD == d && GET(0, 0) && GET(20, 10) && GET(3, 3) && !GET(20, 0) && !GET(4, 3) &&
                        GET(w - 1, h - 1) && !GET(w - 2, h - 1) && PXW(scr->cx1, 60) &&
                        PXW(scr->cx1 + 2, 61) == ((scr->cx1 + 2u + 61u) & 1u);   /* the desktop */
            REQ('X');
            bool other_side = rp[9] == 1 && RD == d && GET(0, 0) && GET(20, 10) && GET(3, 3) &&
                              !GET(4, 3) && GET(w - 1, h - 1) && PXW(scr->cx1, 60) && PXW(scr->cx0, 60);
            REQ('X');
            REQ('L', SCREEN_LAYOUT_SPLIT_HALF);
            bool fresh = RD == d + 1u && !GET(3, 3);
            REQ('p', I16(1), I16(1), I16(1));
            REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
            bool stale = RD == d + 2u && !GET(3, 3);
            REQ('p', I16(3), I16(3), I16(1));
            REQ('L', SCREEN_LAYOUT_TEXT);
            REQ('Z');
            REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
            bool forgot = RD == d + 3u && !GET(3, 3);
            /* A redraw after damage ('D' 1 .. 'D' 0) is the same picture at
             * another size: both layouts' stores stay good, and resizing
             * back and forth restores without damage (the owner's Lorenz
             * case). A layout change ends the bracket. */
            REQ('p', I16(3), I16(3), I16(1));           /* drawn in wide */
            REQ('L', SCREEN_LAYOUT_SPLIT_HALF);
            unsigned dh = RD;
            REQ('D', 1);
            REQ('p', I16(7), I16(7), I16(1));           /* the redraw, in half */
            REQ('D', 0);
            REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);
            bool wide_back = RD == dh && GET(3, 3) && !GET(7, 7);
            REQ('L', SCREEN_LAYOUT_SPLIT_HALF);
            bool half_back = RD == dh && GET(7, 7) && !GET(3, 3);
            REQ('D', 1);
            REQ('L', SCREEN_LAYOUT_SPLIT_WIDE);         /* ends the bracket */
            REQ('p', I16(9), I16(9), I16(1));
            REQ('L', SCREEN_LAYOUT_SPLIT_HALF);
            bool ended = RD == dh + 1u && !GET(7, 7);
            st_check(&cx, "38.8: a layout's canvas is stored and comes back without damage, also swapped; "
                          "stale after drawing elsewhere, and forgotten on 'Z'",
                     kept && other_side && fresh && stale && forgot);
            st_check(&cx, "38.8: a redraw ('D') keeps the other stores good, so resizing back and forth "
                          "restores both; a layout change ends it",
                     wide_back && half_back && ended);
#undef GET
#undef PXW
        }
        screen_set_store(scr, 0, 0);

        /* A split is refused where there is no room for a canvas. */
        screen_init(scr, cx.buf, ST_SW / 8u, ST_SW, ST_SH);
        REQ('L', SCREEN_LAYOUT_SPLIT_HALF);
        st_check(&cx, "37.3b: a split the screen is too narrow for is refused", rp[0] == 1 && rp[8] == 0);
#undef GET
#undef PXL
#undef RD
#undef RH
#undef RW
#undef I16
#undef REQ
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
