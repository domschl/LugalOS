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
#define GT911_PRODUCT_ID  0x8140u
#define GT911_RES         0x8048u
#define GT911_STATUS      0x814Eu
#define GT911_POINT1      0x814Fu

#define POLL_MS   20u
#define TAP_PX    24        /* screen pixels: a finger wobbles */
#define TAP_MS    600u
#define SWIPE_DIV 8         /* a swipe crosses at least 1/8 of the width */

typedef struct {
    bool     found;
    uint16_t res_x, res_y;          /* the controller's coordinate space */
    unsigned scr_w, scr_h;          /* the screen's, when last mapped */
    uint32_t polls, reports, presses, taps, swipes, nothings, errors;
    int      last_x, last_y;        /* mapped, the last point seen */
    volatile uint32_t log_seq;      /* bumped per event, for `ts log` */
    volatile int      log_kind;     /* 1 press, 2 move, 3 release */
} touch_t;

static touch_t g_t;

static bool reg_read(uint16_t reg, uint8_t *buf, uint32_t n) {
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)reg };
    return i2c_xfer(GT911_ADDR, a, 2, buf, n);
}

static bool reg_write8(uint16_t reg, uint8_t v) {
    uint8_t a[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, v };
    return i2c_xfer(GT911_ADDR, a, 3, 0, 0);
}

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
    unsigned px = g_t.res_x ? rx * w / g_t.res_x : rx;
    unsigned py = g_t.res_y ? ry * h / g_t.res_y : ry;
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
    int swipe_px = (int)(g_t.scr_w ? g_t.scr_w : 800u) / SWIPE_DIV;
    if (adx >= swipe_px && adx >= 2 * ady) {
        /* The finger drags the ribbon: moving it left brings the window on
         * the right into view, as Cmd+Right does. */
        g_t.swipes++;
        console_hotkey(dx < 0 ? CONSOLE_HOTKEY_FOCUS_RIGHT : CONSOLE_HOTKEY_FOCUS_LEFT);
    } else if (ady >= swipe_px && ady >= 2 * adx) {
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
        uint8_t st = 0;
        if (!reg_read(GT911_STATUS, &st, 1)) { g_t.errors++; continue; }
        if (!(st & 0x80u)) {                        /* nothing new */
            /* A held finger reports every ~10 ms; silence that long means
             * the release report was missed. */
            if (down && time_get_ms() - t_last > 250u) {
                down = false;
                g_t.log_kind = 3;
                g_t.log_seq++;
                gesture(x0, y0, x, y, (uint32_t)(t_last - t0));
            }
            continue;
        }
        t_last = time_get_ms();
        unsigned n = st & 0x0Fu;
        uint8_t p[8];
        bool got = n > 0 && reg_read(GT911_POINT1, p, 8);
        (void)reg_write8(GT911_STATUS, 0);          /* take the report */
        g_t.reports++;
        if (got) {
            map_point((unsigned)(p[1] | (p[2] << 8)), (unsigned)(p[3] | (p[4] << 8)), &x, &y);
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
        } else if (n == 0 && down) {
            down = false;
            g_t.log_kind = 3;
            g_t.log_seq++;
            gesture(x0, y0, x, y, (uint32_t)(time_get_ms() - t0));
        }
    }
}

int touch_start(void) {
    uint8_t id[4] = { 0, 0, 0, 0 };
    if (!reg_read(GT911_PRODUCT_ID, id, 4) || id[0] != '9' || id[1] != '1' || id[2] != '1') {
        printk("[Touch] no GT911 at 0x%02x; no touch input\n", GT911_ADDR);
        return -1;
    }
    uint8_t r[4] = { 0, 0, 0, 0 };
    if (reg_read(GT911_RES, r, 4)) {
        g_t.res_x = (uint16_t)(r[0] | (r[1] << 8));
        g_t.res_y = (uint16_t)(r[2] | (r[3] << 8));
    }
    if (!g_t.res_x || !g_t.res_y) { g_t.res_x = 800; g_t.res_y = 480; }
    g_t.found = true;
    int pid = task_create_driver("touch", touch_task_body, 0, 1);
    if (pid < 0) { printk("[Touch] could not start the touch task\n"); return -1; }
    printk("[Touch] GT911 at 0x%02x, %ux%u, polled every %u ms by task #%d\n", GT911_ADDR,
           (unsigned)g_t.res_x, (unsigned)g_t.res_y, POLL_MS, pid);
    return pid;
}

void touch_command(const char *args) {
    while (*args == ' ') args++;
    if (!g_t.found) { cprintf("touch: no GT911 found at boot\n"); return; }
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
    cprintf("touch: GT911 at 0x%02x, %ux%u -> screen %ux%u%s, polled every %u ms\n", GT911_ADDR,
            (unsigned)g_t.res_x, (unsigned)g_t.res_y, g_t.scr_w, g_t.scr_h,
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
