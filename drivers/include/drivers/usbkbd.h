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

/* --- Key events to terminal bytes (36.9): kernel side, not U-mode ------------
 *
 * US layout only (plan §4.4): Shift and Ctrl change a byte. Since 37.5a
 * (plan/phase37_screen_layouts_and_apps.md) the other modifiers send what a
 * modern terminal sends, so that kernel/keyseq.c reads the panel's keyboard
 * and a host's terminal alike: Alt is an ESC prefix; Shift, Alt, Ctrl or
 * Super with an arrow, Home, End or a ~ key is xterm's ESC [ 1 ; m X or
 * ESC [ n ; m ~ (m = 1 + Shift 1 + Alt 2 + Ctrl 4 + Super 8); Super with
 * anything else is CSI-u, ESC [ cp ; m u with the unshifted key's code
 * point; Ctrl-Space is NUL. Super+[ , Super+] , Super+\\ and Super+Shift+3
 * are the screen's (USBKBD_HOTKEY_*) and send nothing. **Caps Lock is the compose key** (37.2,
 * plan/phase37_screen_layouts_and_apps.md §3.2): Caps, then two keys, gives
 * one character as UTF-8 -- `Caps " a` is ä, `Caps = e` is € -- by X11's
 * compose conventions, in either order. An unknown pair gives nothing; Esc
 * or Caps again cancels; any other key (Enter, an arrow) cancels and does
 * what it always does. Nothing in a compose sequence repeats. Enter is \r, Backspace 0x7F, Tab \t, Esc 0x1B; Ctrl-letter
 * is 0x01-0x1A; arrows ESC[A-D, Home ESC[1~, Insert ESC[2~, Delete ESC[3~,
 * End ESC[4~, PgUp ESC[5~, PgDn ESC[6~; the keypad as if Num Lock were on.
 *
 * Typematic is the host's job: SET_IDLE(0) makes the keyboard report only on
 * change. The last non-modifier key pressed repeats after USBKBD_REPEAT_DELAY
 * ms, every USBKBD_REPEAT_EVERY ms, until it (or anything else) changes. */
#define USBKBD_REPEAT_DELAY 500u
#define USBKBD_REPEAT_EVERY  33u
#define USBKBD_SEQ_MAX       10u    /* the longest sequence: CSI-u, ESC [ 127 ; 16 u */

typedef struct {
    uint8_t  mods;          /* current modifier byte */
    uint8_t  compose;       /* 37.2: 0 off, 1 Caps pressed, 2 one key in */
    uint8_t  repeat_usage;  /* 0: nothing repeating */
    uint8_t  compose_first; /* the first key's character, in state 2 */
    uint32_t repeat_at;     /* ms: when it next repeats */
    uint8_t  hotkey;        /* 37.5a: USBKBD_HOTKEY_*, for the caller to take */
    uint8_t  _pad[3];
} usbkbd_xlate_t;

/* 37.5a: keys the screen takes before any program sees them -- they
 * produce no bytes, only `hotkey`, which the caller hands to the console. */
#define USBKBD_HOTKEY_NONE       0u
#define USBKBD_HOTKEY_LEFT       1u     /* Super+[ : the split's divider one step left */
#define USBKBD_HOTKEY_RIGHT      2u     /* Super+] : one step right */
#define USBKBD_HOTKEY_SCREENSHOT 3u     /* Super+Shift+3, as on the Mac */
#define USBKBD_HOTKEY_SWAP       4u     /* Super+\\ : canvas and text change sides */
/* The same numbers as kernel/console.h's CONSOLE_HOTKEY_*, which the
 * keyboard source hands them to unchanged. */

/* The bytes for one event at time `now_ms` (for typematic); returns how many
 * were written to out[USBKBD_SEQ_MAX], 0 for keys that produce nothing. */
uint32_t usbkbd_translate(usbkbd_xlate_t *x, uint32_t ev, uint32_t now_ms, uint8_t *out);

/* A repeat that is due at `now_ms`: its bytes, else 0. */
uint32_t usbkbd_repeat(usbkbd_xlate_t *x, uint32_t now_ms, uint8_t *out);

/* 37.2: what the screen shows while a compose sequence is open --
 * `Compose`, then `Compose "` once the first key is in -- or "" when none
 * is. Writes a NUL-terminated string of at most `cap` bytes. */
void usbkbd_compose_hint(const usbkbd_xlate_t *x, char *buf, uint32_t cap);

/* `usbkbdselftest`. Prints USBKBD_SELFTEST_OK/_FAIL; returns failures. */
int usbkbd_selftest(void);

#endif /* LUGALOS_DRIVERS_USBKBD_H */
