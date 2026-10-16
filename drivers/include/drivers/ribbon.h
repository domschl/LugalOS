#ifndef LUGALOS_DRIVERS_RIBBON_H
#define LUGALOS_DRIVERS_RIBBON_H

#include <stdbool.h>
#include <stdint.h>

/* Horizontal Scrollable Tiling Window System (Niri-inspired).
 *
 * Windows are organized in a 1D non-overlapping horizontal ribbon.
 * Each window has a column width from the table below: 38, 48, 64 or 98
 * columns on the RP2350's 800 px, 49, 62, 82 or 126 on the LCD-7B's 1024.
 * The viewport x_view slides horizontally across the ribbon to bring active
 * windows into view without overlapping.
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
    RIBBON_WIN_EMPTY  = 0,
    RIBBON_WIN_TERM   = 1,
    RIBBON_WIN_CANVAS = 2,
    RIBBON_WIN_READER = 3,
} ribbon_win_type_t;

enum {
    RIBBON_FLAG_LOCKED = (1u << 0),
    RIBBON_FLAG_DIRTY  = (1u << 1),
};

typedef struct {
    uint8_t  type;       /* ribbon_win_type_t */
    uint8_t  vterm_id;   /* for RIBBON_WIN_TERM: associated vterm ID */
    uint8_t  cols;       /* width in text columns (e.g. 38, 48, 64, 98) */
    uint8_t  flags;      /* RIBBON_FLAG_* */
    int32_t  rx0;        /* ribbon horizontal left frame coordinate (px) */
    int32_t  rx1;        /* ribbon horizontal right frame coordinate (px) */
    char     title[RIBBON_TITLE_MAX];  /* window title */
} ribbon_win_t;

typedef struct {
    ribbon_win_t wins[RIBBON_MAX_WINDOWS];
    uint8_t      count;         /* number of active windows (0..RIBBON_MAX_WINDOWS) */
    uint8_t      active_idx;    /* index of active (focused) window */
    int32_t      x_view;        /* current viewport horizontal offset (px) */
    int32_t      x_target;      /* target viewport horizontal offset for animation (px) */
    uint16_t     screen_w;      /* screen width in pixels (e.g. 800) */
    uint16_t     screen_h;      /* screen height in pixels (e.g. 480) */
} ribbon_t;

/* Initialize ribbon structure */
void ribbon_init(ribbon_t *r, uint16_t screen_w, uint16_t screen_h);

/* Recompute rx0 and rx1 for all windows in sequence */
void ribbon_layout(ribbon_t *r);

/* Insert a new window at idx (0..count). If idx >= count, appends at count.
 * Returns the index where the window was placed, or -1 if full. */
int ribbon_insert(ribbon_t *r, uint8_t idx, uint8_t type, uint8_t vterm_id, uint8_t cols, const char *title);

/* Remove window at idx. Returns true on success. */
bool ribbon_remove(ribbon_t *r, uint8_t idx);

/* Swap windows at idx_a and idx_b. */
bool ribbon_swap(ribbon_t *r, uint8_t idx_a, uint8_t idx_b);

/* Move window at from_idx left (dir < 0) or right (dir > 0).
 * Updates active_idx to track the window. */
bool ribbon_move(ribbon_t *r, uint8_t from_idx, int dir);

/* Focus window at idx, updating x_target to bring it into comfortable view. */
bool ribbon_focus(ribbon_t *r, uint8_t idx);

/* Focus left (dir < 0) or right (dir > 0) neighbor. */
bool ribbon_focus_step(ribbon_t *r, int dir);

/* Resize the active window: expand (true) or shrink (false) across 38/48/64/98 presets. */
bool ribbon_resize_active(ribbon_t *r, bool expand);

/* Set title of window at idx. */
void ribbon_set_title(ribbon_t *r, uint8_t idx, const char *title);

/* Calculate ideal viewport x offset so window idx is visible on screen. */
int32_t ribbon_compute_viewport(const ribbon_t *r, uint8_t idx, int32_t current_x);

/* Total width in pixels of the entire ribbon content (including padding). */
int32_t ribbon_total_width(const ribbon_t *r);

/* Find window index containing given vterm_id, or -1 if not found. */
int ribbon_find_vterm(const ribbon_t *r, uint8_t vterm_id);

/* Find first window index of type RIBBON_WIN_TERM, or -1 if none. */
int ribbon_find_term(const ribbon_t *r);

/* Find first window index of type RIBBON_WIN_CANVAS, or -1 if none. */
int ribbon_find_canvas(const ribbon_t *r);

/* Find first window index of type RIBBON_WIN_READER, or -1 if none. */
int ribbon_find_reader(const ribbon_t *r);

/* Find window index of type RIBBON_WIN_CANVAS matching canvas_id slot, or -1 if none. */
int ribbon_find_canvas_slot(const ribbon_t *r, uint8_t canvas_id);

/* Find window index of type RIBBON_WIN_READER matching reader_id slot, or -1 if none. */
int ribbon_find_reader_slot(const ribbon_t *r, uint8_t reader_id);

/* Identify range of windows intersecting visible screen viewport [x_view, x_view + screen_w].
 * Returns count of visible windows, writing indices to *first_idx and *last_idx if found. */
int ribbon_visible_range(const ribbon_t *r, int32_t x_view, uint8_t *first_idx, uint8_t *last_idx);

/* Perform one animation step moving x_view towards x_target.
 * Returns true if x_view changed (needs redraw), false if already at target. */
bool ribbon_step_scroll(ribbon_t *r, int32_t step_px);

/* Toggle active window width between FULL and HALF presets. */
bool ribbon_toggle_maximize(ribbon_t *r);

/* Standalone selftest */
void ribbon_selftest(void);

#endif /* LUGALOS_DRIVERS_RIBBON_H */
