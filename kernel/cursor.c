/* kernel/cursor.c -- the mouse cursor on an RGB565 scan-out the CPU composes
 * (kernel/include/kernel/cursor.h). */

#include "kernel/cursor.h"
#include "kernel/console.h"
#include "kernel/lock.h"

#include <string.h>

typedef struct {
    uint16_t *fb;
    uint16_t  bg[CURSOR_H][CURSOR_W];   /* what was under it, without a restore callback */
    int       x, y;
    bool      has_saved;
} cursor_fb_slot_t;

static ylock_t           g_cursor_lock;
/* guarded by: g_cursor_lock */
static cursor_fb_slot_t  s_slots[2];
static cursor_restore_fn s_restore_cb;  /* guarded by: g_cursor_lock */

static cursor_fb_slot_t *cursor_get_slot(uint16_t *fb) {
    if (s_slots[0].fb == fb) return &s_slots[0];
    if (s_slots[1].fb == fb) return &s_slots[1];
    if (!s_slots[0].fb) { s_slots[0].fb = fb; return &s_slots[0]; }
    if (!s_slots[1].fb) { s_slots[1].fb = fb; return &s_slots[1]; }
    s_slots[0].fb = fb;
    s_slots[0].has_saved = false;
    return &s_slots[0];
}

void cursor_init(void) {
    ylock_acquire(&g_cursor_lock);
    memset(s_slots, 0, sizeof(s_slots));
    s_restore_cb = NULL;
    ylock_release(&g_cursor_lock);
}

void cursor_set_restore_cb(cursor_restore_fn cb) {
    ylock_acquire(&g_cursor_lock);
    s_restore_cb = cb;
    ylock_release(&g_cursor_lock);
}

static void cursor_restore_locked(uint16_t *fb, unsigned fb_w, unsigned fb_h) {
    cursor_fb_slot_t *slot = cursor_get_slot(fb);
    if (!slot->has_saved) return;
    if (s_restore_cb) {
        s_restore_cb(fb, fb_w, fb_h, slot->x, slot->y);
    } else {
        for (int r = 0; r < CURSOR_H; r++) {
            int py = slot->y + r;
            if (py < 0 || (unsigned)py >= fb_h) continue;
            for (int c = 0; c < CURSOR_W; c++) {
                int px = slot->x + c;
                if (px >= 0 && (unsigned)px < fb_w) fb[py * fb_w + px] = slot->bg[r][c];
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
    int cx = 0, cy = 0;
    bool shown = console_pointer_shown(&cx, &cy);
    ylock_acquire(&g_cursor_lock);
    cursor_restore_locked(fb, fb_w, fb_h);
    if (!shown) {
        ylock_release(&g_cursor_lock);
        return;
    }
    cursor_fb_slot_t *slot = cursor_get_slot(fb);
    for (int r = 0; r < CURSOR_H; r++) {
        int py = cy + r;
        if (py < 0 || (unsigned)py >= fb_h) continue;
        uint16_t ink = k_cursor_ink[r], fill = k_cursor_fill[r];
        for (int c = 0; c < CURSOR_W; c++) {
            int px = cx + c;
            if (px < 0 || (unsigned)px >= fb_w) continue;
            uint16_t *p = &fb[py * fb_w + px];
            if (!s_restore_cb) slot->bg[r][c] = *p;
            uint16_t bit = (uint16_t)(0x8000u >> c);
            if (ink & bit) *p = 0x0000;
            else if (fill & bit) *p = 0xFFFF;
        }
    }
    slot->x = cx;
    slot->y = cy;
    slot->has_saved = true;
    ylock_release(&g_cursor_lock);
}
