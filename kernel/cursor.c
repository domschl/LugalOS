#include "kernel/cursor.h"
#include "kernel/console.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include <string.h>

/* 16x16 Desktop Arrow Cursor
 * '#' = Black border (RGB565 0x0000)
 * '*' = White fill   (RGB565 0xFFFF)
 * '.' = Transparent
 */
static const char *const k_cursor_arrow[16] = {
    "#...............",
    "##..............",
    "#*#.............",
    "#**#............",
    "#***#...........",
    "#****#..........",
    "#*****#.........",
    "#******#........",
    "#*******#.......",
    "#*****####......",
    "#**#**#.........",
    "#*#.#**#........",
    "##..#**#........",
    "#....#**#.......",
    ".....#**#.......",
    "......##........"
};

/* single task: cursor engine; guarded by: g_cursor_lock */
static cursor_state_t g_cursor;
static ylock_t        g_cursor_lock;

typedef struct {
    uint16_t *fb;
    uint16_t  bg[CURSOR_H][CURSOR_W];
    int       x;
    int       y;
    bool      has_saved;
} cursor_fb_slot_t;

/* guarded by: g_cursor_lock */
static cursor_fb_slot_t s_slots[2];
static int            s_prev_wheel = 0;
static uint8_t        s_prev_buttons = 0;
static cursor_restore_fn s_restore_cb = NULL;

static cursor_fb_slot_t *cursor_get_slot(uint16_t *fb) {
    if (!fb) return NULL;
    if (s_slots[0].fb == fb) return &s_slots[0];
    if (s_slots[1].fb == fb) return &s_slots[1];
    if (!s_slots[0].fb) { s_slots[0].fb = fb; return &s_slots[0]; }
    if (!s_slots[1].fb) { s_slots[1].fb = fb; return &s_slots[1]; }
    s_slots[0].fb = fb;
    s_slots[0].has_saved = false;
    return &s_slots[0];
}

void cursor_init(void) {
    memset(&g_cursor, 0, sizeof(g_cursor));
    g_cursor.x = 960;
    g_cursor.y = 540;
    g_cursor.visible = true;
    g_cursor.speed_factor = 100;
    memset(s_slots, 0, sizeof(s_slots));
    s_prev_wheel = 0;
    s_prev_buttons = 0;
    s_restore_cb = NULL;
}

void cursor_set_restore_cb(cursor_restore_fn cb) {
    ylock_acquire(&g_cursor_lock);
    s_restore_cb = cb;
    ylock_release(&g_cursor_lock);
}

void cursor_update(int x, int y, uint8_t buttons, int wheel) {
    ylock_acquire(&g_cursor_lock);

    int prev_x = g_cursor.x;
    int prev_y = g_cursor.y;
    uint8_t prev_btn = g_cursor.buttons;

    g_cursor.x = x;
    g_cursor.y = y;
    g_cursor.buttons = buttons;
    g_cursor.wheel = wheel;

    if (x != prev_x || y != prev_y) {
        g_cursor.moves_count++;
    }
    if ((buttons & 1u) && !(prev_btn & 1u)) {
        g_cursor.clicks_count++;
    }

    int d_wheel = wheel - s_prev_wheel;
    s_prev_wheel = wheel;
    s_prev_buttons = buttons;

    ylock_release(&g_cursor_lock);

    /* Phase 71 pointer & wheel contract */
    console_pointer(x, y, buttons);
    if (d_wheel != 0) {
        console_wheel(0, d_wheel);
    }
}

static void cursor_restore_locked(uint16_t *fb, unsigned fb_w, unsigned fb_h) {
    if (!fb) return;
    cursor_fb_slot_t *slot = cursor_get_slot(fb);
    if (!slot || !slot->has_saved) return;

    if (s_restore_cb) {
        s_restore_cb(fb, fb_w, fb_h, slot->x, slot->y);
    } else {
        for (int r = 0; r < CURSOR_H; r++) {
            int py = slot->y + r;
            if (py < 0 || (unsigned)py >= fb_h) continue;

            for (int c = 0; c < CURSOR_W; c++) {
                int px = slot->x + c;
                if (px < 0 || (unsigned)px >= fb_w) continue;

                fb[py * fb_w + px] = slot->bg[r][c];
            }
        }
    }
    slot->has_saved = false;
}

void cursor_restore(uint16_t *fb, unsigned fb_w, unsigned fb_h) {
    if (!fb) return;
    ylock_acquire(&g_cursor_lock);
    cursor_restore_locked(fb, fb_w, fb_h);
    ylock_release(&g_cursor_lock);
}

void cursor_render(uint16_t *fb, unsigned fb_w, unsigned fb_h) {
    if (!fb) return;

    ylock_acquire(&g_cursor_lock);
    int cx = g_cursor.x;
    int cy = g_cursor.y;
    bool vis = g_cursor.visible;

    if (!vis) {
        ylock_release(&g_cursor_lock);
        return;
    }

    /* 1. Restore previous background on this framebuffer */
    cursor_restore_locked(fb, fb_w, fb_h);

    cursor_fb_slot_t *slot = cursor_get_slot(fb);
    if (!slot) {
        ylock_release(&g_cursor_lock);
        return;
    }

    /* 2. Save new background pixels (only when no authoritative restore callback) */
    if (!s_restore_cb) {
        for (int r = 0; r < CURSOR_H; r++) {
            int py = cy + r;
            if (py < 0 || (unsigned)py >= fb_h) continue;

            for (int c = 0; c < CURSOR_W; c++) {
                int px = cx + c;
                if (px < 0 || (unsigned)px >= fb_w) continue;

                slot->bg[r][c] = fb[py * fb_w + px];
            }
        }
    }
    slot->x = cx;
    slot->y = cy;
    slot->has_saved = true;

    /* 3. Blit cursor sprite */
    for (int r = 0; r < CURSOR_H; r++) {
        int py = cy + r;
        if (py < 0 || (unsigned)py >= fb_h) continue;

        const char *row = k_cursor_arrow[r];
        for (int c = 0; c < CURSOR_W; c++) {
            int px = cx + c;
            if (px < 0 || (unsigned)px >= fb_w) continue;

            char ch = row[c];
            if (ch == '#') {
                fb[py * fb_w + px] = 0x0000; /* Black outline */
            } else if (ch == '*') {
                fb[py * fb_w + px] = 0xFFFF; /* White fill */
            }
        }
    }
    ylock_release(&g_cursor_lock);
}

void cursor_set_speed(int speed) {
    if (speed < 10) speed = 10;
    if (speed > 500) speed = 500;
    ylock_acquire(&g_cursor_lock);
    g_cursor.speed_factor = speed;
    ylock_release(&g_cursor_lock);
}

void cursor_set_visible(bool visible) {
    ylock_acquire(&g_cursor_lock);
    g_cursor.visible = visible;
    ylock_release(&g_cursor_lock);
}

const cursor_state_t *cursor_get_state(void) {
    return &g_cursor;
}

void cursor_cmd_status(void) {
    ylock_acquire(&g_cursor_lock);
    int x = g_cursor.x;
    int y = g_cursor.y;
    bool vis = g_cursor.visible;
    uint8_t btns = g_cursor.buttons;
    int wheel = g_cursor.wheel;
    int spd = g_cursor.speed_factor;
    uint32_t moves = g_cursor.moves_count;
    uint32_t clicks = g_cursor.clicks_count;
    ylock_release(&g_cursor_lock);

    cprintf("Mouse Cursor Engine (16x16 Arrow Sprite):\n");
    cprintf("  Position:     X=%d, Y=%d (Hotspot: top-left 0,0)\n", x, y);
    cprintf("  Visibility:   %s\n", vis ? "VISIBLE" : "HIDDEN");
    cprintf("  Buttons:      0x%02x (Left:%u Right:%u Middle:%u)\n",
            btns, (btns & 1) ? 1 : 0, (btns & 2) ? 1 : 0, (btns & 4) ? 1 : 0);
    cprintf("  Wheel:        %d\n", wheel);
    cprintf("  Speed:        %d%%\n", spd);
    cprintf("  Activity:     %u moves, %u clicks processed\n", moves, clicks);
}
