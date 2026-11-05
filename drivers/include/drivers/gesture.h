#ifndef LUGALOS_DRIVERS_GESTURE_H
#define LUGALOS_DRIVERS_GESTURE_H

/* drivers/gesture.c -- one finger's contacts into gestures.
 *
 * The touch driver reports each poll's contact (down, at x, y) or its end,
 * with a millisecond clock; the recogniser answers with what the finger did.
 * Thresholds are in millimetres, turned into pixels from the panel's DPI, so
 * a tap or a swipe is the same movement of a finger on every board.
 * Portable and allocation-free: the state is the caller's, one per contact
 * source, and a host harness drives it from recorded traces. */

#include <stdbool.h>
#include <stdint.h>

#define GESTURE_TAP_MM        5u    /* a tap moves less than this, in both axes */
#define GESTURE_SWIPE_MM     10u    /* a swipe crosses at least this, twice as far as across */
#define GESTURE_TAP_MAX_MS  600u    /* a tap is released within this */
#define GESTURE_LONG_MS     500u    /* held this long within the tap distance: a long press */

typedef enum {
    GESTURE_NONE = 0,
    GESTURE_TAP,           /* x, y: where it went down */
    GESTURE_LONG_PRESS,    /* x, y: where it went down; reported while still held */
    GESTURE_SWIPE_LEFT,    /* the finger moved left: dx < 0 */
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_UP,      /* the finger moved up: dy < 0 */
    GESTURE_SWIPE_DOWN,
} gesture_kind_t;

typedef struct {
    uint8_t  kind;         /* gesture_kind_t */
    int16_t  x, y;         /* where the contact began */
    int16_t  dx, dy;       /* how far it went, release point minus start */
    uint32_t ms;           /* how long it lasted */
} gesture_t;

typedef struct {
    int16_t  tap_px, swipe_px;
    bool     down, long_sent;
    int16_t  x0, y0, x, y;
    uint32_t t0;
} gesture_state_t;

/* Thresholds for a panel of `dpi` dots per inch (0: 133). */
void gesture_init(gesture_state_t *g, unsigned dpi);

/* The contact at x, y at now_ms (down), or its end (up). Returns the gesture
 * the end completes in *out (GESTURE_NONE when it was none, or when a long
 * press was already reported for it); a contact report returns NONE. */
gesture_kind_t gesture_contact(gesture_state_t *g, bool down, int x, int y, uint32_t now_ms, gesture_t *out);

/* Between reports, from the poll loop: a contact held still for
 * GESTURE_LONG_MS becomes a long press, once. */
gesture_kind_t gesture_tick(gesture_state_t *g, uint32_t now_ms, gesture_t *out);

/* Pixels for mm millimetres at dpi (exact integer arithmetic). */
int gesture_mm_px(unsigned mm, unsigned dpi);

#endif /* LUGALOS_DRIVERS_GESTURE_H */
