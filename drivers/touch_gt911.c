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
 * Gestures come from the first point only, from press to release:
 *   moved less than TAP_PX in both axes, within TAP_MS     -> a tap
 *   moved at least SWIPE_PX sideways, twice as far as down -> a swipe
 * anything else is nothing. */

#include "drivers/touch.h"
#include "drivers/i2c_bus.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>

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
#if defined(CONFIG_FONT_HIDPI)
#define TAP_PX    48        /* screen pixels on HiDPI (~300 DPI): finger wobble */
#else
#define TAP_PX    24        /* screen pixels on standard DPI (~133-170 DPI) */
#endif
#define TAP_MS    600u
#define SWIPE_DIV 8         /* a swipe crosses at least 1/8 of the width/height */

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
    uint32_t          polls, reports, presses, taps, swipes, nothings, errors;
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

static void gesture(int x0, int y0, int x1, int y1, uint32_t ms) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int swipe_px_x = (int)(g_t.scr_w ? g_t.scr_w : 800u) / SWIPE_DIV;
    int swipe_px_y = (int)(g_t.scr_h ? g_t.scr_h : 480u) / SWIPE_DIV;
    if (adx >= swipe_px_x && adx >= 2 * ady) {
        /* The finger drags the ribbon: moving it left brings the window on
         * the right into view, as Cmd+Right does. */
        g_t.swipes++;
        console_hotkey(dx < 0 ? CONSOLE_HOTKEY_FOCUS_RIGHT : CONSOLE_HOTKEY_FOCUS_LEFT);
    } else if (ady >= swipe_px_y && ady >= 2 * adx) {
        /* Vertical swipe: scroll document up/down with momentum/inertia */
        g_t.swipes++;
        console_touch_scroll(dy, ms);
    } else if (adx < TAP_PX && ady < TAP_PX && ms <= TAP_MS) {
        g_t.taps++;
        console_touch_tap((unsigned)x0, (unsigned)y0);
    } else {
        g_t.nothings++;
    }
}

static void touch_task_body(void *arg) {
    (void)arg;
    bool down = false;
    int x0 = 0, y0 = 0, x = 0, y = 0;
    uint64_t t0 = 0, t_last = 0;
    for (;;) {
        task_sleep_ms(POLL_MS);
        g_t.polls++;

        bool got = false;
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
            if (!(st & 0x80u)) {
                if (down && time_get_ms() - t_last > 250u) {
                    down = false;
                    g_t.log_kind = 3;
                    g_t.log_seq++;
                    gesture(x0, y0, x, y, (uint32_t)(t_last - t0));
                }
                continue;
            }
            unsigned n = st & 0x0Fu;
            uint8_t p[8];
            got = n > 0 && reg_read(g_t.addr, GT911_POINT1, p, 8);
            (void)reg_write8(g_t.addr, GT911_STATUS, 0);
            if (got) {
                rx = (unsigned)(p[1] | (p[2] << 8));
                ry = (unsigned)(p[3] | (p[4] << 8));
            }
        }

        if (got) {
            t_last = time_get_ms();
            g_t.reports++;
            map_point(rx, ry, &x, &y);
            g_t.last_x = x;
            g_t.last_y = y;
            if (!down) {
                down = true;
                x0 = x; y0 = y;
                t0 = time_get_ms();
                g_t.presses++;
                g_t.log_kind = 1;
            } else {
                g_t.log_kind = 2;
            }
            g_t.log_seq++;
        } else if (down) {
            if (time_get_ms() - t_last > 80u) {
                down = false;
                g_t.log_kind = 3;
                g_t.log_seq++;
                gesture(x0, y0, x, y, (uint32_t)(t_last - t0));
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

void touch_command(const char *args) {
    while (*args == ' ') args++;
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
    cprintf("touch: %lu polls, %lu reports, %lu presses: %lu taps, %lu swipes, %lu neither; %lu I2C errors\n",
            (unsigned long)g_t.polls, (unsigned long)g_t.reports, (unsigned long)g_t.presses,
            (unsigned long)g_t.taps, (unsigned long)g_t.swipes, (unsigned long)g_t.nothings,
            (unsigned long)g_t.errors);
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
