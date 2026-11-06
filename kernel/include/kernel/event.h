#ifndef LUGALOS_KERNEL_EVENT_H
#define LUGALOS_KERNEL_EVENT_H

#include <stdbool.h>
#include <stdint.h>

/* A window's events (kernel/winevent.c): each canvas window of the ribbon
 * has its own ring, so two programs on two windows (a game paused in the
 * background, chess beside it) each get only their own.
 *
 *   - Pointer events in the window's content coordinates: a tap as
 *     POINTER_DOWN + POINTER_UP (flags WIN_EVF_TAP), framed by a press pair
 *     (WIN_EVF_PRESS) for feedback while the finger is down; with WIN_CLAIM_POINTER
 *     the raw stream instead -- every contact's DOWN, MOVE (successive moves
 *     merged while unread) and UP, as long as the window had the focus when
 *     the contact began and it did not begin at the screen's edge (that one
 *     is the window manager's).
 *   - WIN_EV_CLOSE_REQUEST when the window's close box or Cmd+W closed it.
 *
 * Keys are not here: they reach the focused window as bytes, through
 * keyseq_read_canvas() (kernel/keyseq.h), which decodes the sequences.
 *
 * A window's handle is its canvas slot; window_by_name() finds it. Only on
 * boards with a panel: elsewhere window_poll() finds nothing. */

typedef int win_t;              /* a window's handle; -1: none */

typedef enum {
    WIN_EV_NONE = 0,
    WIN_EV_POINTER_DOWN,
    WIN_EV_POINTER_UP,
    WIN_EV_POINTER_MOVE,
    WIN_EV_KEY_DOWN,            /* reserved: keys come through keyseq (above) */
    WIN_EV_KEY_UP,
    WIN_EV_FOCUS_GAINED,        /* reserved for the view framework (71.4) */
    WIN_EV_FOCUS_LOST,
    WIN_EV_CLOSE_REQUEST,
    WIN_EV_RESIZE,
} win_event_type_t;

#define WIN_EVF_TAP   0x01u     /* the DOWN / UP pair of a recognised tap */
#define WIN_EVF_PRESS 0x02u     /* press feedback: a finger went down at (x, y) (DOWN) and
                                 * lifted or moved off (UP); a tap it made follows as its
                                 * own TAP pair, before this UP. Not for claimed windows. */

typedef struct {
    uint8_t  type;      /* win_event_type_t */
    uint8_t  button;    /* 1: primary touch / left button */
    uint8_t  mods;
    uint8_t  flags;     /* WIN_EVF_* */
    int16_t  x;         /* content coordinates (pixels) */
    int16_t  y;
    int16_t  dx;        /* since the previous pointer event of the contact */
    int16_t  dy;
    uint32_t key;
    uint16_t w;
    uint16_t h;
} win_event_t;

#define WIN_CLAIM_POINTER 0x01u /* the raw pointer stream, and no gestures, while focused */

/* The next event for window w, the console's input pumped first (so taps and
 * close requests are delivered). False when there is none. */
bool window_poll(win_t w, win_event_t *ev);

/* Producers (the input path, the touch task): queue ev for w; dropped when
 * the ring is full, except that a MOVE merges into an unread MOVE. */
void window_post(win_t w, const win_event_t *ev);

/* Forget w's unread events. */
void window_clear(win_t w);

/* What w takes over from the window manager (WIN_CLAIM_*); 0 gives it back.
 * A program sets it once it has its window and clears it when it ends. */
void    window_claim(win_t w, uint8_t claims);
uint8_t window_claims(win_t w);

/* The focused window's handle when it is a canvas window, else -1. */
win_t window_focused(void);

/* The handle of the canvas called name, or -1. */
win_t window_by_name(const char *name);

/* Where window w's content begins on the screen; false when it is not
 * visible. Pointer events for it are made relative to this. */
bool window_origin(win_t w, int *x0, int *y0);

#endif /* LUGALOS_KERNEL_EVENT_H */
