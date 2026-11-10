/* USB boot keyboard, portable half.
 * See drivers/include/drivers/usbkbd.h.
 *
 * Everything above the selftest runs in the U-mode `kbd` task on the
 * RP2350-LCD-7 (KBD_UTEXT): no libc, no switch (jump tables are .rodata
 * outside the task's domain; the file is also built -fno-jump-tables), no
 * string literals. */

#include "drivers/usbkbd.h"
#include "kernel/console.h"
#include "kernel/keyseq.h"
#include "kernel/wmkeys.h"

KBD_UTEXT bool usb_parse_config(const uint8_t *cfg, uint32_t len, usb_cfg_info_t *out) {
    uint8_t *o = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) o[i] = 0;
    if (len < 9 || cfg[1] != 2) return false;
    out->cfg_value = cfg[5];
    uint32_t cls = 0, sub = 0, proto = 0, alt = 0, iface = 0, hid_len = 0;
    uint32_t at = 0;
    while (at + 2 <= len) {
        uint32_t bl = cfg[at], type = cfg[at + 1];
        if (bl < 2 || at + bl > len) break;
        if (type == 4 && bl >= 9) {                         /* interface */
            iface = cfg[at + 2]; alt = cfg[at + 3];
            cls = cfg[at + 5]; sub = cfg[at + 6]; proto = cfg[at + 7];
            hid_len = 0;
            if (cls == 9) out->is_hub = 1;
        } else if (type == 0x21 && bl >= 9) {               /* HID: its report descriptor's length */
            hid_len = (uint32_t)(cfg[at + 7] | (cfg[at + 8] << 8));
        } else if (type == 5 && bl >= 7) {                  /* endpoint */
            uint32_t ea = cfg[at + 2], attr = cfg[at + 3] & 3u;
            bool int_in = (ea & 0x80u) && attr == 3u;
            if (int_in && alt == 0 && cls == 3 && sub == 1 && proto == 1 && !out->kbd_found) {
                out->kbd_found = 1;
                out->kbd_iface = (uint8_t)iface;
                out->kbd_ep = (uint8_t)(ea & 0x0fu);
                out->kbd_mps = cfg[at + 4];
                out->kbd_interval = cfg[at + 6];
            }
            if (int_in && alt == 0 && cls == 3 && sub == 1 && proto == 2 && !out->mouse_found) {
                out->mouse_found = 1;
                out->mouse_iface = (uint8_t)iface;
                out->mouse_ep = (uint8_t)(ea & 0x0fu);
                out->mouse_mps = cfg[at + 4];
                out->mouse_interval = cfg[at + 6];
                out->mouse_rdesc_len = (uint16_t)hid_len;
            }
            if (int_in && cls == 9 && !out->hub_ep) out->hub_ep = (uint8_t)(ea & 0x0fu);
        }
        at += bl;
    }
    return true;
}

KBD_UTEXT void usbmouse_boot_layout(usbmouse_layout_t *l) {
    l->valid = 1;
    l->report_id = 0;
    l->buttons = 3;
    l->btn_bit = 0;
    l->x_bit = 8;  l->x_size = 8;
    l->y_bit = 16; l->y_size = 8;
    l->wheel_bit = 24; l->wheel_size = 8;
}

#define RD_IDS     16u    /* report IDs whose bit offsets are kept apart; others share the last */
#define RD_USAGES  8u

KBD_UTEXT bool usbmouse_parse_report_desc(const uint8_t *d, uint32_t len, usbmouse_layout_t *l) {
    uint8_t *o = (uint8_t *)l;
    for (uint32_t i = 0; i < sizeof(*l); i++) o[i] = 0;
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0;
    uint32_t usages[RD_USAGES], nu = 0, umin = 0;
    bool range = false;
    uint32_t off[RD_IDS];
    for (uint32_t i = 0; i < RD_IDS; i++) off[i] = 0;
    uint32_t xy_id = 0xFFFFFFFFu, btn_id = 0xFFFFFFFFu;
    uint32_t i = 0;
    while (i < len) {
        uint32_t b = d[i];
        if (b == 0xFEu) {                                   /* a long item: skipped */
            if (i + 2u >= len) break;
            i += 3u + d[i + 1];
            continue;
        }
        uint32_t sz = b & 3u;
        if (sz == 3u) sz = 4u;
        if (i + 1u + sz > len) break;
        uint32_t v = 0;
        for (uint32_t k = 0; k < sz; k++) v |= (uint32_t)d[i + 1u + k] << (8u * k);
        uint32_t type = (b >> 2) & 3u, tag = b >> 4;
        i += 1u + sz;
        if (type == 1u) {                                   /* global */
            if (tag == 0u) page = v;
            else if (tag == 7u) rsize = v;
            else if (tag == 9u) rcount = v;
            else if (tag == 8u) rid = v & 0xFFu;
        } else if (type == 2u) {                            /* local */
            if (tag == 0u) { if (nu < RD_USAGES) usages[nu++] = v & 0xFFFFu; }
            else if (tag == 1u) { umin = v; range = true; }
        } else if (type == 0u) {                            /* main */
            if (tag == 8u) {                                /* Input */
                uint32_t slot = rid < RD_IDS ? rid : RD_IDS - 1u;
                uint32_t base = off[slot];
                bool constant = (v & 1u) != 0;
                for (uint32_t f = 0; f < rcount && !constant; f++) {
                    uint32_t u = f < nu ? usages[f] : range ? umin + f : nu ? usages[nu - 1u] : 0;
                    uint32_t bit = base + f * rsize;
                    if (bit > 0xFFFFu || rsize > 16u) continue;
                    if (page == 9u && rsize == 1u) {
                        if (btn_id == 0xFFFFFFFFu) { btn_id = rid; l->btn_bit = (uint16_t)bit; }
                        if (btn_id == rid && bit == (uint32_t)l->btn_bit + l->buttons && l->buttons < 8u) l->buttons++;
                    } else if (page == 1u && (xy_id == 0xFFFFFFFFu || xy_id == rid)) {
                        if (u == 0x30u && !l->x_size) { l->x_bit = (uint16_t)bit; l->x_size = (uint8_t)rsize; xy_id = rid; }
                        else if (u == 0x31u && !l->y_size) { l->y_bit = (uint16_t)bit; l->y_size = (uint8_t)rsize; xy_id = rid; }
                        else if (u == 0x38u && !l->wheel_size) { l->wheel_bit = (uint16_t)bit; l->wheel_size = (uint8_t)rsize; }
                    }
                }
                off[slot] = base + rcount * rsize;
            }
            nu = 0;                                         /* every main item ends the locals */
            range = false;
            umin = 0;
        }
    }
    if (xy_id == 0xFFFFFFFFu || btn_id != xy_id) l->buttons = 0;
    l->report_id = (uint8_t)(xy_id == 0xFFFFFFFFu ? 0u : xy_id);
    l->valid = (uint8_t)(l->x_size && l->y_size && l->buttons);
    return l->valid != 0;
}

/* size bits from bit on, little-endian as HID packs them; sign-extended. */
KBD_UTEXT static int32_t rd_field(const uint8_t *r, uint32_t n, uint32_t bit, uint32_t size, bool sign) {
    uint32_t v = 0;
    for (uint32_t k = 0; k < size; k++) {
        uint32_t at = bit + k;
        if (at / 8u >= n) return 0;
        v |= (uint32_t)((r[at / 8u] >> (at % 8u)) & 1u) << k;
    }
    if (sign && size > 0u && size < 32u && ((v >> (size - 1u)) & 1u)) v |= ~0u << size;
    return (int32_t)v;
}

KBD_UTEXT bool usbmouse_decode(const usbmouse_layout_t *l, const uint8_t *r, uint32_t n, usbmouse_ev_t *ev) {
    if (!l->valid) return false;
    if (l->report_id) {
        if (n < 1u || r[0] != l->report_id) return false;
        r++;
        n--;
    }
    if (n * 8u < (uint32_t)l->y_bit + l->y_size || n * 8u < (uint32_t)l->x_bit + l->x_size) return false;
    int32_t dx = rd_field(r, n, l->x_bit, l->x_size, true);
    int32_t dy = rd_field(r, n, l->y_bit, l->y_size, true);
    int32_t w = 0;
    if (l->wheel_size && n * 8u >= (uint32_t)l->wheel_bit + l->wheel_size)
        w = rd_field(r, n, l->wheel_bit, l->wheel_size, true);
    ev->dx = (int16_t)(dx < -32768 ? -32768 : dx > 32767 ? 32767 : dx);
    ev->dy = (int16_t)(dy < -32768 ? -32768 : dy > 32767 ? 32767 : dy);
    ev->wheel = (int8_t)(w < -127 ? -127 : w > 127 ? 127 : w);
    ev->buttons = (uint8_t)(rd_field(r, n, l->btn_bit, l->buttons < 3u ? l->buttons : 3u, false) & 7);
    return true;
}

KBD_UTEXT static bool has_key(const uint8_t r[8], uint8_t k) {
    for (int i = 2; i < 8; i++) if (r[i] == k) return true;
    return false;
}

KBD_UTEXT int usbkbd_diff(const uint8_t prev[8], const uint8_t cur[8], uint32_t *ev, uint32_t max) {
    if (cur[2] == 0x01) return -1;          /* ErrorRollOver: says nothing */
    uint32_t n = 0, mods = (uint32_t)cur[0] << 16;
    for (uint32_t b = 0; b < 8; b++) {
        uint32_t was = (prev[0] >> b) & 1u, is = (cur[0] >> b) & 1u;
        if (was != is && n < max) ev[n++] = (0xE0u + b) | (is ? USBKBD_EV_MAKE : 0u) | mods;
    }
    for (int i = 2; i < 8; i++)             /* releases before presses */
        if (prev[i] > 3 && !has_key(cur, prev[i]) && n < max) ev[n++] = prev[i] | mods;
    for (int i = 2; i < 8; i++)
        if (cur[i] > 3 && !has_key(prev, cur[i]) && n < max) ev[n++] = cur[i] | USBKBD_EV_MAKE | mods;
    return (int)n;
}

/* --- Events to bytes (kernel side) ---------------------------------------- */

/* Usages 0x04-0x38 (letters, digits, punctuation, Enter/Esc/Backspace/Tab/
 * Space): unshifted and shifted. 0 = no byte. */
static const char k_us_plain[0x39 - 0x04] = {
    'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t',
    'u','v','w','x','y','z','1','2','3','4','5','6','7','8','9','0',
    '\r', 0x1b, 0x7f, '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/',
};
static const char k_us_shift[0x39 - 0x04] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T',
    'U','V','W','X','Y','Z','!','@','#','$','%','^','&','*','(',')',
    '\r', 0x1b, 0x7f, '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?',
};
/* Keypad 0x54-0x63 with Num Lock on: / * - + Enter 1-9 0 . */
static const char k_keypad[0x64 - 0x54] = {
    '/', '*', '-', '+', '\r', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
};

#define MOD_CTRL  0x11u     /* left or right */
#define MOD_SHIFT 0x22u
#define MOD_ALT   0x44u     /* 37.5a */
#define MOD_GUI   0x88u     /* Super, Cmd, the Windows key */

/* xterm's modifier parameter: 1 + Shift 1 + Alt 2 + Ctrl 4 + Super 8. */
static unsigned xterm_mod(uint32_t mods) {
    return 1u + ((mods & MOD_SHIFT) ? 1u : 0u) + ((mods & MOD_ALT) ? 2u : 0u) +
           ((mods & MOD_CTRL) ? 4u : 0u) + ((mods & MOD_GUI) ? 8u : 0u);
}

static uint32_t put_dec(uint8_t *out, unsigned v) {
    char tmp[4];
    uint32_t n = 0, k = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 4u);
    while (n) out[k++] = (uint8_t)tmp[--n];
    return k;
}

/* ESC [ a ; m final -- a navigation key with modifiers, or CSI-u. */
static uint32_t seq_mod(uint8_t *out, unsigned a, unsigned m, char final) {
    uint32_t n = 0;
    out[n++] = 0x1b; out[n++] = '[';
    n += put_dec(out + n, a);
    out[n++] = ';';
    n += put_dec(out + n, m);
    out[n++] = (uint8_t)final;
    return n;
}

static uint32_t seq(uint8_t *out, char final, char digit) {
    out[0] = 0x1b; out[1] = '[';
    if (!digit) { out[2] = (uint8_t)final; return 3; }
    out[2] = (uint8_t)digit; out[3] = '~';
    return 4;
}

/* The bytes one usage produces with modifiers `mods`; 0 for none. */
static uint32_t usage_bytes(uint32_t u, uint32_t mods, uint8_t *out) {
    bool shift = (mods & MOD_SHIFT) != 0, ctrl = (mods & MOD_CTRL) != 0;
    bool alt = (mods & MOD_ALT) != 0, gui = (mods & MOD_GUI) != 0;
    if (u >= 0x04 && u <= 0x38) {
        if (gui) {                              /* CSI-u, the unshifted key */
            char base = k_us_plain[u - 0x04];
            return base ? seq_mod(out, (unsigned char)base, xterm_mod(mods), 'u') : 0;
        }
        char c = shift ? k_us_shift[u - 0x04] : k_us_plain[u - 0x04];
        if (!c) return 0;
        if (ctrl && c == ' ') {                 /* Ctrl-Space: NUL, the Emacs mark */
            out[0] = 0;
            return 1;
        }
        if (ctrl) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
            else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
            else if (c == '[') c = 0x1b;
            else if (c == '\\') c = 0x1c;
            else if (c == ']') c = 0x1d;
            else if (c != '\r' && c != 0x7f && c != '\t' && c != 0x1b) return 0;
        }
        if (alt) {                              /* Alt: an ESC prefix */
            out[0] = 0x1b;
            out[1] = (uint8_t)c;
            return 2;
        }
        out[0] = (uint8_t)c;
        return 1;
    }
    if (u >= 0x54 && u <= 0x63) { out[0] = (uint8_t)k_keypad[u - 0x54]; return 1; }
    if (mods & (MOD_SHIFT | MOD_ALT | MOD_CTRL | MOD_GUI)) {
        unsigned m = xterm_mod(mods);
        if (u == 0x52) return seq_mod(out, 1, m, 'A');
        if (u == 0x51) return seq_mod(out, 1, m, 'B');
        if (u == 0x4f) return seq_mod(out, 1, m, 'C');
        if (u == 0x50) return seq_mod(out, 1, m, 'D');
        if (u == 0x4a) return seq_mod(out, 1, m, 'H');
        if (u == 0x4d) return seq_mod(out, 1, m, 'F');
        if (u == 0x49) return seq_mod(out, 2, m, '~');
        if (u == 0x4c) return seq_mod(out, 3, m, '~');
        if (u == 0x4b) return seq_mod(out, 5, m, '~');
        if (u == 0x4e) return seq_mod(out, 6, m, '~');
    }
    if (u == 0x52) return seq(out, 'A', 0);     /* Up */
    if (u == 0x51) return seq(out, 'B', 0);     /* Down */
    if (u == 0x4f) return seq(out, 'C', 0);     /* Right */
    if (u == 0x50) return seq(out, 'D', 0);     /* Left */
    if (u == 0x4a) return seq(out, 0, '1');     /* Home */
    if (u == 0x49) return seq(out, 0, '2');     /* Insert */
    if (u == 0x4c) return seq(out, 0, '3');     /* Delete */
    if (u == 0x4d) return seq(out, 0, '4');     /* End */
    if (u == 0x4b) return seq(out, 0, '5');     /* Page Up */
    if (u == 0x4e) return seq(out, 0, '6');     /* Page Down */
    return 0;                                   /* F-keys, Print, ...: nothing */
}

/* 37.2: the compose table, X11's conventions (Compose key, then two keys).
 * Looked up in either order, so `" a` and `a "` are both ä; generated with
 * a check that no pair means two things in either order. Every Latin-1
 * letter, the euro, and the common Latin-1 symbols. */
typedef struct {
    char     a, b;
    uint16_t cp;
} compose_t;

static const compose_t k_compose[] = {
    { '"', 'a', 0x00e4 },
    { '"', 'e', 0x00eb },
    { '"', 'i', 0x00ef },
    { '"', 'o', 0x00f6 },
    { '"', 'u', 0x00fc },
    { '"', 'y', 0x00ff },
    { '"', 'A', 0x00c4 },
    { '"', 'E', 0x00cb },
    { '"', 'I', 0x00cf },
    { '"', 'O', 0x00d6 },
    { '"', 'U', 0x00dc },
    { '\'', 'a', 0x00e1 },
    { '\'', 'e', 0x00e9 },
    { '\'', 'i', 0x00ed },
    { '\'', 'o', 0x00f3 },
    { '\'', 'u', 0x00fa },
    { '\'', 'y', 0x00fd },
    { '\'', 'A', 0x00c1 },
    { '\'', 'E', 0x00c9 },
    { '\'', 'I', 0x00cd },
    { '\'', 'O', 0x00d3 },
    { '\'', 'U', 0x00da },
    { '\'', 'Y', 0x00dd },
    { '`', 'a', 0x00e0 },
    { '`', 'e', 0x00e8 },
    { '`', 'i', 0x00ec },
    { '`', 'o', 0x00f2 },
    { '`', 'u', 0x00f9 },
    { '`', 'A', 0x00c0 },
    { '`', 'E', 0x00c8 },
    { '`', 'I', 0x00cc },
    { '`', 'O', 0x00d2 },
    { '`', 'U', 0x00d9 },
    { '^', 'a', 0x00e2 },
    { '^', 'e', 0x00ea },
    { '^', 'i', 0x00ee },
    { '^', 'o', 0x00f4 },
    { '^', 'u', 0x00fb },
    { '^', 'A', 0x00c2 },
    { '^', 'E', 0x00ca },
    { '^', 'I', 0x00ce },
    { '^', 'O', 0x00d4 },
    { '^', 'U', 0x00db },
    { '~', 'a', 0x00e3 },
    { '~', 'n', 0x00f1 },
    { '~', 'o', 0x00f5 },
    { '~', 'A', 0x00c3 },
    { '~', 'N', 0x00d1 },
    { '~', 'O', 0x00d5 },
    { ',', 'c', 0x00e7 },
    { ',', 'C', 0x00c7 },
    { 'o', 'a', 0x00e5 },
    { 'o', 'A', 0x00c5 },
    { 'a', 'e', 0x00e6 },
    { 'A', 'E', 0x00c6 },
    { 's', 's', 0x00df },
    { 'o', '/', 0x00f8 },
    { 'O', '/', 0x00d8 },
    { 't', 'h', 0x00fe },
    { 'T', 'H', 0x00de },
    { 'd', '-', 0x00f0 },
    { 'D', '-', 0x00d0 },
    { '!', '!', 0x00a1 },
    { '?', '?', 0x00bf },
    { '<', '<', 0x00ab },
    { '>', '>', 0x00bb },
    { '=', 'e', 0x20ac },
    { '=', 'E', 0x20ac },
    { 'c', '/', 0x00a2 },
    { 'L', '-', 0x00a3 },
    { 'Y', '=', 0x00a5 },
    { 's', 'o', 0x00a7 },
    { 'o', 'o', 0x00b0 },
    { '+', '-', 0x00b1 },
    { '1', '2', 0x00bd },
    { '1', '4', 0x00bc },
    { '3', '4', 0x00be },
    { '^', '1', 0x00b9 },
    { '^', '2', 0x00b2 },
    { '^', '3', 0x00b3 },
    { 'm', 'u', 0x00b5 },
    { 'x', 'x', 0x00d7 },
    { ':', '-', 0x00f7 },
    { '^', '.', 0x00b7 },
    { 'o', 'c', 0x00a9 },
    { 'o', 'r', 0x00ae },
    { '-', 'a', 0x00aa },
    { '-', 'o', 0x00ba },
    { 'P', '!', 0x00b6 },
};

/* Code point `cp` as UTF-8 into out (at most 3 bytes: nothing here is above
 * U+FFFF). */
static uint32_t utf8_put(uint32_t cp, uint8_t *out) {
    if (cp < 0x80u) { out[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800u) {
        out[0] = (uint8_t)(0xc0u | (cp >> 6));
        out[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        return 2;
    }
    out[0] = (uint8_t)(0xe0u | (cp >> 12));
    out[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
    out[2] = (uint8_t)(0x80u | (cp & 0x3fu));
    return 3;
}

static uint32_t compose_lookup(char a, char b, uint8_t *out) {
    for (uint32_t i = 0; i < sizeof(k_compose) / sizeof(k_compose[0]); i++) {
        const compose_t *k = &k_compose[i];
        if ((k->a == a && k->b == b) || (k->a == b && k->b == a)) return utf8_put(k->cp, out);
    }
    return 0;
}

void usbkbd_compose_hint(const usbkbd_xlate_t *x, char *buf, uint32_t cap) {
    static const char word[] = "Compose";
    uint32_t n = 0;
    if (cap == 0) return;
    if (x->compose) {
        for (uint32_t i = 0; word[i] && n + 1 < cap; i++) buf[n++] = word[i];
        if (x->compose == 2 && n + 3 < cap) {
            buf[n++] = ' ';
            buf[n++] = (char)x->compose_first;
        }
    }
    buf[n] = '\0';
}

uint32_t usbkbd_translate(usbkbd_xlate_t *x, uint32_t ev, uint32_t now_ms, uint8_t *out) {
    uint32_t u = USBKBD_EV_USAGE(ev);
    bool make = (ev & USBKBD_EV_MAKE) != 0;
    x->mods = (uint8_t)USBKBD_EV_MODS(ev);
    if (u >= 0xE0 && u <= 0xE7) return 0;       /* modifiers: state only */
    if (!make) {
        if (u == x->repeat_usage) x->repeat_usage = 0;
        return 0;
    }
    /* the window manager's keys (kernel/wmkeys.h), before anything else sees them. */
    if (x->mods & MOD_GUI) {
        uint8_t m = KMOD_SUPER;
        if (x->mods & MOD_SHIFT) m |= KMOD_SHIFT;
        if (x->mods & MOD_CTRL) m |= KMOD_CTRL;
        if (x->mods & MOD_ALT) m |= KMOD_ALT;
        uint32_t key = wm_usage_key(u);
        unsigned hk = key ? wm_hotkey(key, m) : CONSOLE_HOTKEY_NONE;
        if (hk != CONSOLE_HOTKEY_NONE) {
            x->hotkey = (uint8_t)hk;
            x->repeat_usage = 0;
            return 0;
        }
    }
    if (u == 0x39) {                            /* Caps Lock: compose, or cancel it */
        x->compose = x->compose ? 0 : 1;
        x->repeat_usage = 0;
        return 0;
    }
    uint32_t n = usage_bytes(u, x->mods, out);
    if (x->compose) {
        x->repeat_usage = 0;
        if (n == 1 && out[0] >= 0x20u && out[0] < 0x7fu) {
            if (x->compose == 1) {
                x->compose_first = out[0];
                x->compose = 2;
                return 0;
            }
            x->compose = 0;
            return compose_lookup((char)x->compose_first, (char)out[0], out);
        }
        x->compose = 0;
        if (n == 1 && out[0] == 0x1bu) return 0;    /* Esc only cancels */
        /* anything else cancels and then does what it always does */
    }
    x->repeat_usage = n ? (uint8_t)u : 0;       /* the newest key repeats */
    x->repeat_at = now_ms + USBKBD_REPEAT_DELAY;
    return n;
}

uint32_t usbkbd_repeat(usbkbd_xlate_t *x, uint32_t now_ms, uint8_t *out) {
    if (!x->repeat_usage || (int32_t)(now_ms - x->repeat_at) < 0) return 0;
    x->repeat_at = now_ms + USBKBD_REPEAT_EVERY;
    return usage_bytes(x->repeat_usage, x->mods, out);
}

/* --- selftest (kernel only) --------------------------------------------- */

static bool bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

int usbkbd_selftest(void) {
    int fails = 0;
#define CHECK(name, cond) do { bool ok_ = (cond); cprintf("  %-50s %s\n", name, ok_ ? "ok" : "FAIL"); if (!ok_) fails++; } while (0)
    usb_cfg_info_t ci;
    /* The HID 1.11 spec's own keyboard example (Appendix E). */
    static const uint8_t kbd_cfg[34] = {
        0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0A,
    };
    bool ok = usb_parse_config(kbd_cfg, sizeof(kbd_cfg), &ci);
    CHECK("HID spec keyboard: iface 0, ep 1, 8 bytes, 10 ms",
          ok && ci.cfg_value == 1 && ci.kbd_found && ci.kbd_iface == 0 && ci.kbd_ep == 1 &&
          ci.kbd_mps == 8 && ci.kbd_interval == 10 && !ci.is_hub);
    /* A composite device: a non-boot HID interface first (consumer keys, a
     * mouse), the boot keyboard second -- the common gaming-keyboard shape. */
    static const uint8_t comp_cfg[59] = {
        0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x40, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x01,
        0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
        0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x04,
    };
    ok = usb_parse_config(comp_cfg, sizeof(comp_cfg), &ci);
    CHECK("composite: the boot keyboard is iface 1, ep 2",
          ok && ci.kbd_found && ci.kbd_iface == 1 && ci.kbd_ep == 2 && ci.kbd_interval == 4);
    /* A hub: class 9 with one status-change endpoint, no keyboard. */
    static const uint8_t hub_cfg[25] = {
        0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0xE0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x01, 0x00, 0xFF,
    };
    ok = usb_parse_config(hub_cfg, sizeof(hub_cfg), &ci);
    CHECK("hub: class 9, status endpoint 1, no keyboard",
          ok && ci.is_hub && ci.hub_ep == 1 && !ci.kbd_found);
    static const uint8_t bad_cfg[12] = { 0x09, 0x02, 0x0C, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32, 0x00, 0x04, 0x00 };
    ok = usb_parse_config(bad_cfg, sizeof(bad_cfg), &ci);
    CHECK("a zero bLength ends the walk instead of looping", ok && !ci.kbd_found);
    /* A boot mouse (HID 1.11 Appendix E's shape: protocol 2, 4-byte reports
     * with the wheel), and a wireless receiver with a keyboard on interface
     * 0 and a mouse on 1. */
    static const uint8_t mouse_cfg[34] = {
        0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x34, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x0A,
    };
    ok = usb_parse_config(mouse_cfg, sizeof(mouse_cfg), &ci);
    CHECK("boot mouse: iface 0, ep 1, 4 bytes, 10 ms, no keyboard",
          ok && ci.mouse_found && ci.mouse_iface == 0 && ci.mouse_ep == 1 && ci.mouse_mps == 4 &&
          ci.mouse_interval == 10 && !ci.kbd_found);
    static const uint8_t combo_cfg[59] = {
        0x09, 0x02, 0x3B, 0x00, 0x02, 0x01, 0x00, 0xA0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3F, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x08,
        0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x34, 0x00,
        0x07, 0x05, 0x82, 0x03, 0x05, 0x00, 0x02,
    };
    ok = usb_parse_config(combo_cfg, sizeof(combo_cfg), &ci);
    CHECK("receiver: keyboard iface 0 ep 1, mouse iface 1 ep 2 every 2 ms",
          ok && ci.kbd_found && ci.kbd_iface == 0 && ci.kbd_ep == 1 &&
          ci.mouse_found && ci.mouse_iface == 1 && ci.mouse_ep == 2 && ci.mouse_interval == 2);
    usbmouse_layout_t ml;
    usbmouse_ev_t mev;
    usbmouse_boot_layout(&ml);
    static const uint8_t mr3[3] = { 0x01, 0x05, 0xFD };              /* left, 5 right, 3 up */
    static const uint8_t mr4[4] = { 0x06, 0x80, 0x7F, 0xFF };        /* right + middle, extremes, wheel down */
    CHECK("boot report: left held, dx 5, dy -3, no wheel",
          usbmouse_decode(&ml, mr3, 3, &mev) && mev.buttons == 1 && mev.dx == 5 && mev.dy == -3 && mev.wheel == 0);
    CHECK("boot report: right + middle, dx -128, dy 127, wheel -1",
          usbmouse_decode(&ml, mr4, 4, &mev) && mev.buttons == 6 && mev.dx == -128 && mev.dy == 127 && mev.wheel == -1);
    CHECK("a short report is nothing", !usbmouse_decode(&ml, mr4, 2, &mev));
    /* A wheel mouse's report descriptor (the HID spec's mouse, with a
     * wheel): 3 buttons and 5 bits of padding, then X, Y and the wheel. */
    static const uint8_t rd_wheel[52] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
        0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
        0x81, 0x06, 0xC0, 0xC0,
    };
    CHECK("report descriptor: 3 buttons, X 8 at 8, Y 8 at 16, wheel 8 at 24, no ID",
          usbmouse_parse_report_desc(rd_wheel, sizeof rd_wheel, &ml) && ml.report_id == 0 && ml.buttons == 3 &&
          ml.btn_bit == 0 && ml.x_bit == 8 && ml.x_size == 8 && ml.y_bit == 16 && ml.wheel_bit == 24 && ml.wheel_size == 8);
    static const uint8_t rr_wheel[4] = { 0x02, 0x00, 0x00, 0x01 };    /* right held, wheel up */
    CHECK("its report: right held, the wheel one up",
          usbmouse_decode(&ml, rr_wheel, 4, &mev) && mev.buttons == 2 && mev.wheel == 1 && mev.dx == 0);
    /* A receiver's mouse: report ID 2, 16 buttons, 12-bit X and Y, the wheel. */
    static const uint8_t rd_id[60] = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02, 0x05, 0x01, 0x16, 0x01,
        0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06, 0x15, 0x81,
        0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06, 0xC0, 0xC0,
    };
    CHECK("report descriptor: ID 2, buttons, X 12 at 16, Y 12 at 28, wheel 8 at 40",
          usbmouse_parse_report_desc(rd_id, sizeof rd_id, &ml) && ml.report_id == 2 && ml.buttons == 8 &&
          ml.x_bit == 16 && ml.x_size == 12 && ml.y_bit == 28 && ml.y_size == 12 && ml.wheel_bit == 40);
    static const uint8_t rr_id[7] = { 0x02, 0x01, 0x00, 0x05, 0xD0, 0xFF, 0x01 };
    static const uint8_t rr_other[7] = { 0x01, 0x01, 0x00, 0x05, 0xD0, 0xFF, 0x01 };
    CHECK("its report: left, dx 5, dy -3 (12-bit), wheel up; another ID is not the mouse's",
          usbmouse_decode(&ml, rr_id, 7, &mev) && mev.buttons == 1 && mev.dx == 5 && mev.dy == -3 && mev.wheel == 1 &&
          !usbmouse_decode(&ml, rr_other, 7, &mev));
    CHECK("a keyboard's report descriptor has no mouse in it",
          !usbmouse_parse_report_desc(rd_wheel, 10, &ml));

    uint32_t ev[16];
    static const uint8_t r0[8]   = { 0 };
    static const uint8_t ra[8]   = { 0x00, 0, 0x04, 0, 0, 0, 0, 0 };
    static const uint8_t rAb[8]  = { 0x02, 0, 0x04, 0x05, 0, 0, 0, 0 };
    static const uint8_t rB[8]   = { 0x02, 0, 0x05, 0, 0, 0, 0, 0 };
    static const uint8_t rro[8]  = { 0x00, 0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
    int n = usbkbd_diff(r0, ra, ev, 16);
    CHECK("press a: one make 0x04", n == 1 && ev[0] == (0x04u | USBKBD_EV_MAKE));
    n = usbkbd_diff(ra, rAb, ev, 16);
    CHECK("shift + b: make LeftShift, then make 0x05, mods 0x02",
          n == 2 && ev[0] == (0xE1u | USBKBD_EV_MAKE | (0x02u << 16)) &&
          ev[1] == (0x05u | USBKBD_EV_MAKE | (0x02u << 16)));
    n = usbkbd_diff(rAb, rB, ev, 16);
    CHECK("release a: one break 0x04, shift still held", n == 1 && ev[0] == (0x04u | (0x02u << 16)));
    n = usbkbd_diff(rB, rro, ev, 16);
    CHECK("rollover report: ignored", n == -1);
    n = usbkbd_diff(rB, r0, ev, 16);
    CHECK("all released: break LeftShift, break 0x05",
          n == 2 && ev[0] == 0xE1u && ev[1] == 0x05u);

    /* Translation: the byte streams the line editor and `e` expect. */
    usbkbd_xlate_t x = { 0 };
    uint8_t o[USBKBD_SEQ_MAX];
    uint32_t m = usbkbd_translate(&x, 0x04u | USBKBD_EV_MAKE, 0, o);
    CHECK("a -> 'a'", m == 1 && o[0] == 'a');
    (void)usbkbd_translate(&x, 0x04u, 10, o);
    m = usbkbd_translate(&x, 0x1Fu | USBKBD_EV_MAKE | (0x02u << 16), 20, o);
    CHECK("Shift-2 -> '@'", m == 1 && o[0] == '@');
    m = usbkbd_translate(&x, 0x06u | USBKBD_EV_MAKE | (0x01u << 16), 30, o);
    CHECK("Ctrl-c -> 0x03 (the interrupt)", m == 1 && o[0] == 0x03);
    m = usbkbd_translate(&x, 0x28u | USBKBD_EV_MAKE, 40, o);
    CHECK("Enter -> CR", m == 1 && o[0] == '\r');
    m = usbkbd_translate(&x, 0x2Au | USBKBD_EV_MAKE, 50, o);
    CHECK("Backspace -> DEL (0x7F)", m == 1 && o[0] == 0x7f);
    m = usbkbd_translate(&x, 0x52u | USBKBD_EV_MAKE, 60, o);
    CHECK("Up -> ESC [ A", m == 3 && o[0] == 0x1b && o[1] == '[' && o[2] == 'A');
    m = usbkbd_translate(&x, 0x4Au | USBKBD_EV_MAKE, 70, o);
    CHECK("Home -> ESC [ 1 ~", m == 4 && o[2] == '1' && o[3] == '~');
    m = usbkbd_translate(&x, 0x4Cu | USBKBD_EV_MAKE, 80, o);
    CHECK("Delete -> ESC [ 3 ~", m == 4 && o[2] == '3' && o[3] == '~');
    m = usbkbd_translate(&x, 0x3Au | USBKBD_EV_MAKE, 130, o);
    CHECK("F1 -> nothing", m == 0);
    /* Typematic: 'x' held from t=1000 repeats at 1500, then every 33 ms,
     * and stops at its release. */
    usbkbd_xlate_t r = { 0 };
    (void)usbkbd_translate(&r, 0x1Bu | USBKBD_EV_MAKE, 1000, o);
    bool early = usbkbd_repeat(&r, 1499, o) == 0;
    bool first = usbkbd_repeat(&r, 1500, o) == 1 && o[0] == 'x';
    bool gap   = usbkbd_repeat(&r, 1510, o) == 0;
    bool next  = usbkbd_repeat(&r, 1533, o) == 1;
    (void)usbkbd_translate(&r, 0x1Bu, 1540, o);
    bool stops = usbkbd_repeat(&r, 2000, o) == 0;
    CHECK("typematic: 500 ms delay, then every 33 ms, until release", early && first && gap && next && stops);

    /* 37.2: Caps Lock is the compose key. Usages: Caps 0x39, ' and " 0x34
     * (" with Shift), a 0x04, e 0x08, q 0x14, = 0x2E, Esc 0x29, Enter 0x28. */
#define KEY(u, mods) usbkbd_translate(&c, (u) | USBKBD_EV_MAKE | ((uint32_t)(mods) << 16), t += 10, o)
    usbkbd_xlate_t c = { 0 };
    uint32_t t = 2000;
    char hint[16];
    (void)KEY(0x39, 0);
    usbkbd_compose_hint(&c, hint, sizeof(hint));
    bool h1 = hint[0] == 'C' && hint[7] == '\0';
    uint32_t k1 = KEY(0x34, 0x02);
    usbkbd_compose_hint(&c, hint, sizeof(hint));
    bool h2 = k1 == 0 && hint[7] == ' ' && hint[8] == '"' && hint[9] == '\0';
    m = KEY(0x04, 0);
    usbkbd_compose_hint(&c, hint, sizeof(hint));
    CHECK("compose: Caps \" a -> a-umlaut (C3 A4), with the hint shown then gone",
          h1 && h2 && m == 2 && o[0] == 0xc3 && o[1] == 0xa4 && hint[0] == '\0' && c.repeat_usage == 0);
    (void)KEY(0x39, 0); (void)KEY(0x04, 0x02); m = KEY(0x34, 0x02);
    CHECK("compose: either order -- Caps A \" -> A-umlaut (C3 84)", m == 2 && o[0] == 0xc3 && o[1] == 0x84);
    (void)KEY(0x39, 0); (void)KEY(0x2E, 0); m = KEY(0x08, 0);
    CHECK("compose: Caps = e -> euro (E2 82 AC)", m == 3 && o[0] == 0xe2 && o[1] == 0x82 && o[2] == 0xac);
    (void)KEY(0x39, 0); (void)KEY(0x14, 0); m = KEY(0x14, 0);
    uint32_t after = KEY(0x04, 0);
    CHECK("compose: an unknown pair gives nothing, and the next key is plain",
          m == 0 && c.compose == 0 && after == 1 && o[0] == 'a');
    (void)KEY(0x39, 0); m = KEY(0x29, 0);
    uint32_t m3 = KEY(0x39, 0), m4 = KEY(0x39, 0), m5 = KEY(0x04, 0);
    CHECK("compose: Esc cancels silently; Caps twice cancels too",
          m == 0 && m3 == 0 && m4 == 0 && c.compose == 0 && m5 == 1 && o[0] == 'a');
    (void)KEY(0x39, 0); (void)KEY(0x34, 0x02); m = KEY(0x28, 0);
    CHECK("compose: Enter cancels and is still Enter", m == 1 && o[0] == '\r' && c.compose == 0);

    /* 37.5a: the modifier encodings kernel/keyseq.c reads. HID modifier
     * bits: LCtrl 0x01, LShift 0x02, LAlt 0x04, LGUI 0x08. */
#define SEQ_IS(n_, str_) ((n_) == sizeof(str_) - 1u && bytes_eq(o, (const uint8_t *)(str_), (n_)))
    usbkbd_xlate_t k = { 0 };
    c = k;
    m = KEY(0x1a, 0x04);                        /* Alt-w */
    CHECK("Alt-w -> ESC w", SEQ_IS(m, "\x1b" "w"));
    m = KEY(0x50, 0x02);                        /* Shift-Left */
    CHECK("Shift-Left -> ESC [ 1 ; 2 D", SEQ_IS(m, "\x1b[1;2D"));
    m = KEY(0x4f, 0x01);                        /* Ctrl-Right */
    CHECK("Ctrl-Right -> ESC [ 1 ; 5 C", SEQ_IS(m, "\x1b[1;5C"));
    m = KEY(0x4c, 0x02);                        /* Shift-Delete */
    CHECK("Shift-Delete -> ESC [ 3 ; 2 ~", SEQ_IS(m, "\x1b[3;2~"));
    m = KEY(0x06, 0x08);                        /* Super-c */
    CHECK("Super-c -> CSI-u ESC [ 99 ; 9 u", SEQ_IS(m, "\x1b[99;9u"));
    m = KEY(0x1d, 0x0a);                        /* Super-Shift-z */
    CHECK("Super-Shift-z -> ESC [ 122 ; 10 u", SEQ_IS(m, "\x1b[122;10u"));
    m = KEY(0x2c, 0x01);                        /* Ctrl-Space */
    CHECK("Ctrl-Space -> NUL", m == 1 && o[0] == 0);
    m = KEY(0x2f, 0x08);
    uint8_t hk1 = c.hotkey; c.hotkey = 0;
    uint32_t m2 = KEY(0x30, 0x08);
    uint8_t hk2 = c.hotkey; c.hotkey = 0;
    uint32_t m6 = KEY(0x20, 0x0a);              /* Super-Shift-3 */
    uint8_t hk6 = c.hotkey; c.hotkey = 0;
    (void)KEY(0x28, 0x08);                      /* Super-Enter */
    uint8_t hk_ent = c.hotkey; c.hotkey = 0;
    (void)KEY(0x50, 0x08);                      /* Super-Left */
    uint8_t hk_l = c.hotkey; c.hotkey = 0;
    (void)KEY(0x4f, 0x08);                      /* Super-Right */
    uint8_t hk_r = c.hotkey; c.hotkey = 0;
    (void)KEY(0x50, 0x09);                      /* Super-Ctrl-Left */
    uint8_t hk_cl = c.hotkey; c.hotkey = 0;
    (void)KEY(0x4f, 0x09);                      /* Super-Ctrl-Right */
    uint8_t hk_cr = c.hotkey; c.hotkey = 0;
    (void)KEY(0x1a, 0x08);                      /* Super-w */
    uint8_t hk_w = c.hotkey; c.hotkey = 0;
    (void)KEY(0x12, 0x08);                      /* Super-o */
    uint8_t hk_o = c.hotkey; c.hotkey = 0;
    (void)KEY(0x1e, 0x08);                      /* Super-1 */
    uint8_t hk_1 = c.hotkey; c.hotkey = 0;
    (void)KEY(0x09, 0x08);                      /* Super-f */
    uint8_t hk_f = c.hotkey; c.hotkey = 0;
    (void)KEY(0x09, 0x0a);                      /* Super-Shift-f */
    uint8_t hk_sf = c.hotkey; c.hotkey = 0;
    (void)KEY(0x15, 0x08);                      /* Super-r */
    uint8_t hk_rr = c.hotkey; c.hotkey = 0;
    (void)KEY(0x2d, 0x08);                      /* Super-- */
    uint8_t hk_min = c.hotkey; c.hotkey = 0;
    (void)KEY(0x2e, 0x0a);                      /* Super-Shift-= */
    uint8_t hk_hup = c.hotkey; c.hotkey = 0;
    (void)KEY(0x52, 0x08);                      /* Super-Up */
    uint8_t hk_up = c.hotkey; c.hotkey = 0;
    uint32_t m_bs = KEY(0x31, 0x08);            /* Super-\\: a program's now */
    uint8_t hk_bs = c.hotkey; c.hotkey = 0;
    CHECK("Super window hotkeys: Enter/arrows/Ctrl-arrows/w/o/1/screenshot/[/]/f/F/r/-/+",
          m == 0 && m2 == 0 && m6 == 0 && hk1 == CONSOLE_HOTKEY_JOIN_LEFT &&
          hk2 == CONSOLE_HOTKEY_JOIN_RIGHT && hk6 == CONSOLE_HOTKEY_SCREENSHOT &&
          hk_ent == CONSOLE_HOTKEY_NEW_TERM && hk_l == CONSOLE_HOTKEY_FOCUS_LEFT &&
          hk_r == CONSOLE_HOTKEY_FOCUS_RIGHT && hk_cl == CONSOLE_HOTKEY_MOVE_LEFT &&
          hk_cr == CONSOLE_HOTKEY_MOVE_RIGHT && hk_w == CONSOLE_HOTKEY_CLOSE &&
          hk_o == CONSOLE_HOTKEY_ORIENT && hk_1 == CONSOLE_HOTKEY_JUMP(1) &&
          hk_f == CONSOLE_HOTKEY_TOGGLE_MAX && hk_sf == CONSOLE_HOTKEY_FULLSCREEN &&
          hk_rr == CONSOLE_HOTKEY_WIDTH_NEXT && hk_min == CONSOLE_HOTKEY_WIDTH_DOWN &&
          hk_hup == CONSOLE_HOTKEY_HEIGHT_UP && hk_up == CONSOLE_HOTKEY_FOCUS_UP &&
          m_bs > 0 && hk_bs == CONSOLE_HOTKEY_NONE);
#undef SEQ_IS
#undef KEY
#undef CHECK
    cprintf("%s\n", fails ? "USBKBD_SELFTEST_FAIL" : "USBKBD_SELFTEST_OK");
    return fails;
}
