/* A GT911 touch controller, polled, and the ribbon's three touch gestures --
 * 47.12, . The interface and what the
 * gestures do are drivers/include/drivers/touch.h.
 *
 * The controller (Goodix GT911; register map as Waveshare's
 * libraries/bsp/bsp_gt911.c in RP2350-Touch-LCD-7-Demo uses it):
 *   0x8140  product ID, "911"
 *   0x8048  X and Y output maximum, 16-bit little endian each
 *   0x814E  status: bit 7 a new report is ready, bits 3:0 the points in it;
 *           written 0 to take the report
 *   0x814F  point 1: track id, X (LE), Y (LE), size, 8 bytes per point
 * 16-bit register addresses, big endian, at I2C address 0x5D.
 *
 * Polled, not interrupt-driven: the LCD-7B routes INT only to a test point,
 * and one task reading a status byte every 20 ms is cheap (~0.5 ms of a
 * 100 kHz bus). Both boards already answer at 0x5D after power-up, so the
 * reset/address-select dance with INT is not needed either.
 *
 * Gestures come from the first point only, classified by drivers/gesture.c
 * in millimetres at the panel's CONFIG_PANEL_DPI: a tap, a long press, or a
 * swipe in one of four directions; anything else is nothing. */

#include "drivers/touch.h"
#include "drivers/gesture.h"
#include "kernel/event.h"
#include "drivers/lcdterm.h"
#include "drivers/screen.h"
#include "drivers/i2c_bus.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(TOUCH_PRESENT)

#define GT911_ADDR        0x5Du
#define GT911_ADDR_ALT    0x14u
#define GT911_PRODUCT_ID  0x8140u
#define GT911_RES         0x8048u
#define GT911_STATUS      0x814Eu
#define GT911_POINT1      0x814Fu

#define ST7123_ADDR       0x55u
#define ST7123_FW_VER     0x0000u
#define ST7123_MAX_X_H    0x0005u
#define ST7123_STATUS     0x0010u
#define ST7123_REPORT_0   0x0014u

#define POLL_MS   20u
#if !defined(CONFIG_PANEL_DPI)
#define CONFIG_PANEL_DPI 133
#endif

typedef enum {
    CHIP_NONE = 0,
    CHIP_GT911,
    CHIP_ST7123,
} touch_chip_type_t;

typedef struct {
    bool              found;
    touch_chip_type_t chip;
    uint8_t           addr;
    const char       *chip_name;
    uint16_t          res_x, res_y;          /* the controller's coordinate space */
    unsigned          scr_w, scr_h;          /* the screen's, when last mapped */
    uint32_t          polls, reports, presses, taps, swipes, longs, drags, nothings, errors;
    uint32_t          ends_lifted, ends_quiet;   /* how contacts ended: a lift report, or reports stopping */
    bool              edge;                      /* this contact began in the edge band */
    win_t             owner;                     /* the window it is posted to raw, or -1 (gestures) */
    int               owner_x0, owner_y0;        /* that window's content origin when it began */
    int               last_px, last_py;          /* the last point posted to it */
    bool              pressed;                   /* press feedback shown for this contact */
    uint64_t          repeat_ms;                 /* a held press's next repeat (0: none) */
    bool              capture;                   /* a control follows this contact: no gestures */
    uint32_t          edge_contacts;
    uint32_t          st_empty;                  /* ST7123 reports with coordinates but no valid point */
    uint32_t          st_empty_down;             /* ... of them while a contact was down */
    uint32_t          st_empty_mid;              /* ... followed by a point again in the same contact */
    uint32_t          gap_max_ms;                /* longest gap between two reports of one contact */
    gesture_state_t   gs;                    /* the first point's contact */
    int               last_x, last_y;        /* mapped, the last point seen */
    volatile uint32_t log_seq;               /* bumped per event, for `ts log` */
    volatile int      log_kind;              /* 1 press, 2 move, 3 release */
} touch_t;

/* single task: the touch task; touch_start() fills it at boot before creating
 * it, and `ts` only reads. */
static touch_t g_t;

static bool reg_read(uint8_t addr, uint16_t reg, uint8_t *buf, uint32_t n) {
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_xfer(addr, a, 2, buf, n);
}

static bool reg_write8(uint8_t addr, uint16_t reg, uint8_t v) {
    uint8_t a[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, v };
    return i2c_xfer(addr, a, 3, 0, 0);
}

#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))
#include "drivers/dsi_esp32p4.h"
#define HAS_DSI_ORIENT 1
#endif

/* The controller's point in screen pixels. Two board facts: how the touch
 * layer sits on the panel (CONFIG_TOUCH_MIRROR_X/_Y -- the LCD-7B's is
 * turned, Waveshare's own code mirrors both axes there), and whether the
 * board turns its picture (CONFIG_LCD_ROTATE_180, the RP2350 terminal), which
 * turns the touch with it. */
static void map_point(unsigned rx, unsigned ry, int *x, int *y) {
    const uint8_t *fb;
    unsigned w = 0, h = 0, stride;
    if (!console_pixels(&fb, &w, &h, &stride) || !w || !h) { w = g_t.res_x; h = g_t.res_y; }
    g_t.scr_w = w;
    g_t.scr_h = h;

    unsigned px = 0, py = 0;
#if defined(HAS_DSI_ORIENT)
    int rot = dsi_lcd_get_orientation();
    if (rot == 270) {
        px = g_t.res_y ? (g_t.res_y - 1u - ry) * w / g_t.res_y : (g_t.res_y - 1u - ry);
        py = g_t.res_x ? rx * h / g_t.res_x : rx;
    } else if (rot == 90) {
        px = g_t.res_y ? ry * w / g_t.res_y : ry;
        py = g_t.res_x ? (g_t.res_x - 1u - rx) * h / g_t.res_x : (g_t.res_x - 1u - rx);
    } else if (rot == 180) {
        px = g_t.res_x ? (g_t.res_x - 1u - rx) * w / g_t.res_x : (g_t.res_x - 1u - rx);
        py = g_t.res_y ? (g_t.res_y - 1u - ry) * h / g_t.res_y : (g_t.res_y - 1u - ry);
    } else {
        px = g_t.res_x ? rx * w / g_t.res_x : rx;
        py = g_t.res_y ? ry * h / g_t.res_y : ry;
    }
#else
    if (w > h && g_t.res_x < g_t.res_y) {
        px = g_t.res_y ? (g_t.res_y - 1u - ry) * w / g_t.res_y : (g_t.res_y - 1u - ry);
        py = g_t.res_x ? rx * h / g_t.res_x : rx;
    } else {
        px = g_t.res_x ? rx * w / g_t.res_x : rx;
        py = g_t.res_y ? ry * h / g_t.res_y : ry;
    }
#endif
    if (px >= w) px = w - 1;
    if (py >= h) py = h - 1;
#if defined(CONFIG_TOUCH_MIRROR_X)
    px = w - 1 - px;
#endif
#if defined(CONFIG_TOUCH_MIRROR_Y)
    py = h - 1 - py;
#endif
#if defined(CONFIG_LCD_ROTATE_180)
    px = w - 1 - px;
    py = h - 1 - py;
#endif
    *x = (int)px;
    *y = (int)py;
}

/* What a gesture does: a horizontal swipe moves the ribbon (the finger
 * dragging it: moving left brings the window on the right into view, as
 * Cmd+Right does), a vertical drag scrolls with the finger and may end in a
 * fling, a tap taps, a long press opens the context menu (a mouse's right
 * click) and starts a held key's repeat. */
static void gesture_act(gesture_kind_t kind, const gesture_t *ev) {
    switch (kind) {
    case GESTURE_SWIPE_LEFT:  g_t.swipes++; console_hotkey(CONSOLE_HOTKEY_FOCUS_RIGHT); break;
    case GESTURE_SWIPE_RIGHT: g_t.swipes++; console_hotkey(CONSOLE_HOTKEY_FOCUS_LEFT); break;
    case GESTURE_SWIPE_UP:
    case GESTURE_SWIPE_DOWN:  g_t.swipes++; console_touch_scroll(ev->dy, ev->ms); break;
    case GESTURE_TAP:         g_t.taps++; console_touch_tap((unsigned)ev->x, (unsigned)ev->y); break;
    case GESTURE_DRAG:        console_touch_drag(ev->dy); break;
    case GESTURE_DRAG_END:
        g_t.drags++;
        console_touch_drag_end(g_t.gs.y - g_t.gs.y0, ev->dy, ev->ms);
        break;
    case GESTURE_LONG_PRESS:
        g_t.longs++;
        if (g_t.pressed) g_t.repeat_ms = time_get_ms();        /* held: repeat from now */
        console_touch_context((unsigned)ev->x, (unsigned)ev->y);
        break;
    default:                  g_t.nothings++; break;
    }
}

/* A window that claimed the pointer (WIN_CLAIM_POINTER: a full-screen game)
 * gets the contacts that begin while it has the focus, raw, and no gestures
 * run for them -- except one that begins within this many millimetres of the
 * left or right edge of the screen: that one is the window manager's, so a
 * swipe in from the edge leaves the game for the ribbon. */
#define TOUCH_EDGE_MM 3u

#define TOUCH_REPEAT_MS 80u     /* a held key's repeat interval, after the long press */

static bool owned(void) {
    return g_t.owner >= 0;
}

/* A point of an owned contact to its window, in content coordinates. */
static void owner_post(int kind, int x, int y) {
    win_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = kind == 1 ? WIN_EV_POINTER_DOWN : kind == 2 ? WIN_EV_POINTER_MOVE : WIN_EV_POINTER_UP;
    ev.button = 1;
    ev.x = (int16_t)(x - g_t.owner_x0);
    ev.y = (int16_t)(y - g_t.owner_y0);
    ev.dx = (int16_t)(kind == 1 ? 0 : x - g_t.last_px);
    ev.dy = (int16_t)(kind == 1 ? 0 : y - g_t.last_py);
    g_t.last_px = x;
    g_t.last_py = y;
    window_post(g_t.owner, &ev);
}

/* The contact ended, last seen at last_ms: to the window that owns it, or
 * the gesture (timed to the last report, not to when the end was
 * noticed, so a tap's and a swipe's duration do not include the wait). */
static void contact_end(int x, int y, uint64_t last_ms) {
    g_t.log_kind = 3;
    g_t.log_seq++;
    gesture_t ev;
    if (g_t.capture) {                                   /* the control had it: its last point, no gesture */
        g_t.capture = false;
        console_touch_move((unsigned)x, (unsigned)y);
        g_t.pressed = false;
        console_touch_release();
        (void)gesture_contact(&g_t.gs, false, x, y, (uint32_t)last_ms, &ev);
        return;
    }
    if (g_t.pressed) {                                   /* before the tap, which acts on it */
        g_t.pressed = false;
        console_touch_release();
    }
    gesture_kind_t kind = gesture_contact(&g_t.gs, false, x, y, (uint32_t)last_ms, &ev);
    if (owned()) {                                       /* the window's, not the window manager's */
        owner_post(3, x, y);
        g_t.owner = -1;
        return;
    }
    if (kind != GESTURE_NONE || !g_t.gs.long_sent) gesture_act(kind, &ev);
}

static void touch_task_body(void *arg) {
    (void)arg;
    bool down = false;
    bool empty_seen = false;      /* an ST7123 empty report since this contact's last point */
    int x = 0, y = 0;
    uint64_t t_last = 0;
    gesture_init(&g_t.gs, CONFIG_PANEL_DPI);
    g_t.owner = -1;
    for (;;) {
        task_sleep_ms(POLL_MS);
        g_t.polls++;

        bool got = false;
        bool lifted = false;      /* the controller said the finger is gone */
        bool st_empty_now = false;
        unsigned rx = 0, ry = 0;

        if (g_t.chip == CHIP_ST7123) {
            uint8_t adv = 0;
            if (!reg_read(g_t.addr, ST7123_STATUS, &adv, 1)) {
                g_t.errors++;
                continue;
            }
            if (adv & 0x08u) { /* with_coord */
                uint8_t p[7];
                if (reg_read(g_t.addr, ST7123_REPORT_0, p, 7)) {
                    if (p[0] & 0x80u) {
                        rx = ((unsigned)(p[0] & 0x3Fu) << 8) | p[1];
                        ry = ((unsigned)(p[2] & 0x0Fu) << 8) | p[3];
                        got = true;
                    } else {
                        g_t.st_empty++;        /* counted to see whether it marks a lift */
                        st_empty_now = true;
                    }
                }
                /* ST7121/ST7123 TDDI requires reading through the end of the coordinate table
                 * (last register 0x0059 for 10 touches) to clear INT and advance coordinate FIFO. */
                uint8_t dummy[7];
                (void)reg_read(g_t.addr, 0x0053u, dummy, 7);
            }
        } else {
            uint8_t st = 0;
            if (!reg_read(g_t.addr, GT911_STATUS, &st, 1)) {
                g_t.errors++;
                continue;
            }
            if (st & 0x80u) {
                /* A report: points, or none -- the finger lifted, which ends
                 * the contact now rather than after a timeout (a tap used to
                 * land 80-250 ms after the lift). */
                unsigned n = st & 0x0Fu;
                uint8_t p[8];
                got = n > 0 && reg_read(g_t.addr, GT911_POINT1, p, 8);
                lifted = n == 0;
                (void)reg_write8(g_t.addr, GT911_STATUS, 0);
                if (got) {
                    rx = (unsigned)(p[1] | (p[2] << 8));
                    ry = (unsigned)(p[3] | (p[4] << 8));
                }
            }
        }

        /* The ST7123 sends coordinate reports with no valid point all the
         * time, touched or not; while a contact is down, the first one is
         * the lift (measured on the Tab5, 2026-11-05: 92 during contacts,
         * none ever followed by another point of the same contact). */
        if (st_empty_now && down) { g_t.st_empty_down++; empty_seen = true; lifted = true; }
        if (got) {
            uint64_t now = time_get_ms();
            if (down) {
                if (now - t_last > g_t.gap_max_ms) g_t.gap_max_ms = (uint32_t)(now - t_last);
                if (empty_seen) g_t.st_empty_mid++;
            }
            empty_seen = false;
            t_last = now;
            g_t.reports++;
            map_point(rx, ry, &x, &y);
            g_t.last_x = x;
            g_t.last_y = y;
            int kind = down ? 2 : 1;
            if (!down) {
                down = true;
                g_t.presses++;
                int band = gesture_mm_px(TOUCH_EDGE_MM, CONFIG_PANEL_DPI);
                g_t.edge = g_t.scr_w && (x < band || x >= (int)g_t.scr_w - band);
                if (g_t.edge) g_t.edge_contacts++;
                win_t w = g_t.edge ? -1 : window_focused();
                g_t.owner = -1;
                if (w >= 0 && (window_claims(w) & WIN_CLAIM_POINTER) && window_origin(w, &g_t.owner_x0, &g_t.owner_y0))
                    g_t.owner = w;
                /* The window manager's contact: the control under it shows
                 * pressed until it lifts or moves off as a swipe or drag. */
                g_t.pressed = g_t.owner < 0 && !g_t.edge;
                g_t.repeat_ms = 0;
                g_t.capture = g_t.pressed && console_touch_captures((unsigned)x, (unsigned)y);
                if (g_t.pressed) console_touch_press((unsigned)x, (unsigned)y);
            } else if (g_t.capture) {
                console_touch_move((unsigned)x, (unsigned)y);
            } else if (g_t.pressed &&
                       (x - g_t.gs.x0 >= g_t.gs.tap_px || g_t.gs.x0 - x >= g_t.gs.tap_px ||
                        y - g_t.gs.y0 >= g_t.gs.tap_px || g_t.gs.y0 - y >= g_t.gs.tap_px)) {
                g_t.pressed = false;                     /* no longer a tap */
                console_touch_release();
            }
            g_t.log_kind = kind;
            g_t.log_seq++;
            gesture_t ev;
            gesture_kind_t gk = gesture_contact(&g_t.gs, true, x, y, (uint32_t)t_last, &ev);
            if (owned()) {
                owner_post(kind, x, y);
            } else if (gk == GESTURE_DRAG && !g_t.capture) {
                gesture_act(gk, &ev);
            }
        } else if (down) {
            /* No point this poll: ended if the controller said so, or if
             * reports stopped (a missed lift: 80 ms on the ST7123, which
             * reports at every 20 ms poll while touched -- 49 ms the longest
             * gap seen; 250 ms on the GT911). */
            uint64_t quiet = time_get_ms() - t_last;
            if (lifted || quiet > (g_t.chip == CHIP_ST7123 ? 80u : 250u)) {
                down = false;
                if (lifted) g_t.ends_lifted++; else g_t.ends_quiet++;
                contact_end(x, y, t_last);
            } else {
                gesture_t ev;
                gesture_kind_t k = gesture_tick(&g_t.gs, (uint32_t)time_get_ms(), &ev);
                if (k != GESTURE_NONE && !owned() && !g_t.capture) gesture_act(k, &ev);
            }
        }
        if (down && got) {
            gesture_t ev;
            gesture_kind_t k = gesture_tick(&g_t.gs, (uint32_t)time_get_ms(), &ev);
            if (k != GESTURE_NONE && !owned() && !g_t.capture) gesture_act(k, &ev);
        }
        /* A held press repeats every TOUCH_REPEAT_MS until it lifts or moves
         * off (both clear pressed). */
        if (down && g_t.pressed && g_t.repeat_ms) {
            uint64_t now = time_get_ms();
            if (now >= g_t.repeat_ms) {
                console_touch_repeat();
                g_t.repeat_ms = now + TOUCH_REPEAT_MS;
            }
        }
    }
}

int touch_start(void) {
    /* 1. Probe ST7123 / ST7121 at 0x55 */
    uint8_t ver = 0;
    if (reg_read(ST7123_ADDR, ST7123_FW_VER, &ver, 1) && ver != 0) {
        g_t.addr = ST7123_ADDR;
        g_t.chip = CHIP_ST7123;
        g_t.chip_name = (ver == 0x01) ? "ST7121" : "ST7123";
        uint8_t info[5] = { 0 };
        if (reg_read(ST7123_ADDR, ST7123_MAX_X_H, info, 5)) {
            g_t.res_x = ((uint16_t)info[0] << 8) | info[1];
            g_t.res_y = ((uint16_t)info[2] << 8) | info[3];
        }
        if (!g_t.res_x || !g_t.res_y) { g_t.res_x = 720; g_t.res_y = 1280; }
        g_t.found = true;
    }

    /* 2. Probe GT911 at 0x5D, then 0x14 */
    if (!g_t.found) {
        uint8_t id[4] = { 0 };
        uint8_t addrs[2] = { GT911_ADDR, GT911_ADDR_ALT };
        for (int i = 0; i < 2; i++) {
            uint8_t a = addrs[i];
            if (reg_read(a, GT911_PRODUCT_ID, id, 4) &&
                id[0] == '9' && id[1] == '1' && id[2] == '1') {
                g_t.addr = a;
                g_t.chip = CHIP_GT911;
                g_t.chip_name = "GT911";
                uint8_t r[4] = { 0 };
                if (reg_read(a, GT911_RES, r, 4)) {
                    g_t.res_x = (uint16_t)(r[0] | (r[1] << 8));
                    g_t.res_y = (uint16_t)(r[2] | (r[3] << 8));
                }
                if (!g_t.res_x || !g_t.res_y) { g_t.res_x = 800; g_t.res_y = 480; }
                g_t.found = true;
                break;
            }
        }
    }

    if (!g_t.found) {
        printk("[Touch] no supported touch controller (0x55, 0x5d, 0x14); no touch input\n");
        return -1;
    }

    const uint8_t *fb;
    unsigned w = 0, h = 0, stride;
    if (console_pixels(&fb, &w, &h, &stride) && w && h) {
        g_t.scr_w = w;
        g_t.scr_h = h;
    } else {
        g_t.scr_w = g_t.res_x;
        g_t.scr_h = g_t.res_y;
    }

    int pid = task_create_driver("touch", touch_task_body, 0, 1);
    if (pid < 0) { printk("[Touch] could not start the touch task\n"); return -1; }
    printk("[Touch] %s at 0x%02x, %ux%u, polled every %u ms by task #%d\n",
           g_t.chip_name, g_t.addr, (unsigned)g_t.res_x, (unsigned)g_t.res_y, POLL_MS, pid);
    return pid;
}

/* `touch tap X Y`, `touch swipe left|right|up|down`: what the gesture code
 * hands the console for a real tap or swipe, so a test or a person at the
 * serial console can drive the touch paths without a finger. */
static bool touch_inject(const char *args) {
    if (strncmp(args, "tap", 3) == 0) {
        const char *p = args + 3;
        unsigned v[2] = { 0, 0 };
        for (int i = 0; i < 2; i++) {
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') { cprintf("usage: touch tap X Y (screen pixels)\n"); return true; }
            while (*p >= '0' && *p <= '9') v[i] = v[i] * 10u + (unsigned)(*p++ - '0');
        }
        /* as a finger does: down (press feedback), the release, the tap;
         * "hold" keeps the finger down 150 ms, so the input path sees the
         * press on its own first */
        console_touch_press(v[0], v[1]);
        if (strstr(p, "hold")) task_sleep_ms(150);
        console_touch_release();
        console_touch_tap(v[0], v[1]);
        cprintf("touch: tap at %u,%u\n", v[0], v[1]);
        return true;
    }
    if (strncmp(args, "hold", 4) == 0) {
        /* a finger held at X Y for MS ms: the press, its repeats from the
         * long press on, the release; no tap, as a long press makes none */
        const char *p = args + 4;
        unsigned v[3] = { 0, 0, 0 };
        for (int i = 0; i < 3; i++) {
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') { cprintf("usage: touch hold X Y MS\n"); return true; }
            while (*p >= '0' && *p <= '9') v[i] = v[i] * 10u + (unsigned)(*p++ - '0');
        }
        console_touch_press(v[0], v[1]);
        uint64_t t0 = time_get_ms(), next = t0 + GESTURE_LONG_MS;
        unsigned reps = 0;
        while (time_get_ms() - t0 < v[2]) {
            if (time_get_ms() >= next) { console_touch_repeat(); reps++; next += TOUCH_REPEAT_MS; }
            task_sleep_ms(10);
        }
        console_touch_release();
        cprintf("touch: held at %u,%u for %u ms, %u repeats\n", v[0], v[1], v[2], reps);
        return true;
    }
    if (strncmp(args, "osk", 3) == 0) {
        if (strstr(args, "show")) {
            uint8_t req[2] = { 'k', 1 }, reply[SCREEN_REPLY_LEN];
            (void)console_canvas(req, 2, reply);
        }
        int x0, y0, w, h;
        if (lcdterm_osk_rect(&x0, &y0, &w, &h)) cprintf("touch: keyboard at x %d..%d y %d..%d\n", x0, x0 + w - 1, y0, y0 + h - 1);
        else cprintf("touch: no on-screen keyboard shown\n");
        return true;
    }
    if (strncmp(args, "swipe", 5) == 0) {
        const char *p = args + 5;
        while (*p == ' ') p++;
        /* The finger moving left brings the window on the right into view. */
        if (strcmp(p, "left") == 0) console_hotkey(CONSOLE_HOTKEY_FOCUS_RIGHT);
        else if (strcmp(p, "right") == 0) console_hotkey(CONSOLE_HOTKEY_FOCUS_LEFT);
        else if (strcmp(p, "up") == 0) console_touch_scroll(-200, 150);
        else if (strcmp(p, "down") == 0) console_touch_scroll(200, 150);
        else { cprintf("usage: touch swipe left|right|up|down\n"); return true; }
        cprintf("touch: swipe %s\n", p);
        return true;
    }
    return false;
}

void touch_command(const char *args) {
    while (*args == ' ') args++;
    if (touch_inject(args)) return;
    if (!g_t.found) { cprintf("touch: no touch controller found at boot\n"); return; }
    if (args[0] == 'l') {                               /* `ts log [s]` */
        const char *p = args;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        uint32_t s = 0;
        while (*p >= '0' && *p <= '9') s = s * 10u + (uint32_t)(*p++ - '0');
        if (!s) s = 20;
        cprintf("touch: logging for %lu s (Ctrl-C stops)\n", (unsigned long)s);
        uint32_t seen = g_t.log_seq, taps = g_t.taps, swipes = g_t.swipes;
        uint64_t end = time_get_ms() + (uint64_t)s * 1000u;
        while (time_get_ms() < end) {
            if (console_interrupt_requested()) break;
            if (g_t.log_seq != seen) {
                seen = g_t.log_seq;
                static const char *const k[] = { "?", "press", "move", "release" };
                int kind = g_t.log_kind;
                if (kind != 2) cprintf("touch: %-7s at %d,%d\n", k[kind & 3], g_t.last_x, g_t.last_y);
            }
            if (g_t.taps != taps) { taps = g_t.taps; cprintf("touch:   -> tap\n"); }
            if (g_t.swipes != swipes) { swipes = g_t.swipes; cprintf("touch:   -> swipe\n"); }
            task_sleep_ms(10);
        }
        cprintf("touch: done\n");
        return;
    }
    cprintf("touch: %s at 0x%02x, %ux%u -> screen %ux%u%s, polled every %u ms\n",
            g_t.chip_name ? g_t.chip_name : "controller",
            g_t.addr, (unsigned)g_t.res_x, (unsigned)g_t.res_y, g_t.scr_w, g_t.scr_h,
#if defined(CONFIG_TOUCH_MIRROR_X) && defined(CONFIG_TOUCH_MIRROR_Y)
            ", mirrored in X and Y"
#elif defined(CONFIG_TOUCH_MIRROR_X)
            ", mirrored in X"
#elif defined(CONFIG_TOUCH_MIRROR_Y)
            ", mirrored in Y"
#endif
#if defined(CONFIG_LCD_ROTATE_180)
            ", turned 180 degrees with the picture",
#else
            "",
#endif
            POLL_MS);
    cprintf("touch: %lu polls, %lu reports, %lu presses: %lu taps, %lu long presses, %lu swipes, %lu drags, "
            "%lu neither; %lu I2C errors\n",
            (unsigned long)g_t.polls, (unsigned long)g_t.reports, (unsigned long)g_t.presses,
            (unsigned long)g_t.taps, (unsigned long)g_t.longs, (unsigned long)g_t.swipes,
            (unsigned long)g_t.drags, (unsigned long)g_t.nothings, (unsigned long)g_t.errors);
    cprintf("touch: contacts ended by a lift report %lu, by reports stopping %lu; "
            "ST7123 reports without a valid point %lu (%lu while down, %lu mid-contact); "
            "longest gap in a contact %lu ms\n",
            (unsigned long)g_t.ends_lifted, (unsigned long)g_t.ends_quiet, (unsigned long)g_t.st_empty,
            (unsigned long)g_t.st_empty_down, (unsigned long)g_t.st_empty_mid, (unsigned long)g_t.gap_max_ms);
    cprintf("touch: %lu contacts began in the edge band (%d px, the window manager's)\n",
            (unsigned long)g_t.edge_contacts, gesture_mm_px(TOUCH_EDGE_MM, CONFIG_PANEL_DPI));
    cprintf("touch: %u dpi: a tap moves under %d px, a swipe crosses %d px\n",
            (unsigned)CONFIG_PANEL_DPI, (int)g_t.gs.tap_px, (int)g_t.gs.swipe_px);
    if (g_t.presses) cprintf("touch: last point %d,%d\n", g_t.last_x, g_t.last_y);
}

uint32_t touch_press_count(void) {
    return g_t.presses;
}

#else

int touch_start(void) { return -1; }
void touch_command(const char *args) { (void)args; cprintf("touch: this board has no touch controller\n"); }
uint32_t touch_press_count(void) { return 0; }

#endif
