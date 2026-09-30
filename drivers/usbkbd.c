/* USB boot keyboard, portable half -- 36.8, plan/phase36_rp2350_lcd7_terminal.md.
 * See drivers/include/drivers/usbkbd.h.
 *
 * Everything above the selftest runs in the U-mode `kbd` task on the
 * RP2350-LCD-7 (KBD_UTEXT): no libc, no switch (jump tables are .rodata
 * outside the task's domain; the file is also built -fno-jump-tables), no
 * string literals. */

#include "drivers/usbkbd.h"
#include "kernel/console.h"

KBD_UTEXT bool usb_parse_config(const uint8_t *cfg, uint32_t len, usb_cfg_info_t *out) {
    uint8_t *o = (uint8_t *)out;
    for (uint32_t i = 0; i < sizeof(*out); i++) o[i] = 0;
    if (len < 9 || cfg[1] != 2) return false;
    out->cfg_value = cfg[5];
    uint32_t cls = 0, sub = 0, proto = 0, alt = 0, iface = 0;
    uint32_t at = 0;
    while (at + 2 <= len) {
        uint32_t bl = cfg[at], type = cfg[at + 1];
        if (bl < 2 || at + bl > len) break;
        if (type == 4 && bl >= 9) {                         /* interface */
            iface = cfg[at + 2]; alt = cfg[at + 3];
            cls = cfg[at + 5]; sub = cfg[at + 6]; proto = cfg[at + 7];
            if (cls == 9) out->is_hub = 1;
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
            if (int_in && cls == 9 && !out->hub_ep) out->hub_ep = (uint8_t)(ea & 0x0fu);
        }
        at += bl;
    }
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

static uint32_t seq(uint8_t *out, char final, char digit) {
    out[0] = 0x1b; out[1] = '[';
    if (!digit) { out[2] = (uint8_t)final; return 3; }
    out[2] = (uint8_t)digit; out[3] = '~';
    return 4;
}

/* The bytes one usage produces with modifiers `mods`; 0 for none. */
static uint32_t usage_bytes(const usbkbd_xlate_t *x, uint32_t u, uint32_t mods, uint8_t *out) {
    bool shift = (mods & MOD_SHIFT) != 0, ctrl = (mods & MOD_CTRL) != 0;
    if (u >= 0x04 && u <= 0x38) {
        char c = shift ? k_us_shift[u - 0x04] : k_us_plain[u - 0x04];
        if (!c) return 0;
        if (u <= 0x1d && x->caps) c = shift ? k_us_plain[u - 0x04] : k_us_shift[u - 0x04];
        if (ctrl) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
            else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
            else if (c == '[') c = 0x1b;
            else if (c == '\\') c = 0x1c;
            else if (c == ']') c = 0x1d;
            else if (c != '\r' && c != 0x7f && c != '\t' && c != 0x1b) return 0;
        }
        out[0] = (uint8_t)c;
        return 1;
    }
    if (u >= 0x54 && u <= 0x63) { out[0] = (uint8_t)k_keypad[u - 0x54]; return 1; }
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

uint32_t usbkbd_translate(usbkbd_xlate_t *x, uint32_t ev, uint32_t now_ms, uint8_t *out) {
    uint32_t u = USBKBD_EV_USAGE(ev);
    bool make = (ev & USBKBD_EV_MAKE) != 0;
    x->mods = (uint8_t)USBKBD_EV_MODS(ev);
    if (u >= 0xE0 && u <= 0xE7) return 0;       /* modifiers: state only */
    if (!make) {
        if (u == x->repeat_usage) x->repeat_usage = 0;
        return 0;
    }
    if (u == 0x39) { x->caps = (uint8_t)!x->caps; return 0; }   /* Caps Lock */
    uint32_t n = usage_bytes(x, u, x->mods, out);
    x->repeat_usage = n ? (uint8_t)u : 0;       /* the newest key repeats */
    x->repeat_at = now_ms + USBKBD_REPEAT_DELAY;
    return n;
}

uint32_t usbkbd_repeat(usbkbd_xlate_t *x, uint32_t now_ms, uint8_t *out) {
    if (!x->repeat_usage || (int32_t)(now_ms - x->repeat_at) < 0) return 0;
    x->repeat_at = now_ms + USBKBD_REPEAT_EVERY;
    return usage_bytes(x, x->repeat_usage, x->mods, out);
}

/* --- selftest (kernel only) --------------------------------------------- */

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
    (void)usbkbd_translate(&x, 0x39u | USBKBD_EV_MAKE, 90, o);     /* Caps on */
    m = usbkbd_translate(&x, 0x05u | USBKBD_EV_MAKE, 100, o);
    uint32_t m2 = usbkbd_translate(&x, 0x1Eu | USBKBD_EV_MAKE, 110, o + 1);
    CHECK("Caps Lock: b -> 'B', 1 stays '1'", m == 1 && o[0] == 'B' && m2 == 1 && o[1] == '1');
    (void)usbkbd_translate(&x, 0x39u | USBKBD_EV_MAKE, 120, o);    /* Caps off */
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
#undef CHECK
    cprintf("%s\n", fails ? "USBKBD_SELFTEST_FAIL" : "USBKBD_SELFTEST_OK");
    return fails;
}
