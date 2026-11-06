/* tests/host/gesture_host.c -- drivers/gesture.c against recorded-style traces:
 * one contact report per 20 ms poll, as the touch drivers produce, at the
 * three panels' densities. Runs under ASan/UBSan and valgrind. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/gesture.h"

static int g_fail = 0, g_run = 0;

#define CHECK(cond, ...) do { g_run++; if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What the reports of the last stroke() said while it was down. */
static int g_drag_sum, g_drags;

/* A straight stroke from (x0,y0) to (x1,y1) over `steps` polls 20 ms apart,
 * held still at the end for `hold` more polls, then the release one poll
 * later; ticks between reports. Returns the release's gesture, and in
 * *long_seen whether a tick reported a long press. */
static gesture_kind_t stroke_hold(unsigned dpi, int x0, int y0, int x1, int y1, int steps, int hold,
                                  gesture_t *out, bool *long_seen) {
    gesture_state_t g;
    gesture_init(&g, dpi);
    uint32_t t = 1000;
    *long_seen = false;
    g_drag_sum = g_drags = 0;
    for (int i = 0; i <= steps + hold; i++) {
        int k = i < steps ? i : steps;
        int x = x0 + (x1 - x0) * k / (steps ? steps : 1);
        int y = y0 + (y1 - y0) * k / (steps ? steps : 1);
        gesture_t d;
        if (gesture_contact(&g, true, x, y, t, &d) == GESTURE_DRAG) {
            g_drag_sum += d.dy;
            g_drags++;
        }
        gesture_t lp;
        if (gesture_tick(&g, t + 10, &lp) == GESTURE_LONG_PRESS) {
            *long_seen = true;
            CHECK(lp.x == x0 && lp.y == y0, "long press at the start point (%d,%d)", lp.x, lp.y);
        }
        t += 20;
    }
    return gesture_contact(&g, false, 0, 0, t, out);
}

static gesture_kind_t stroke(unsigned dpi, int x0, int y0, int x1, int y1, int steps, gesture_t *out, bool *long_seen) {
    return stroke_hold(dpi, x0, y0, x1, y1, steps, 0, out, long_seen);
}

static void test_mm(void) {
    CHECK(gesture_mm_px(10, 254) == 100, "10 mm at 254 dpi is 100 px, got %d", gesture_mm_px(10, 254));
    CHECK(gesture_mm_px(5, 294) == 58, "5 mm on the Tab5, got %d", gesture_mm_px(5, 294));
    CHECK(gesture_mm_px(10, 170) == 67, "10 mm on the 7B, got %d", gesture_mm_px(10, 170));
    CHECK(gesture_mm_px(10, 0) == gesture_mm_px(10, 133), "dpi 0 means 133");
}

static void test_board(unsigned dpi) {
    int tap = gesture_mm_px(GESTURE_TAP_MM, dpi), sw = gesture_mm_px(GESTURE_SWIPE_MM, dpi);
    gesture_t ev;
    bool lp;

    /* A tap: a few polls, no movement, then a wobble just under the limit. */
    CHECK(stroke(dpi, 300, 200, 300, 200, 4, &ev, &lp) == GESTURE_TAP && !lp, "still tap at %u dpi", dpi);
    CHECK(ev.x == 300 && ev.y == 200, "tap at its start point");
    CHECK(stroke(dpi, 300, 200, 300 + tap - 1, 200 - (tap - 1), 4, &ev, &lp) == GESTURE_TAP, "wobbly tap at %u dpi", dpi);
    /* Too slow for a tap and moved, so no long press either: nothing. */
    CHECK(stroke(dpi, 300, 200, 300 + tap + 1, 200, 40, &ev, &lp) == GESTURE_NONE, "slow small move is nothing");

    /* Held still: a long press while held, and nothing at the release. */
    CHECK(stroke(dpi, 300, 200, 300, 200, 40, &ev, &lp) == GESTURE_NONE && lp, "long press at %u dpi", dpi);
    /* Moved away before the time is up: no long press. */
    CHECK(stroke(dpi, 300, 200, 300 + 2 * sw, 200, 40, &ev, &lp) == GESTURE_SWIPE_RIGHT && !lp, "slow swipe right, no long press");

    /* The four swipes, just over the distance, and their reported motion. */
    CHECK(stroke(dpi, 500, 300, 500 - sw, 300, 5, &ev, &lp) == GESTURE_SWIPE_LEFT && ev.dx == -sw, "swipe left at %u dpi", dpi);
    CHECK(stroke(dpi, 500, 300, 500 + sw, 300 + sw / 3, 5, &ev, &lp) == GESTURE_SWIPE_RIGHT, "swipe right, a little downhill");
    /* Vertical strokes are drags: their movement reported while down, adding
     * up to the stroke; the release reports the speed over its last 100 ms. */
    CHECK(stroke(dpi, 500, 300, 500, 300 - sw, 5, &ev, &lp) == GESTURE_DRAG_END && g_drag_sum == -sw,
          "drag up at %u dpi: sum %d, want %d", dpi, g_drag_sum, -sw);
    CHECK(ev.ms >= 100 && ev.dy < 0, "drag up released moving: dy %d over %u ms", ev.dy, (unsigned)ev.ms);
    CHECK(stroke(dpi, 500, 300, 500 + sw / 3, 300 + sw, 5, &ev, &lp) == GESTURE_DRAG_END && g_drag_sum == sw,
          "drag down, a little sideways: sum %d", g_drag_sum);
    CHECK(g_drags >= 2, "a drag reports as it goes: %d reports", g_drags);
    /* Dragged, then held still before the release: no speed, no fling. */
    CHECK(stroke_hold(dpi, 500, 300, 500, 300 + 3 * sw, 10, 8, &ev, &lp) == GESTURE_DRAG_END && ev.dy == 0,
          "drag that stopped: release dy %d", ev.dy);
    CHECK(!lp, "a drag held still is not a long press");
    /* A vertical wobble under the tap distance is still a tap, not a drag. */
    CHECK(stroke(dpi, 300, 200, 300, 200 + tap - 1, 3, &ev, &lp) == GESTURE_TAP && g_drags == 0, "vertical wobble is a tap");

    /* Diagonal (neither axis twice the other) and short of a swipe: nothing. */
    CHECK(stroke(dpi, 500, 300, 500 + sw, 300 + sw, 5, &ev, &lp) == GESTURE_NONE, "diagonal is nothing");
    CHECK(stroke(dpi, 500, 300, 500 + sw - 1, 300, 5, &ev, &lp) == GESTURE_NONE, "one pixel short of a swipe");
}

static void test_release_without_contact(void) {
    gesture_state_t g;
    gesture_t ev;
    gesture_init(&g, 170);
    CHECK(gesture_contact(&g, false, 0, 0, 5, &ev) == GESTURE_NONE && ev.kind == GESTURE_NONE, "a release with nothing down");
    CHECK(gesture_tick(&g, 10000, &ev) == GESTURE_NONE, "a tick with nothing down");
}

int main(void) {
    printf("gesture_host: one-finger gestures\n");
    test_mm();
    test_board(294);     /* Tab5 */
    test_board(170);     /* LCD-7B */
    test_board(133);     /* RP2350-LCD-7 */
    test_release_without_contact();
    printf("gesture_host: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
