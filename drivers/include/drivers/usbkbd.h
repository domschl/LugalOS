#ifndef LUGALOS_DRIVERS_USBKBD_H
#define LUGALOS_DRIVERS_USBKBD_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/kbd_attr.h"

/* A USB boot keyboard, above the transaction interface -- 36.8,
 * plan/phase36_rp2350_lcd7_terminal.md §3.2.
 *
 * Portable: nothing here touches hardware. drivers/usbkbd_rp2350.c's `kbd`
 * task feeds it descriptors and reports from the PIO-USB engine; QEMU feeds
 * it the same bytes in `usbkbdselftest`. */

/* What a configuration descriptor says, as far as this host cares. */
typedef struct {
    uint8_t cfg_value;      /* for SET_CONFIGURATION */
    uint8_t kbd_found;      /* an interface of class 3/1/1 (HID boot keyboard) */
    uint8_t kbd_iface;
    uint8_t kbd_ep;         /* its interrupt IN endpoint number */
    uint8_t kbd_mps;
    uint8_t kbd_interval;   /* bInterval: ms at full speed */
    uint8_t is_hub;         /* an interface of class 9 */
    uint8_t hub_ep;         /* the hub's status-change interrupt IN endpoint */
} usb_cfg_info_t;

/* Walks `len` bytes of configuration descriptor. False if it is malformed
 * before the first interface. */
bool usb_parse_config(const uint8_t *cfg, uint32_t len, usb_cfg_info_t *out);

/* Key events, one per make or break:
 *   bits 0-7   HID usage (0x04 'a' ... ; 0xE0-0xE7 the modifiers)
 *   bit 8      1 = make (pressed), 0 = break (released)
 *   bits 16-23 the modifier byte after this report */
#define USBKBD_EV_MAKE      (1u << 8)
#define USBKBD_EV_USAGE(e)  ((e) & 0xffu)
#define USBKBD_EV_MODS(e)   (((e) >> 16) & 0xffu)

/* The events between two 8-byte boot reports, modifiers first. Returns how
 * many were written (at most `max`), or -1 for a phantom/rollover report
 * (keys all 0x01), which the caller must ignore -- keeping `prev` as it was. */
int usbkbd_diff(const uint8_t prev[8], const uint8_t cur[8], uint32_t *ev, uint32_t max);

/* `usbkbdselftest`. Prints USBKBD_SELFTEST_OK/_FAIL; returns failures. */
int usbkbd_selftest(void);

#endif /* LUGALOS_DRIVERS_USBKBD_H */
