/* drivers/osk.c — Classic Mac On-Screen Touch Keyboard
 *
 * Implements an on-screen keyboard (OSK) for terminal windows on touch-first
 * devices when no physical keyboard is attached:
 * - 5-row laptop/terminal layout (Esc, Numbers, QWERTY, Shift, Space, Arrows, ^C, Enter)
 * - Classic Mac beveled button aesthetics using 1-bpp canvas
 * - Direct coordinate geometry hit testing (zero dynamic allocation, no hitbox limits)
 * - Sticky Shift, sticky Ctrl, and Symbols mode toggle
 * - Feeds keys directly into active vterm
 * - Scale-aware (CHROME_SCALE = 1 on 8x16 panels, 2 on 16x32 panels)
 * - Zero float in M-mode, freestanding C, LCDTERM_UTEXT for U-mode compatibility.
 */

#include "drivers/osk.h"
#include "lugalos_config.h"

#if !defined(TOUCH_PRESENT)

bool osk_is_visible(void) { return false; }
void osk_set_visible(bool visible) { (void)visible; }
void osk_toggle(void) {}
int osk_get_height(void) { return 0; }
int osk_get_text_rows(int cell_h) { (void)cell_h; return 0; }
void osk_draw(const canvas1_t *cv, int x0, int y0, int w, int h) { (void)cv; (void)x0; (void)y0; (void)w; (void)h; }
void osk_draw_toggle_button(const canvas1_t *cv, int btn_x, int btn_y, int btn_w, int btn_h) { (void)cv; (void)btn_x; (void)btn_y; (void)btn_w; (void)btn_h; }
bool osk_handle_tap(int client_x, int client_y, int w, int h) { (void)client_x; (void)client_y; (void)w; (void)h; return false; }
bool osk_handle_toggle_tap(int client_x, int client_y, int btn_x, int btn_y, int btn_w, int btn_h) { (void)client_x; (void)client_y; (void)btn_x; (void)btn_y; (void)btn_w; (void)btn_h; return false; }

#else

#include "drivers/canvas1.h"
#include "drivers/font8x16.h"
#include "drivers/lcdterm_attr.h"
#include "drivers/screen.h"
#include "drivers/usbkbd_task.h"
#include "kernel/console.h"
#include "kernel/vterm.h"

#define OSK_TOTAL_WEIGHT    120u

#define OSK_KEY_NONE        0
#define OSK_KEY_ESC         0x1B
#define OSK_KEY_TAB         '\t'
#define OSK_KEY_ENTER       '\r'
#define OSK_KEY_BKSP        0x7F
#define OSK_KEY_SHIFT       0x101
#define OSK_KEY_CTRL        0x102
#define OSK_KEY_SYM         0x103
#define OSK_KEY_HIDE        0x104
#define OSK_KEY_UP          0x105
#define OSK_KEY_DOWN        0x106
#define OSK_KEY_LEFT        0x107
#define OSK_KEY_RIGHT       0x108
#define OSK_KEY_CTRL_C      0x109

typedef struct {
    const char *norm;
    const char *shift;
    const char *sym;
    uint16_t code;
    uint16_t shift_code;
    uint16_t sym_code;
    uint8_t weight;
} osk_key_t;

static const osk_key_t k_row0[] = {
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

static const osk_key_t k_row1[] = {
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

static const osk_key_t k_row2[] = {
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

static const osk_key_t k_row3[] = {
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

static const osk_key_t k_row4[] = {
    { "Sym",   "Sym",   "ABC",   OSK_KEY_SYM,    OSK_KEY_SYM,    OSK_KEY_SYM,    12 },
    { "^C",    "^C",    "^C",    OSK_KEY_CTRL_C, OSK_KEY_CTRL_C, OSK_KEY_CTRL_C, 10 },
    { "(",     "(",     "[",     '(',            '(',            '[',             9 },
    { ")",     ")",     "]",     ')',            ')',            ']',             9 },
    { "Space", "Space", "Space", ' ',            ' ',            ' ',            40 },
    { "/",     "\\",    "|",     '/',            '\\',           '|',             9 },
    { "Up",    "Up",    "Up",    OSK_KEY_UP,     OSK_KEY_UP,     OSK_KEY_UP,     10 },
    { "Dn",    "Dn",    "Dn",    OSK_KEY_DOWN,   OSK_KEY_DOWN,   OSK_KEY_DOWN,   10 },
    { "Hide",  "Hide",  "Hide",  OSK_KEY_HIDE,   OSK_KEY_HIDE,   OSK_KEY_HIDE,   11 },
};

typedef struct {
    const osk_key_t *keys;
    uint8_t count;
} osk_row_t;

static const osk_row_t k_rows[5] = {
    { k_row0, sizeof(k_row0) / sizeof(k_row0[0]) },
    { k_row1, sizeof(k_row1) / sizeof(k_row1[0]) },
    { k_row2, sizeof(k_row2) / sizeof(k_row2[0]) },
    { k_row3, sizeof(k_row3) / sizeof(k_row3[0]) },
    { k_row4, sizeof(k_row4) / sizeof(k_row4[0]) },
};

/* single task: screen task */
static bool s_osk_visible = false;
/* single task: screen task */
static bool s_shift = false;
/* single task: screen task */
static bool s_ctrl = false;
/* single task: screen task */
static bool s_sym = false;
/* single task: screen task */
static bool s_initialized = false;

LCDTERM_UTEXT bool osk_is_visible(void) {
    if (!s_initialized) {
        s_initialized = true;
        s_osk_visible = !usbkbd_is_present();
    }
    return s_osk_visible;
}

LCDTERM_UTEXT void osk_set_visible(bool visible) {
    s_initialized = true;
    s_osk_visible = visible;
}

LCDTERM_UTEXT void osk_toggle(void) {
    s_initialized = true;
    s_osk_visible = !s_osk_visible;
}

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
    int pad_x = 4 * sc;
    int pad_y = 4 * sc;
    int row_h = 28 * sc;
    int gap_x = 2 * sc;
    int gap_y = 2 * sc;

    int avail_w = osk_w - 2 * pad_x;
    if (avail_w < 60) avail_w = 60;

    int y = osk_y + pad_y + r_idx * row_h;
    int h = row_h - gap_y;

    const osk_row_t *row = &k_rows[r_idx];
    uint32_t w_accum = 0;
    for (int i = 0; i < k_idx; i++) {
        w_accum += row->keys[i].weight;
    }

    int x_start = osk_x + pad_x + (int)((uint32_t)avail_w * w_accum / OSK_TOTAL_WEIGHT);
    int x_end   = osk_x + pad_x + (int)((uint32_t)avail_w * (w_accum + row->keys[k_idx].weight) / OSK_TOTAL_WEIGHT);
    int w = (x_end - x_start) - gap_x;
    if (w < 4) w = 4;

    *out_x = x_start;
    *out_y = y;
    *out_w = w;
    *out_h = h;
}

LCDTERM_UTEXT static void draw_key_label(const canvas1_t *cv, int kx, int ky, int kw, int kh,
                                         const char *label, unsigned fg_pat, int sc) {
    if (!cv || !label || !*label) return;
    unsigned len = cstr_len(label);
    int cw = 8 * sc;
    int ch = 16 * sc;
    int tw = (int)len * cw;
    int tx = kx + (kw - tw) / 2;
    int ty = ky + (kh - ch) / 2;
    if (tx < kx + sc) tx = kx + sc;

    if (sc == 1) {
        /* Standard 8x16 1-bpp font */
        for (unsigned i = 0; i < len; i++) {
            uint8_t code = (uint8_t)label[i];
            if (code < FONT8X16_FIRST) code = FONT8X16_REPLACEMENT;
            const uint8_t *g = font8x16_glyphs[code - FONT8X16_FIRST];
            for (int r = 0; r < 16; r++) {
                uint8_t bits = g[r];
                if (!bits) continue;
                for (int b = 0; b < 8; b++) {
                    if ((bits >> b) & 1u) {
                        int px = tx + (int)i * 8 + b;
                        int py = ty + r;
                        if (px >= 0 && px < (int)cv->w && py >= 0 && py < (int)cv->h) {
                            canvas1_fill(cv, px, py, px, py, fg_pat);
                        }
                    }
                }
            }
        }
    } else {
        /* 2x scaled 16x32 font */
        for (unsigned i = 0; i < len; i++) {
            uint8_t code = (uint8_t)label[i];
            if (code < FONT8X16_FIRST) code = FONT8X16_REPLACEMENT;
            const uint8_t *g = font8x16_glyphs[code - FONT8X16_FIRST];
            for (int r = 0; r < 16; r++) {
                uint8_t bits = g[r];
                if (!bits) continue;
                for (int b = 0; b < 8; b++) {
                    if ((bits >> b) & 1u) {
                        int px = tx + (int)i * 16 + b * 2;
                        int py = ty + r * 2;
                        if (px >= 0 && px + 1 < (int)cv->w && py >= 0 && py + 1 < (int)cv->h) {
                            canvas1_fill(cv, px, py, px + 1, py + 1, fg_pat);
                        }
                    }
                }
            }
        }
    }
}

LCDTERM_UTEXT void osk_draw(const canvas1_t *cv, int x0, int y0, int w, int h) {
    if (!cv || w <= 0 || h <= 0) return;
    int sc = (int)CHROME_SCALE;

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

            int shadow = 2 * sc;
            /* Drop shadow bottom and right */
            canvas1_fill(cv, kx + shadow, ky + kh, kx + kw + shadow - 1, ky + kh + shadow - 1, CANVAS1_BLACK);
            canvas1_fill(cv, kx + kw, ky + shadow, kx + kw + shadow - 1, ky + kh + shadow - 1, CANVAS1_BLACK);

            /* Key interior fill */
            unsigned bg_pat = is_active ? CANVAS1_BLACK : CANVAS1_WHITE;
            unsigned fg_pat = is_active ? CANVAS1_WHITE : CANVAS1_BLACK;
            canvas1_fill(cv, kx, ky, kx + kw - 1, ky + kh - 1, bg_pat);

            /* Outer 1px black border */
            canvas1_hline(cv, kx, kx + kw - 1, ky, CANVAS1_BLACK);
            canvas1_hline(cv, kx, kx + kw - 1, ky + kh - 1, CANVAS1_BLACK);
            canvas1_vline(cv, kx, ky, ky + kh - 1, CANVAS1_BLACK);
            canvas1_vline(cv, kx + kw - 1, ky, ky + kh - 1, CANVAS1_BLACK);

            /* Pick current key label */
            const char *label = key->norm;
            if (s_sym) label = key->sym;
            else if (s_shift) label = key->shift;

            draw_key_label(cv, kx, ky, kw, kh, label, fg_pat, sc);
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

    draw_key_label(cv, btn_x, btn_y, btn_w, btn_h, "Keybd", CANVAS1_BLACK, sc);
}

LCDTERM_UTEXT bool osk_handle_tap(int client_x, int client_y, int w, int h) {
    if (client_x < 0 || client_y < 0 || client_x >= w || client_y >= h) return false;

    /* Hit test across rows and keys */
    for (int r = 0; r < 5; r++) {
        const osk_row_t *row = &k_rows[r];
        for (int k = 0; k < row->count; k++) {
            int kx, ky, kw, kh;
            get_key_rect(0, 0, w, r, k, &kx, &ky, &kw, &kh);
            if (client_x >= kx && client_x < kx + kw &&
                client_y >= ky && client_y < ky + kh) {
                const osk_key_t *key = &row->keys[k];

                if (key->code == OSK_KEY_SHIFT) {
                    s_shift = !s_shift;
                    return true;
                }
                if (key->code == OSK_KEY_CTRL) {
                    s_ctrl = !s_ctrl;
                    return true;
                }
                if (key->code == OSK_KEY_SYM) {
                    s_sym = !s_sym;
                    return true;
                }
                if (key->code == OSK_KEY_HIDE) {
                    s_osk_visible = false;
                    return true;
                }

                vterm_t *vt = vterm_active();

                if (key->code == OSK_KEY_UP) {
                    if (vt) vterm_feed_bytes(vt, "\x1b[A", 3);
                    return true;
                }
                if (key->code == OSK_KEY_DOWN) {
                    if (vt) vterm_feed_bytes(vt, "\x1b[B", 3);
                    return true;
                }
                if (key->code == OSK_KEY_LEFT) {
                    if (vt) vterm_feed_bytes(vt, "\x1b[D", 3);
                    return true;
                }
                if (key->code == OSK_KEY_RIGHT) {
                    if (vt) vterm_feed_bytes(vt, "\x1b[C", 3);
                    return true;
                }
                if (key->code == OSK_KEY_CTRL_C) {
                    if (vt) vterm_feed_char(vt, 0x03);
                    return true;
                }

                /* Resolve character code based on modifiers */
                uint16_t code = key->code;
                if (s_sym) code = key->sym_code;
                else if (s_shift) code = key->shift_code;

                if (s_ctrl) {
                    if (code >= 'a' && code <= 'z') code = (uint16_t)(code - 'a' + 1);
                    else if (code >= 'A' && code <= 'Z') code = (uint16_t)(code - 'A' + 1);
                    s_ctrl = false;
                }
                if (s_shift) {
                    s_shift = false;
                }

                if (vt && code > 0 && code < 0x100) {
                    vterm_feed_char(vt, (char)code);
                }
                return true;
            }
        }
    }
    return true; /* Tap was on keyboard container padding */
}

LCDTERM_UTEXT bool osk_handle_toggle_tap(int client_x, int client_y, int btn_x, int btn_y, int btn_w, int btn_h) {
    if (client_x >= btn_x && client_x < btn_x + btn_w &&
        client_y >= btn_y && client_y < btn_y + btn_h) {
        osk_set_visible(true);
        return true;
    }
    return false;
}

#endif /* TOUCH_PRESENT */
