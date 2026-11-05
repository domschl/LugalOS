#ifndef LUGALOS_KERNEL_EVENT_H
#define LUGALOS_KERNEL_EVENT_H

#include <stdbool.h>
#include <stdint.h>

/* Unified Application Window Event Model (Phase 70, Milestone 70.6)
 *
 * Provides a standardized event delivery channel for interactive windowed
 * applications (Canvases, Games, Readers, Terminal overlays):
 * - Pointer events (tap down, tap up, drag/move) in local window content space.
 * - Keyboard events (ASCII, unicode codepoints, virtual navigation keys).
 * - Window lifecycle notifications (focus gained/lost, close requests, resize).
 * - Freestanding C, zero heap allocations, lockless SPSC queue.
 */

typedef enum {
    WIN_EV_NONE = 0,
    WIN_EV_POINTER_DOWN,
    WIN_EV_POINTER_UP,
    WIN_EV_POINTER_MOVE,
    WIN_EV_KEY_DOWN,
    WIN_EV_KEY_UP,
    WIN_EV_FOCUS_GAINED,
    WIN_EV_FOCUS_LOST,
    WIN_EV_CLOSE_REQUEST,
    WIN_EV_RESIZE,
} win_event_type_t;

typedef struct {
    uint8_t  type;      /* win_event_type_t */
    uint8_t  button;    /* 1: primary touch / left button, 2: secondary */
    uint8_t  mods;      /* KMOD_* (kernel/keyseq.h) or OSK_MOD_* (drivers/osk.h) */
    uint8_t  flags;
    int16_t  x;         /* local window content x coordinate (pixels) */
    int16_t  y;         /* local window content y coordinate (pixels) */
    int16_t  dx;        /* relative motion dx (pixels) */
    int16_t  dy;        /* relative motion dy (pixels) */
    uint32_t key;       /* key code or character */
    uint16_t w;         /* active window width (pixels) */
    uint16_t h;         /* active window height (pixels) */
} win_event_t;

/* Push an event into the active window's event ring.
 * Returns true if enqueued, false if ring is full. */
bool window_push_event(const win_event_t *ev);

/* Poll the next event for the active window. Pumps input if queue is empty.
 * Returns true if an event was delivered into *ev, false if queue is empty. */
bool window_poll_event(win_event_t *ev);

/* Clear pending window events. */
void window_clear_events(void);

#endif /* LUGALOS_KERNEL_EVENT_H */
