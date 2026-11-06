/* drivers/osk.c — Classic Mac On-Screen Touch Keyboard
 *
 * Implements an on-screen keyboard (OSK) for terminal windows on touch-first
 * devices when no physical keyboard is attached:
 * - 5-row laptop/terminal layout (Esc, Numbers, QWERTY, Shift, Space, Arrows, ^C, Enter)
 * - Classic Mac beveled button aesthetics using 1-bpp canvas
 * - Direct coordinate geometry hit testing (zero dynamic allocation, no hitbox limits)
 * - Sticky Shift, sticky Ctrl, and Symbols mode toggle
 * - Feeds keys directly into active vterm via kernel/console.c
 * - Scale-aware (CHROME_SCALE = 1 on 8x16 panels, 2 on 16x32 panels)
 * - Zero float in M-mode, freestanding C, LCDTERM_UTEXT for U-mode compatibility.
 */

#include "drivers/osk.h"
#include "drivers/touch.h"
#include "lugalos_config.h"

#if !defined(TOUCH_PRESENT)

int osk_get_height(void) { return 0; }
int osk_get_text_rows(int cell_h) { (void)cell_h; return 0; }
void osk_draw(const canvas1_t *cv, int x0, int y0, int w, int h, uint8_t modifiers, int pressed) {
    (void)cv; (void)x0; (void)y0; (void)w; (void)h; (void)modifiers; (void)pressed;
}
int osk_key_at(int client_x, int client_y, int w, int h) {
    (void)client_x; (void)client_y; (void)w; (void)h;
    return -1;
}
uint16_t osk_repeat_key(int pressed) { (void)pressed; return 0; }
void osk_draw_toggle_button(const canvas1_t *cv, int btn_x, int btn_y, int btn_w, int btn_h) {
    (void)cv; (void)btn_x; (void)btn_y; (void)btn_w; (void)btn_h;
}
uint16_t osk_hit_test(int client_x, int client_y, int w, int h, uint8_t modifiers) {
    (void)client_x; (void)client_y; (void)w; (void)h; (void)modifiers;
    return 0;
}

#else

#include "drivers/canvas1.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"
#include "drivers/screen.h"

#define OSK_TOTAL_WEIGHT    120u

typedef struct {
    char norm[6];
    char shift[6];
    char sym[6];
    uint16_t code;
    uint16_t shift_code;
    uint16_t sym_code;
    uint8_t weight;
} osk_key_t;

static const osk_key_t k_row0[] LCDTERM_URODATA = {
    { "Esc", "Esc", "Esc", OSK_KEY_ESC,  OSK_KEY_ESC,  OSK_KEY_ESC,  10 },
    { "1",   "!",   "~",   '1',          '!',          '~',          10 },
    { "2",   "@",   "`",   '2',          '@',          '`',          10 },
    { "3",   "#",   "#",   '3',          '#',          '#',          10 },
    { "4",   "$",   "$",   '4',          '$',          '$',          10 },
    { "5",   "%",   "%",   '5',          '%',          '%',          10 },
    { "6",   "^",   "^",   '6',          '^',          '^',          10 },
    { "7",   "&",   "&",   '7',          '&',          '&',          10 },
    { "8",   "*",   "*",   '8',          '*',          '*',          10 },
    { "9",   "(",   "(",   '9',          '(',          '(',          10 },
    { "0",   ")",   ")",   '0',          ')',          ')',          10 },
    { "Del", "Del", "Del", OSK_KEY_BKSP, OSK_KEY_BKSP, OSK_KEY_BKSP, 10 },
};

static const osk_key_t k_row1[] LCDTERM_URODATA = {
    { "Tab", "Tab", "Tab", OSK_KEY_TAB,  OSK_KEY_TAB,  OSK_KEY_TAB,  10 },
    { "q",   "Q",   "{",   'q',          'Q',          '{',          10 },
    { "w",   "W",   "}",   'w',          'W',          '}',          10 },
    { "e",   "E",   "[",   'e',          'E',          '[',          10 },
    { "r",   "R",   "]",   'r',          'R',          ']',          10 },
    { "t",   "T",   "<",   't',          'T',          '<',          10 },
    { "y",   "Y",   ">",   'y',          'Y',          '>',          10 },
    { "u",   "U",   "=",   'u',          'U',          '=',          10 },
    { "i",   "I",   "+",   'i',          'I',          '+',          10 },
    { "o",   "O",   "*",   'o',          'O',          '*',          10 },
    { "p",   "P",   "\\",  'p',          'P',          '\\',         10 },
    { "-",   "_",   "|",   '-',          '_',          '|',          10 },
};

static const osk_key_t k_row2[] LCDTERM_URODATA = {
    { "Ctrl", "Ctrl", "Ctrl", OSK_KEY_CTRL,  OSK_KEY_CTRL,  OSK_KEY_CTRL,  12 },
    { "a",    "A",    "1",    'a',           'A',           '1',            9 },
    { "s",    "S",    "2",    's',           'S',           '2',            9 },
    { "d",    "D",    "3",    'd',           'D',           '3',            9 },
    { "f",    "F",    "4",    'f',           'F',           '4',            9 },
    { "g",    "G",    "5",    'g',           'G',           '5',            9 },
    { "h",    "H",    "6",    'h',           'H',           '6',            9 },
    { "j",    "J",    "7",    'j',           'J',           '7',            9 },
    { "k",    "K",    "8",    'k',           'K',           '8',            9 },
    { "l",    "L",    "9",    'l',           'L',           '9',            9 },
    { ";",    ":",    "0",    ';',           ':',           '0',            9 },
    { "Ret",  "Ret",  "Ret",  OSK_KEY_ENTER, OSK_KEY_ENTER, OSK_KEY_ENTER, 18 },
};

static const osk_key_t k_row3[] LCDTERM_URODATA = {
    { "Shft", "Shft", "Shft", OSK_KEY_SHIFT, OSK_KEY_SHIFT, OSK_KEY_SHIFT, 14 },
    { "z",    "Z",    "'",    'z',           'Z',           '\'',           9 },
    { "x",    "X",    "\"",   'x',           'X',           '\"',           9 },
    { "c",    "C",    "`",    'c',           'C',           '`',            9 },
    { "v",    "V",    ":",    'v',           'V',           ':',            9 },
    { "b",    "B",    ";",    'b',           'B',           ';',            9 },
    { "n",    "N",    "?",    'n',           'N',           '?',            9 },
    { "m",    "M",    "!",    'm',           'M',           '!',            9 },
    { ",",    "<",    ",",    ',',           '<',           ',',            9 },
    { ".",    ">",    ".",    '.',           '>',           '.',            9 },
    { "/",    "?",    "/",    '/',           '?',           '/',           10 },
    { "Shft", "Shft", "Shft", OSK_KEY_SHIFT, OSK_KEY_SHIFT, OSK_KEY_SHIFT, 15 },
};

static const osk_key_t k_row4[] LCDTERM_URODATA = {
    { "Sym",   "Sym",   "ABC",   OSK_KEY_SYM,    OSK_KEY_SYM,    OSK_KEY_SYM,    12 },
    { "^C",    "^C",    "^C",    OSK_KEY_CTRL_C, OSK_KEY_CTRL_C, OSK_KEY_CTRL_C, 10 },
    { "Space", "Space", "Space", ' ',            ' ',            ' ',            36 },
    { "Lt",    "Lt",    "Lt",    OSK_KEY_LEFT,   OSK_KEY_LEFT,   OSK_KEY_LEFT,   10 },
    { "Rt",    "Rt",    "Rt",    OSK_KEY_RIGHT,  OSK_KEY_RIGHT,  OSK_KEY_RIGHT,  10 },
    { "Up",    "Up",    "Up",    OSK_KEY_UP,     OSK_KEY_UP,     OSK_KEY_UP,     10 },
    { "Dn",    "Dn",    "Dn",    OSK_KEY_DOWN,   OSK_KEY_DOWN,   OSK_KEY_DOWN,   10 },
    { "Cmd",   "Cmd",   "Cmd",   OSK_KEY_CMD,    OSK_KEY_CMD,    OSK_KEY_CMD,    10 },
    { "Hide",  "Hide",  "Hide",  OSK_KEY_HIDE,   OSK_KEY_HIDE,   OSK_KEY_HIDE,   12 },
};

typedef struct {
    const osk_key_t *keys;
    uint8_t count;
} osk_row_t;

static const osk_row_t k_rows[5] LCDTERM_URODATA = {
    { k_row0, sizeof(k_row0) / sizeof(k_row0[0]) },
    { k_row1, sizeof(k_row1) / sizeof(k_row1[0]) },
    { k_row2, sizeof(k_row2) / sizeof(k_row2[0]) },
    { k_row3, sizeof(k_row3) / sizeof(k_row3[0]) },
    { k_row4, sizeof(k_row4) / sizeof(k_row4[0]) },
};

LCDTERM_UTEXT int osk_get_height(void) {
    int sc = (int)CHROME_SCALE;
    return 5 * 28 * sc + 8 * sc;
}

LCDTERM_UTEXT int osk_get_text_rows(int cell_h) {
    if (cell_h <= 0) return 0;
    int h = osk_get_height();
    return (h + cell_h - 1) / cell_h;
}

LCDTERM_UTEXT static unsigned cstr_len(const char *s) {
    unsigned n = 0;
    while (s && s[n]) n++;
    return n;
}

LCDTERM_UTEXT static void get_key_rect(int osk_x, int osk_y, int osk_w, int r_idx, int k_idx,
                                       int *out_x, int *out_y, int *out_w, int *out_h) {
    int sc = (int)CHROME_SCALE;
    int pad = 4 * sc;
    int gap = 3 * sc;
    int row_h = 25 * sc;
    int avail_w = osk_w - 2 * pad;
    const osk_row_t *row = &k_rows[r_idx];

    int total_gaps = (row->count - 1) * gap;
    int weight_w = avail_w - total_gaps;
    if (weight_w < (int)row->count) weight_w = (int)row->count;

    int cur_x = osk_x + pad;
    for (int k = 0; k < k_idx; k++) {
        int kw = (int)((uint32_t)row->keys[k].weight * (uint32_t)weight_w / OSK_TOTAL_WEIGHT);
        if (kw < 1) kw = 1;
        cur_x += kw + gap;
    }

    int kw = (int)((uint32_t)row->keys[k_idx].weight * (uint32_t)weight_w / OSK_TOTAL_WEIGHT);
    if (kw < 1) kw = 1;
    if (k_idx == row->count - 1) {
        kw = (osk_x + osk_w - pad) - cur_x;
        if (kw < 1) kw = 1;
    }

    *out_x = cur_x;
    *out_y = osk_y + pad + r_idx * (row_h + gap);
    *out_w = kw;
    *out_h = row_h;
}

LCDTERM_UTEXT static void draw_key_label(const canvas1_t *cv, int kx, int ky, int kw, int kh,
                                         const char *label, int sc) {
    if (!label) return;
    unsigned len = cstr_len(label);
    if (len == 0) return;

    int glyph_w = (sc == 2) ? 16 : 8;
    int glyph_h = (sc == 2) ? 32 : 16;
    int total_text_w = (int)len * glyph_w;

    int tx = kx + (kw - total_text_w) / 2;
    int ty = ky + (kh - glyph_h) / 2;
    if (tx < kx) tx = kx;

    if (sc == 1) {
        (void)canvas1_text(cv, tx, ty, label, len, len, 0, 16, false);
    } else {
        for (unsigned i = 0; i < len; i++) {
            uint8_t ch = (uint8_t)label[i];
            if (ch < FONT8X16_FIRST || ch > 0x7E) ch = '?';
            const uint8_t *glyph = font8x16_glyphs[ch - FONT8X16_FIRST];
            for (int r = 0; r < 16; r++) {
                uint8_t bits = glyph[r];
                for (int b = 0; b < 8; b++) {
                    if ((bits >> b) & 1u) {
                        int px = tx + (int)i * 16 + b * 2;
                        int py = ty + r * 2;
                        if (px >= 0 && px + 1 < (int)cv->w && py >= 0 && py + 1 < (int)cv->h) {
                            canvas1_fill(cv, px, py, px + 1, py + 1, CANVAS1_BLACK);
                        }
                    }
                }
            }
        }
    }
}

/* label in font8x16 at an integer scale, its top-left at (x, y). */
LCDTERM_UTEXT static void draw_label_scaled(const canvas1_t *cv, int x, int y, const char *label, int scale) {
    unsigned len = cstr_len(label);
    for (unsigned i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)label[i];
        if (ch < FONT8X16_FIRST || ch > 0x7E) ch = '?';
        const uint8_t *glyph = font8x16_glyphs[ch - FONT8X16_FIRST];
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++)
                if ((glyph[r] >> b) & 1u) {
                    int px = x + (int)i * 8 * scale + b * scale, py = y + r * scale;
                    canvas1_fill(cv, px, py, px + scale - 1, py + scale - 1, CANVAS1_BLACK);
                }
    }
}

/* The pressed character key's preview: its label at twice the keys' scale in
 * a framed bubble whose bottom sits on the key's top, kept inside the
 * keyboard (x0..x0+w-1, from y0). */
LCDTERM_UTEXT static void draw_key_preview(const canvas1_t *cv, int x0, int y0, int w,
                                           int kx, int ky, int kw, const char *label, int sc) {
    unsigned len = cstr_len(label);
    if (!len) return;
    int scale = 2 * sc;
    int pad = 6 * sc;
    int bw = (int)len * 8 * scale + 2 * pad;
    if (bw < kw + 2 * pad) bw = kw + 2 * pad;
    int bh = 16 * scale + 2 * pad;
    int bx = kx + kw / 2 - bw / 2;
    if (bx < x0 + 2 * sc) bx = x0 + 2 * sc;
    if (bx + bw > x0 + w - 4 * sc) bx = x0 + w - 4 * sc - bw;
    int by = ky - bh;
    if (by < y0 + 2 * sc) by = y0 + 2 * sc;
    int shadow = 2 * sc;
    canvas1_fill(cv, bx + shadow, by + bh, bx + bw + shadow - 1, by + bh + shadow - 1, CANVAS1_BLACK);
    canvas1_fill(cv, bx + bw, by + shadow, bx + bw + shadow - 1, by + bh + shadow - 1, CANVAS1_BLACK);
    canvas1_fill(cv, bx, by, bx + bw - 1, by + bh - 1, CANVAS1_WHITE);
    for (int t = 0; t < sc; t++) {
        canvas1_hline(cv, bx, bx + bw - 1, by + t, CANVAS1_BLACK);
        canvas1_hline(cv, bx, bx + bw - 1, by + bh - 1 - t, CANVAS1_BLACK);
        canvas1_vline(cv, bx + t, by, by + bh - 1, CANVAS1_BLACK);
        canvas1_vline(cv, bx + bw - 1 - t, by, by + bh - 1, CANVAS1_BLACK);
    }
    draw_label_scaled(cv, bx + (bw - (int)len * 8 * scale) / 2, by + pad, label, scale);
}

LCDTERM_UTEXT uint16_t osk_repeat_key(int pressed) {
    int r = pressed >= 0 ? pressed / 32 : -1, k = pressed >= 0 ? pressed % 32 : -1;
    if (r < 0 || r >= 5 || k >= k_rows[r].count) return 0;
    uint16_t code = k_rows[r].keys[k].code;
    return (code == OSK_KEY_BKSP || code == OSK_KEY_LEFT || code == OSK_KEY_RIGHT ||
            code == OSK_KEY_UP || code == OSK_KEY_DOWN) ? code : 0;
}

LCDTERM_UTEXT int osk_key_at(int client_x, int client_y, int w, int h) {
    if (client_x < 0 || client_y < 0 || client_x >= w || client_y >= h) return -1;
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < k_rows[r].count; k++) {
            int kx, ky, kw, kh;
            get_key_rect(0, 0, w, r, k, &kx, &ky, &kw, &kh);
            if (client_x >= kx && client_x < kx + kw && client_y >= ky && client_y < ky + kh) return r * 32 + k;
        }
    return -1;
}

LCDTERM_UTEXT void osk_draw(const canvas1_t *cv, int x0, int y0, int w, int h, uint8_t modifiers, int pressed) {
    if (!cv || w <= 0 || h <= 0) return;
    int sc = (int)CHROME_SCALE;
    bool s_shift = (modifiers & OSK_MOD_SHIFT) != 0;
    bool s_ctrl  = (modifiers & OSK_MOD_CTRL)  != 0;
    bool s_sym   = (modifiers & OSK_MOD_SYM)   != 0;
    bool s_cmd   = (modifiers & OSK_MOD_CMD)   != 0;

    /* Keyboard container background fill (clean white) */
    canvas1_fill(cv, x0, y0, x0 + w - 1, y0 + h - 1, CANVAS1_WHITE);

    /* 1px Classic Mac black border line along top edge */
    canvas1_hline(cv, x0, x0 + w - 1, y0, CANVAS1_BLACK);

    /* Draw every key button with Classic Mac drop shadow and border */
    for (int r = 0; r < 5; r++) {
        const osk_row_t *row = &k_rows[r];
        for (int k = 0; k < row->count; k++) {
            const osk_key_t *key = &row->keys[k];
            int kx, ky, kw, kh;
            get_key_rect(x0, y0, w, r, k, &kx, &ky, &kw, &kh);

            bool is_active = false;
            if (key->code == OSK_KEY_SHIFT && s_shift) is_active = true;
            if (key->code == OSK_KEY_CTRL && s_ctrl)   is_active = true;
            if (key->code == OSK_KEY_SYM && s_sym)     is_active = true;
            if (key->code == OSK_KEY_CMD && s_cmd)     is_active = true;

            int shadow = 2 * sc;
            /* Drop shadow bottom and right */
            canvas1_fill(cv, kx + shadow, ky + kh, kx + kw + shadow - 1, ky + kh + shadow - 1, CANVAS1_BLACK);
            canvas1_fill(cv, kx + kw, ky + shadow, kx + kw + shadow - 1, ky + kh + shadow - 1, CANVAS1_BLACK);

            /* Key interior fill */
            canvas1_fill(cv, kx, ky, kx + kw - 1, ky + kh - 1, CANVAS1_WHITE);

            /* Outer 1px black border */
            canvas1_hline(cv, kx, kx + kw - 1, ky, CANVAS1_BLACK);
            canvas1_hline(cv, kx, kx + kw - 1, ky + kh - 1, CANVAS1_BLACK);
            canvas1_vline(cv, kx, ky, ky + kh - 1, CANVAS1_BLACK);
            canvas1_vline(cv, kx + kw - 1, ky, ky + kh - 1, CANVAS1_BLACK);

            /* Pick current key label */
            const char *label = key->norm;
            if (s_sym) label = key->sym;
            else if (s_shift) label = key->shift;

            draw_key_label(cv, kx, ky, kw, kh, label, sc);

            /* Invert active modifier keys and the pressed key (classic Mac
             * inverted button style) */
            if (is_active || pressed == r * 32 + k) {
                canvas1_invert(cv, kx, ky, kx + kw - 1, ky + kh - 1);
            }
        }
    }

    /* The pressed character key's preview, over the rows above it (the top
     * row has none: it would cover the terminal). */
    int pr = pressed >= 0 ? pressed / 32 : -1, pk = pressed >= 0 ? pressed % 32 : -1;
    if (pr > 0 && pr < 5 && pk < k_rows[pr].count) {
        const osk_key_t *key = &k_rows[pr].keys[pk];
        const char *label = s_sym ? key->sym : s_shift ? key->shift : key->norm;
        bool character = key->code >= 0x21 && key->code < 0x7f && cstr_len(label) == 1;
        if (character) {
            int kx, ky, kw, kh;
            get_key_rect(x0, y0, w, pr, pk, &kx, &ky, &kw, &kh);
            draw_key_preview(cv, x0, y0, w, kx, ky, kw, label, sc);
        }
    }
}

LCDTERM_UTEXT void osk_draw_toggle_button(const canvas1_t *cv, int btn_x, int btn_y, int btn_w, int btn_h) {
    if (!cv || btn_w <= 0 || btn_h <= 0) return;
    int sc = (int)CHROME_SCALE;
    int shadow = 2 * sc;

    /* Drop shadow bottom and right */
    canvas1_fill(cv, btn_x + shadow, btn_y + btn_h, btn_x + btn_w + shadow - 1, btn_y + btn_h + shadow - 1, CANVAS1_BLACK);
    canvas1_fill(cv, btn_x + btn_w, btn_y + shadow, btn_x + btn_w + shadow - 1, btn_y + btn_h + shadow - 1, CANVAS1_BLACK);

    /* Button fill and border */
    canvas1_fill(cv, btn_x, btn_y, btn_x + btn_w - 1, btn_y + btn_h - 1, CANVAS1_WHITE);
    canvas1_hline(cv, btn_x, btn_x + btn_w - 1, btn_y, CANVAS1_BLACK);
    canvas1_hline(cv, btn_x, btn_x + btn_w - 1, btn_y + btn_h - 1, CANVAS1_BLACK);
    canvas1_vline(cv, btn_x, btn_y, btn_y + btn_h - 1, CANVAS1_BLACK);
    canvas1_vline(cv, btn_x + btn_w - 1, btn_y, btn_y + btn_h - 1, CANVAS1_BLACK);

    char btn_label[6];
    btn_label[0] = 'K'; btn_label[1] = 'e'; btn_label[2] = 'y'; btn_label[3] = 'b'; btn_label[4] = 'd'; btn_label[5] = '\0';
    draw_key_label(cv, btn_x, btn_y, btn_w, btn_h, btn_label, sc);
}

LCDTERM_UTEXT uint16_t osk_hit_test(int client_x, int client_y, int w, int h, uint8_t modifiers) {
    if (client_x < 0 || client_y < 0 || client_x >= w || client_y >= h) return 0;
    bool s_shift = (modifiers & OSK_MOD_SHIFT) != 0;
    bool s_ctrl  = (modifiers & OSK_MOD_CTRL)  != 0;
    bool s_sym   = (modifiers & OSK_MOD_SYM)   != 0;

    /* Hit test across rows and keys */
    for (int r = 0; r < 5; r++) {
        const osk_row_t *row = &k_rows[r];
        for (int k = 0; k < row->count; k++) {
            int kx, ky, kw, kh;
            get_key_rect(0, 0, w, r, k, &kx, &ky, &kw, &kh);
            if (client_x >= kx && client_x < kx + kw &&
                client_y >= ky && client_y < ky + kh) {
                const osk_key_t *key = &row->keys[k];

                if (key->code == OSK_KEY_SHIFT || key->code == OSK_KEY_CTRL ||
                    key->code == OSK_KEY_SYM   || key->code == OSK_KEY_HIDE ||
                    key->code == OSK_KEY_CMD   ||
                    key->code == OSK_KEY_UP    || key->code == OSK_KEY_DOWN ||
                    key->code == OSK_KEY_LEFT  || key->code == OSK_KEY_RIGHT ||
                    key->code == OSK_KEY_CTRL_C) {
                    return key->code;
                }

                /* Resolve character code based on modifiers */
                uint16_t code = key->code;
                if (s_sym) code = key->sym_code;
                else if (s_shift) code = key->shift_code;

                if (s_ctrl) {
                    if (code >= 'a' && code <= 'z') code = (uint16_t)(code - 'a' + 1);
                    else if (code >= 'A' && code <= 'Z') code = (uint16_t)(code - 'A' + 1);
                }
                return code;
            }
        }
    }
    return 0xFFFF; /* Tap was on keyboard container padding */
}

#endif /* TOUCH_PRESENT */
