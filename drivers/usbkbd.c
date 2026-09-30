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
#undef CHECK
    cprintf("%s\n", fails ? "USBKBD_SELFTEST_FAIL" : "USBKBD_SELFTEST_OK");
    return fails;
}
