/* drivers/gesture.c -- one finger's contacts into gestures (gesture.h). */

#include "drivers/gesture.h"

#include <string.h>

int gesture_mm_px(unsigned mm, unsigned dpi) {
    if (!dpi) dpi = 133u;
    return (int)((mm * dpi * 10u + 127u) / 254u);     /* 25.4 mm an inch, rounded */
}

void gesture_init(gesture_state_t *g, unsigned dpi) {
    memset(g, 0, sizeof *g);
    g->tap_px = (int16_t)gesture_mm_px(GESTURE_TAP_MM, dpi);
    g->swipe_px = (int16_t)gesture_mm_px(GESTURE_SWIPE_MM, dpi);
}

static int iabs(int v) { return v < 0 ? -v : v; }

static gesture_kind_t emit(gesture_kind_t kind, const gesture_state_t *g, uint32_t now_ms, gesture_t *out) {
    if (out) {
        out->kind = (uint8_t)kind;
        out->x = g->x0;
        out->y = g->y0;
        out->dx = (int16_t)(g->x - g->x0);
        out->dy = (int16_t)(g->y - g->y0);
        out->ms = now_ms - g->t0;
    }
    return kind;
}

gesture_kind_t gesture_contact(gesture_state_t *g, bool down, int x, int y, uint32_t now_ms, gesture_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (down) {
        if (!g->down) {
            g->down = true;
            g->long_sent = false;
            g->x0 = (int16_t)x;
            g->y0 = (int16_t)y;
            g->t0 = now_ms;
        }
        g->x = (int16_t)x;
        g->y = (int16_t)y;
        return GESTURE_NONE;
    }
    if (!g->down) return GESTURE_NONE;
    g->down = false;
    if (g->long_sent) return GESTURE_NONE;

    int dx = g->x - g->x0, dy = g->y - g->y0;
    int adx = iabs(dx), ady = iabs(dy);
    uint32_t ms = now_ms - g->t0;
    if (adx >= g->swipe_px && adx >= 2 * ady)
        return emit(dx < 0 ? GESTURE_SWIPE_LEFT : GESTURE_SWIPE_RIGHT, g, now_ms, out);
    if (ady >= g->swipe_px && ady >= 2 * adx)
        return emit(dy < 0 ? GESTURE_SWIPE_UP : GESTURE_SWIPE_DOWN, g, now_ms, out);
    if (adx < g->tap_px && ady < g->tap_px && ms <= GESTURE_TAP_MAX_MS)
        return emit(GESTURE_TAP, g, now_ms, out);
    return GESTURE_NONE;
}

gesture_kind_t gesture_tick(gesture_state_t *g, uint32_t now_ms, gesture_t *out) {
    if (out) memset(out, 0, sizeof *out);
    if (!g->down || g->long_sent) return GESTURE_NONE;
    if (now_ms - g->t0 < GESTURE_LONG_MS) return GESTURE_NONE;
    if (iabs(g->x - g->x0) >= g->tap_px || iabs(g->y - g->y0) >= g->tap_px) return GESTURE_NONE;
    g->long_sent = true;
    return emit(GESTURE_LONG_PRESS, g, now_ms, out);
}
