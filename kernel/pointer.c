/* kernel/pointer.c -- a relative pointer made absolute (kernel/pointer.h). */

#include "kernel/pointer.h"
#include "kernel/console.h"
#include "kernel/settings.h"

/* single task: the one reader of the board's mouse -- the console's input
 * pump (under its g_input_lock), or the VisionFive 2's display pacer. */
static int     g_px = -1, g_py = -1;   /* -1: not placed yet (the screen's middle at the first motion) */
static int     g_rx, g_ry;             /* the speed scaling's remainders, in 1/100 px */
static uint8_t g_buttons;

static int scaled(int d, int *rem, int speed) {
    int v = d * speed + *rem;
    int px = v / 100;
    *rem = v - px * 100;
    return px;
}

void pointer_relative(int dx, int dy, uint8_t buttons, int wheel) {
    const uint8_t *fb;
    unsigned w = 0, h = 0, stride = 0;
    if (!console_pixels(&fb, &w, &h, &stride) || w == 0 || h == 0) return;
    if (g_px < 0) {
        g_px = (int)w / 2;
        g_py = (int)h / 2;
    }
    int speed = settings_get("input.pointer_speed");
    if (speed <= 0) speed = 100;
    int x = g_px + scaled(dx, &g_rx, speed), y = g_py + scaled(dy, &g_ry, speed);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)w) x = (int)w - 1;
    if (y >= (int)h) y = (int)h - 1;
    if (x != g_px || y != g_py || buttons != g_buttons) {
        g_px = x;
        g_py = y;
        g_buttons = buttons;
        console_pointer(x, y, buttons);
    }
    if (wheel) console_wheel(0, wheel);
}

void pointer_release(void) {
    if (!g_buttons || g_px < 0) return;
    g_buttons = 0;
    console_pointer(g_px, g_py, 0);
}

void pointer_position(int *x, int *y) {
    *x = g_px < 0 ? 0 : g_px;
    *y = g_py < 0 ? 0 : g_py;
}
