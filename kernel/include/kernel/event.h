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
    WIN_EV_KEY_DOWN,            /* a view's key: key / mods as kernel/keyseq.h decodes them
                                 * (canvas programs read theirs through keyseq instead) */
    WIN_EV_KEY_UP,
    WIN_EV_FOCUS_GAINED,        /* the window has the keyboard focus now */
    WIN_EV_FOCUS_LOST,
    WIN_EV_CLOSE_REQUEST,
    WIN_EV_RESIZE,              /* the window's content is w x h now (a rotation, a new width) */
    WIN_EV_DRAG,                /* a view's: a finger dragging, dy px since the last (down +) */
    WIN_EV_DRAG_END,            /* it lifted: dy over its last `key` ms (a fling when moving) */
    WIN_EV_SCROLL,              /* a vertical swipe: dy, over `key` ms (0: a step) */
    WIN_EV_SUSPEND,             /* off the screen (scrolled away, the panel blanked) */
    WIN_EV_RESUME,              /* on the screen again */
    /* The system's state, told to every canvas window and view when it
     * changes (kernel/console.c): */
    WIN_EV_INPUT_MODE,          /* key: what is attached, WIN_INPUT_* (a keyboard came or went,
                                 * a mouse; the touch screen is always there where there is one) */
    WIN_EV_ORIENT,              /* the display turned: key its rotation in degrees (0, 90, 180,
                                 * 270); each window's RESIZE follows */
    WIN_EV_POWER,               /* the battery: key its charge in % (0xFF unknown), mods
                                 * WIN_POWER_*; at each 10 % step and each change of those */
    WIN_EV_TILT,                /* with WIN_CLAIM_TILT, at most 10 Hz while the window has the
                                 * focus: gravity in the screen's frame, mg -- x toward its
                                 * right, y toward its bottom, dy toward its back */
} win_event_type_t;

#define WIN_INPUT_KEYBOARD 0x01u
#define WIN_INPUT_POINTER  0x02u     /* a mouse */
#define WIN_INPUT_TOUCH    0x04u

#define WIN_POWER_CHARGING 0x01u
#define WIN_POWER_LOW      0x02u     /* below the warning level: save, and expect the cut-off */

/* The window's state, not something that happened in it: never dropped from
 * a full ring (the oldest event goes instead). A full-screen game pauses
 * from FOCUS_LOST or SUSPEND until FOCUS_GAINED and RESUME. */
static inline bool win_event_is_state(uint8_t type) {
    return type == WIN_EV_FOCUS_GAINED || type == WIN_EV_FOCUS_LOST ||
           type == WIN_EV_SUSPEND || type == WIN_EV_RESUME || type == WIN_EV_CLOSE_REQUEST ||
           type == WIN_EV_RESIZE || type == WIN_EV_INPUT_MODE || type == WIN_EV_ORIENT ||
           type == WIN_EV_POWER;
}

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
#define WIN_CLAIM_TILT    0x02u /* WIN_EV_TILT while focused (boards with an IMU) */

/* The next event for window w, the console's input pumped first (so taps and
 * close requests are delivered). False when there is none. */
bool window_poll(win_t w, win_event_t *ev);

/* Producers (the input path, the touch task): queue ev for w; dropped when
 * the ring is full, except that a MOVE merges into an unread MOVE and a TILT
 * replaces an unread TILT. */
void window_post(win_t w, const win_event_t *ev);

/* Forget w's unread events. */
void window_clear(win_t w);

/* What w takes over from the window manager (WIN_CLAIM_*); 0 gives it back.
 * A program sets it once it has its window and clears it when it ends. */
void    window_claim(win_t w, uint8_t claims);
uint8_t window_claims(win_t w);

/* Whether a program has polled w: then closing its window asks it first
 * (WIN_EV_CLOSE_REQUEST, the console's close protocol); a window nobody
 * reads closes at once. window_forget(): its window is gone -- its unread
 * events too, so the slot's next window starts empty. */
bool window_listened(win_t w);
/* Whether w's program has read the last close request posted to it: it is
 * answering, only slowly (a game shutting its sound down). */
bool window_close_read(win_t w);
void window_forget(win_t w);

/* The focused window's handle when it is a canvas window, else -1. */
win_t window_focused(void);

/* The handle of the canvas called name, or -1. */
win_t window_by_name(const char *name);

/* Where window w's content begins on the screen; false when it is not
 * visible. Pointer events for it are made relative to this. */
bool window_origin(win_t w, int *x0, int *y0);

#endif /* LUGALOS_KERNEL_EVENT_H */
