#ifndef LUGALOS_DRIVERS_RIBBON_H
#define LUGALOS_DRIVERS_RIBBON_H

#include <stdbool.h>
#include <stdint.h>

/* Scrollable Tiling Window System (Niri-inspired).
 *
 * Windows are organized in a 1D non-overlapping ribbon along its major axis:
 * across the screen (RIBBON_AXIS_X, landscape) or down it (RIBBON_AXIS_Y, a
 * portrait screen, 76.3). A window's `size` is counted along that axis --
 * text columns across (38, 48, 64 or 98 at 800 px; 49, 62, 82 or 126 at
 * 1024), text rows down (one window, or a top/bottom pair) -- and it spans
 * m0..m1 there. The viewport m_view slides along the axis to bring the
 * active window into view without overlapping. Down a portrait screen every
 * window has the full width.
 *
 * Across a landscape screen the ribbon is a row of columns, and a column is
 * one window or a stack of them, one above the other (Niri's columns): a
 * window flagged RIBBON_FLAG_STACKED is in the column of the window before
 * it. wins[] is ordered column by column, top to bottom within one. A
 * column's windows share its first one's size and m0..m1; their rows are
 * shared out by ribbon_win_rows(). Down a portrait screen the flags are kept
 * but not laid out -- every window is its own -- so turning back restores
 * the stacks; there they stay with the places in wins[], not the windows.
 */

#define RIBBON_MAX_WINDOWS   8u
#define RIBBON_GAP           10  /* 10 px gap between adjacent window frames */
#define RIBBON_TITLE_MAX     32u /* a window title's bytes, with the NUL */

/* D7, 47.6: the preset widths, from
 * the screen width `w` instead of 800-px constants.
 *
 * A window `c` columns wide is a frame 8c + 7 px wide; the first starts at
 * RIBBON_BORDER_PAD, windows are RIBBON_GAP apart, and the last leaves 2 px
 * of shadow and the pad. So one window fills the screen at w/8 - 2 columns,
 * and two side by side share exactly w/8 - 4 (the "pair"): text and canvas
 * in a split always add up to it. The presets are shares of the pair:
 *
 *                            800 px   1024 px
 *   FULL        w/8 - 2         98       126   one window, the whole screen
 *   TWO_THIRDS  2 * pair / 3    64        82   the narrow canvas's text side
 *   HALF        pair / 2        48        62   two equal halves
 *   TWO_FIFTHS  2 * pair / 5    38        49   the wide canvas's text side
 *
 * Exactly today's 98/64/48/38 at 800, which is what the RP2350 must keep.
 * Macros, not functions: lcdterm's U-mode code uses them, and an out-of-line
 * copy in the kernel's .text would be outside its domain. */
#include "lugalos_config.h"

#ifndef FONT_CELL_W
#if defined(CONFIG_FONT_CELL_W) && (CONFIG_FONT_CELL_W == 16)
#define FONT_CELL_W 16u
#else
#define FONT_CELL_W 8u
#endif
#endif

#ifndef FONT_CELL_H
#if defined(CONFIG_FONT_CELL_H) && (CONFIG_FONT_CELL_H == 32)
#define FONT_CELL_H 32u
#else
#define FONT_CELL_H 16u
#endif
#endif

#define RIBBON_BORDER_PAD          (FONT_CELL_W == 16u ? 12 : 4)

/* A terminal is never narrower than this (ribbon_layout() widens it). A
 * split's text share below it is raised to it, and the canvas gets what is
 * left of the pair, or the split is refused: only on screens far smaller
 * than either panel -- at 800 and 1024 the smallest share is 38. */
#define RIBBON_TERM_MIN_COLS       (FONT_CELL_W == 16 ? 12u : 20u)
#define RIBBON_COLS_FULL(w)        ((uint8_t)((unsigned)(w) / (unsigned)FONT_CELL_W - 2u))
#define RIBBON_COLS_PAIR(w)        ((unsigned)(w) / (unsigned)FONT_CELL_W - 4u)
#define RIBBON_COLS_TWO_THIRDS(w)  ((uint8_t)(2u * RIBBON_COLS_PAIR(w) / 3u))
#define RIBBON_COLS_HALF(w)        ((uint8_t)(RIBBON_COLS_PAIR(w) / 2u))
#define RIBBON_COLS_TWO_FIFTHS(w)  ((uint8_t)(2u * RIBBON_COLS_PAIR(w) / 5u))

typedef enum {
    RIBBON_WIN_EMPTY    = 0,
    RIBBON_WIN_TERM     = 1,
    RIBBON_WIN_CANVAS   = 2,
    RIBBON_WIN_READER   = 3,
    RIBBON_WIN_LAUNCHER = 4,
    RIBBON_WIN_VIEWTEST = 5,   /* `viewtest` (dev builds, drivers/viewtest.c) */
    RIBBON_WIN_LISPUI   = 6,   /* a Lisp program's `ui-` window (drivers/lispui_view.c) */
} ribbon_win_type_t;

enum {
    RIBBON_FLAG_LOCKED    = (1u << 0),
    RIBBON_FLAG_DIRTY     = (1u << 1),
    RIBBON_FLAG_NO_CLOSE  = (1u << 2),   /* no close box (the launcher: always there) */
    RIBBON_FLAG_STACKED   = (1u << 3),   /* in the column of the window before it */
    RIBBON_FLAG_COL_FOCUS = (1u << 4),   /* the window its column last had focused */
    RIBBON_FLAG_FULLSCREEN = (1u << 5),  /* covers the screen while it has the focus (Cmd+Shift+F) */
};

/* A stacked window's height preset (ribbon_win_t.share): of the rows its
 * column's windows share; the others share the rest equally. */
#define RIBBON_SHARE_EQUAL      0u
#define RIBBON_SHARE_THIRD      1u
#define RIBBON_SHARE_HALF       2u
#define RIBBON_SHARE_TWO_THIRDS 3u
#define RIBBON_STACK_MAX        3u   /* windows in a column at most (fewer where rows are short) */
#define RIBBON_NONE             0xFFu

/* The vertical ribbon is for a screen that turns (the Tab5). The RP2350 and
 * the C6 never use it: there the axis is X by definition and its fields are
 * not built. */
#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32C6)
#define RIBBON_AXIS_Y_SUPPORTED 0
#else
#define RIBBON_AXIS_Y_SUPPORTED 1
#endif

#define RIBBON_AXIS_X 0u
#define RIBBON_AXIS_Y 1u

#define RIBBON_ROWS_MIN 4u   /* a window down the screen is never shorter */

typedef struct {
    uint8_t  type;       /* ribbon_win_type_t */
    uint8_t  vterm_id;   /* for RIBBON_WIN_TERM: associated vterm ID */
    uint8_t  size;       /* along the major axis: text columns (X) or rows (Y) */
    uint8_t  flags;      /* RIBBON_FLAG_* */
    int16_t  owner_pid;  /* owning process PID, or -1 if unowned/kernel */
    uint8_t  share;      /* in a stack: RIBBON_SHARE_* */
    uint8_t  x_size;     /* down a portrait screen: its column's width across, for the turn back */
    int32_t  m0;         /* the frame's first pixel along the major axis, in ribbon coordinates */
    int32_t  m1;         /* ... and its last (the 2 px shadow follows) */
    char     title[RIBBON_TITLE_MAX];  /* window title */
} ribbon_win_t;

typedef struct {
    ribbon_win_t wins[RIBBON_MAX_WINDOWS];
    uint8_t      count;         /* number of active windows (0..RIBBON_MAX_WINDOWS) */
    uint8_t      active_idx;    /* index of active (focused) window */
    uint8_t      y_full;        /* down the screen: rows of a window alone on it */
#if RIBBON_AXIS_Y_SUPPORTED
    uint8_t      axis;          /* RIBBON_AXIS_X | RIBBON_AXIS_Y */
#endif
    int32_t      m_view;        /* the viewport's offset along the major axis (px): screen = m - m_view */
    int32_t      m_target;      /* where an animation takes m_view (px) */
    uint16_t     screen_w;      /* screen width in pixels (e.g. 800) */
    uint16_t     screen_h;      /* screen height in pixels (e.g. 480) */
    uint16_t     y_pad0;        /* down the screen: where a window's frame starts (below the menu bar) */
    uint16_t     y_extra;       /* down the screen: a window's pixels besides its rows (title bar, borders) */
} ribbon_t;

#if RIBBON_AXIS_Y_SUPPORTED
#define RIBBON_AXIS(r) ((r)->axis)
#else
#define RIBBON_AXIS(r) ((void)(r), RIBBON_AXIS_X)
#endif

/* Window types from RIBBON_WIN_READER up are views (drivers/view.h): the
 * kernel's own windows, drawn into a backing slot, each type a kind of view. */
static inline int ribbon_is_view(uint8_t type) {
    return type >= RIBBON_WIN_READER;
}

/* Initialize ribbon structure (axis X) */
void ribbon_init(ribbon_t *r, uint16_t screen_w, uint16_t screen_h);

/* The major axis, and the geometry of a window down the screen (the
 * screen's: where windows start, the pixels a window has besides its rows,
 * the rows of one window alone) -- the portrait ribbon's sizes and a
 * landscape stack's rows. Sizes are the caller's to convert. False for Y
 * where Y is not built (the geometry is still taken). */
bool ribbon_set_axis(ribbon_t *r, uint8_t axis, uint16_t y_pad0, uint16_t y_extra, uint8_t y_full);

/* Sizes along the major axis: one window alone (full), two side by side or
 * one above the other (pair: their sizes add up to it), and half a pair. */
uint8_t ribbon_size_full(const ribbon_t *r);
uint8_t ribbon_size_pair(const ribbon_t *r);
uint8_t ribbon_size_half(const ribbon_t *r);

/* Recompute m0 and m1 for all windows in sequence */
void ribbon_layout(ribbon_t *r);

/* Insert a new window at idx (0..count). If idx >= count, appends at count.
 * Returns the index where the window was placed, or -1 if full. */
int ribbon_insert(ribbon_t *r, uint8_t idx, uint8_t type, uint8_t vterm_id, uint8_t size, const char *title);

/* Remove window at idx. Returns true on success. */
bool ribbon_remove(ribbon_t *r, uint8_t idx);

/* Swap windows at idx_a and idx_b (the stacking stays with the places). */
bool ribbon_swap(ribbon_t *r, uint8_t idx_a, uint8_t idx_b);

/* Move window at from_idx left (dir < 0) or right (dir > 0) -- across a
 * landscape screen its whole column, past the next column. Updates
 * active_idx to track the window. */
bool ribbon_move(ribbon_t *r, uint8_t from_idx, int dir);

/* Focus window at idx, updating m_target to bring it into comfortable view. */
bool ribbon_focus(ribbon_t *r, uint8_t idx);

/* Focus left (dir < 0) or right (dir > 0) neighbor: across a landscape
 * screen the next column, at the window it last had focused. */
bool ribbon_focus_step(ribbon_t *r, int dir);

/* Resize the active window: expand (true) or shrink (false) across the
 * presets -- 38/48/64/98 columns at 800 px; in Y, half and full. */
bool ribbon_resize_active(ribbon_t *r, bool expand);

/* Set title of window at idx. */
void ribbon_set_title(ribbon_t *r, uint8_t idx, const char *title);

/* Set/get process owner PID for window at idx. */
void    ribbon_set_owner_pid(ribbon_t *r, uint8_t idx, int16_t pid);
int16_t ribbon_get_owner_pid(const ribbon_t *r, uint8_t idx);

/* The viewport offset that shows window idx whole, as near current as can be. */
int32_t ribbon_compute_viewport(const ribbon_t *r, uint8_t idx, int32_t current);

/* The whole ribbon's length along its axis (px, with its padding); at least the screen's. */
int32_t ribbon_total_len(const ribbon_t *r);

/* Find window index containing given vterm_id, or -1 if not found. */
int ribbon_find_vterm(const ribbon_t *r, uint8_t vterm_id);

/* Find first window index of type RIBBON_WIN_TERM, or -1 if none. */
int ribbon_find_term(const ribbon_t *r);

/* Find first window index of type RIBBON_WIN_CANVAS, or -1 if none. */
int ribbon_find_canvas(const ribbon_t *r);

/* Find first window index of type RIBBON_WIN_READER, or -1 if none. */
int ribbon_find_reader(const ribbon_t *r);

/* Find first window index of type RIBBON_WIN_LAUNCHER, or -1 if none. */
int ribbon_find_launcher(const ribbon_t *r);

/* Find window index of type RIBBON_WIN_CANVAS matching canvas_id slot, or -1 if none. */
int ribbon_find_canvas_slot(const ribbon_t *r, uint8_t canvas_id);

/* Find window index of type RIBBON_WIN_READER matching reader_id slot, or -1 if none. */
int ribbon_find_reader_slot(const ribbon_t *r, uint8_t reader_id);

/* Identify range of windows intersecting the viewport at `view`.
 * Returns count of visible windows, writing indices to *first_idx and *last_idx if found. */
int ribbon_visible_range(const ribbon_t *r, int32_t view, uint8_t *first_idx, uint8_t *last_idx);

/* Perform one animation step moving m_view towards m_target.
 * Returns true if m_view changed (needs redraw), false if already at target. */
bool ribbon_step_scroll(ribbon_t *r, int32_t step_px);

/* The active window's next width preset, from the widest back to the
 * narrowest (in Y: alone and half of a pair). False when there is one only. */
bool ribbon_cycle_active(ribbon_t *r);

/* Toggle the active window between full and half (one-up and a pair). */
bool ribbon_toggle_maximize(ribbon_t *r);

/* --- Columns and stacks (across a landscape screen) ------------------------ */

/* The first window of idx's column, and one past its last. Down a portrait
 * screen every window is a column of its own. */
uint8_t ribbon_col_first(const ribbon_t *r, uint8_t idx);
uint8_t ribbon_col_end(const ribbon_t *r, uint8_t idx);

/* How many windows a column may hold here: as many as get RIBBON_ROWS_MIN
 * rows each, at most RIBBON_STACK_MAX; 1 down a portrait screen. */
uint8_t ribbon_stack_max(const ribbon_t *r);

/* Window idx's frame down the screen (its first and last pixel row) and its
 * text rows, across a landscape screen: the whole height alone in its
 * column, its share in a stack. `solo` is a window that has its column to
 * itself for now (the on-screen keyboard's), or RIBBON_NONE: the others in
 * its column get 0 rows. False down a portrait screen (the frame is m0..m1)
 * or for no such window. */
bool ribbon_win_rows(const ribbon_t *r, uint8_t idx, uint8_t solo, int32_t *y0, int32_t *y1, uint8_t *rows);

/* Cmd+[ / Cmd+] (Niri's consume-or-expel): the active window, alone in its
 * column, joins the column left (dir < 0) or right (dir > 0) at its bottom;
 * in a stack, it leaves it for a column of its own on that side. False down
 * a portrait screen, at the ribbon's end, or into a full column. */
bool ribbon_join(ribbon_t *r, int dir);

/* Cmd+Up / Down: the focus to the window above / below in the stack. */
bool ribbon_focus_stack(ribbon_t *r, int dir);

/* Cmd+Ctrl+Up / Down: the active window up / down within its stack. */
bool ribbon_move_stack(ribbon_t *r, int dir);

/* The active window's height preset in its stack: -1 the next smaller, 1 the
 * next larger, 2 the next one round (from the largest to the smallest).
 * False alone in its column, or when nothing changes. */
bool ribbon_height(ribbon_t *r, int dir);

/* Standalone selftest */
void ribbon_selftest(void);

#endif /* LUGALOS_DRIVERS_RIBBON_H */
