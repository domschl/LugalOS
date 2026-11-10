#ifndef LUGALOS_DRIVERS_SCREEN_H
#define LUGALOS_DRIVERS_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/canvas1.h"
#include "drivers/vtterm.h"
#include "drivers/ribbon.h"

/* The whole 1-bpp screen as the terminal sees it -- 37.1/37.1a/37.3b,
 * : a menu bar, the grey
 * desktop, and one or two framed tiles with striped title bars -- the
 * terminal, a canvas, or both side by side.
 *
 * Portable and hardware-free like vtterm.c, so `vtselftest` checks it on
 * QEMU. On the RP2350-LCD-7 it lives in the `lcdterm` task's own state block
 * and is driven only by that task (drivers/lcd7_rp2350.c); on the other
 * targets a RAM screen (drivers/ramscreen.c) runs the same code, so that
 * Lisp's canvas can be tested there.
 *
 * The geometry, in pixels (§1.2), for a screen of w x h, C = w/8 cells:
 *
 *   menu bar     y 0..19, white, a rule at y 19; the system name in bold
 *                from x 8, the indicators ending 16 px from the right edge
 *   desktop      everything else not covered, 50 % grey
 *   tiles        a frame from y 22 to 40 + 16*rows + 1, a 1-px shadow
 *                right and below, a 17-row title bar at the top: border,
 *                a white row, six stripes, a white row, border, the title
 *                centred in bold
 *   text         from y 40, rows = (h - 43) / 16, on the 8-px grid
 *
 *   layout       text tile (frame x)          canvas tile (frame x)
 *   TEXT         4 .. 8(C-1)+2, cols C-2      --
 *   CANVAS       -- (text hidden)             4 .. 8(C-1)+2
 *   SPLIT_WIDE   8k-4 .. 8(C-1)+2, 38 cols    4 .. 8k-14,   k = C-39
 *   SPLIT_HALF   the same with 48 cols        4 .. 8k-14,   k = C-49
 *   SPLIT_NARROW the same with 64 cols        4 .. 8k-14,   k = C-65  (37.5a)
 *
 * Swapped (37.5a), a split puts the text tile on the left -- its frame
 * x 4 .. 8(cols+1)+2 -- and the canvas 10 px right of it to x 8(C-1)+2, so
 * the canvas is exactly as wide as unswapped.
 *
 * On the 800 x 480 panel: 98 x 27 or 38 x 27 or 48 x 27 cells; a canvas of
 * 789, 469 or 389 x 434 px. Nothing is drawn on x 799 (under the bezel).
 *
 * **The canvas is stored once per layout, where there is memory for it**
 *). A layout change saves
 * the canvas it leaves and restores the one it enters -- without bumping
 * `damage` -- if nothing has been drawn since that one was saved; otherwise,
 * and always without a store, the canvas comes back blank and `damage`
 * moves on, and whoever draws on it redraws (§2.1). A repaint clears it
 * too. The text is stored, in the cell shadow, and survives everything: in
 * CANVAS output still lands in the shadow, and a narrower window keeps each
 * row's left part. */

#define CHROME_SCALE         ((FONT_CELL_W == 16u) ? 2 : 1)
#define SCREEN_MENU_H        (20 * CHROME_SCALE)
#define SCREEN_TILE_Y        (22 * CHROME_SCALE)
#define SCREEN_TITLEBAR_H    (17 * CHROME_SCALE)
#define CHROME_CLOSE_X0(vx0) ((int32_t)(vx0) - 2 * (int32_t)CHROME_SCALE)
#define CHROME_CLOSE_X1(vx0) ((int32_t)(vx0) + 32 * (int32_t)CHROME_SCALE)
#define CHROME_CLOSE_Y0      ((int32_t)SCREEN_TILE_Y - 2 * (int32_t)CHROME_SCALE)
#define CHROME_CLOSE_Y1      ((int32_t)(SCREEN_TILE_Y + SCREEN_TITLEBAR_H) + 4 * (int32_t)CHROME_SCALE)
#define SCREEN_TEXT_X        (FONT_CELL_W)
#define SCREEN_TEXT_Y        (40 * CHROME_SCALE)
#define SCREEN_TEXT_COLS(w)  ((unsigned)(w) / (unsigned)FONT_CELL_W - 2u)
#define SCREEN_TEXT_ROWS(h)  (((unsigned)(h) > (unsigned)(SCREEN_TEXT_Y + 3 * CHROME_SCALE)) ? (((unsigned)(h) - (unsigned)SCREEN_TEXT_Y - 3u * (unsigned)CHROME_SCALE) / (unsigned)FONT_CELL_H) : 1u)

#define SCREEN_RIGHT_MAX  24u           /* bytes of UTF-8, with the NUL */
#define SCREEN_NAME_MAX   16u
#define SCREEN_CTITLE_MAX 32u

enum {
    SCREEN_LAYOUT_TEXT = 0,
    SCREEN_LAYOUT_CANVAS = 1,
    SCREEN_LAYOUT_SPLIT_WIDE = 2,
    SCREEN_LAYOUT_SPLIT_HALF = 3,
    SCREEN_LAYOUT_SPLIT_NARROW = 4,     /* 37.5a: a narrow canvas, 64 text columns */
    SCREEN_LAYOUT_FULLSCREEN = 5,       /* Borderless full-screen canvas (no menu bar, no titlebar) */
};

#define SCREEN_MAX_TERMS 8u
#define SCREEN_MAX_CANVASES 4u
#define SCREEN_MAX_READERS  4u
#define SCREEN_STORES       (SCREEN_MAX_CANVASES + SCREEN_MAX_READERS)

enum {
    CANVAS_FMT_1BPP   = 0,
    CANVAS_FMT_RGB565 = 1,
};

typedef struct {
    canvas1_t backing;                  /* persistent backing store in PSRAM */
    uint16_t *buf565;                   /* RGB565 pixel buffer in PSRAM (NULL if 1-bpp) */
    uint32_t  stride565;                /* stride in uint16_t pixels (width) */
    uint16_t  w565, h565;               /* dimensions of RGB565 buffer */
    uint8_t   format;                   /* CANVAS_FMT_1BPP or CANVAS_FMT_RGB565 */
    uint8_t   vterm_id;                 /* the terminal drawing on it: its creator, or the last to
                                         * select it by name ('L') */
    uint32_t  used_seq;                 /* when its terminal last selected it ('L'): of a terminal's
                                         * canvases, the latest is the one its requests mean */
    bool      in_use;
    bool      locked;                   /* canvas layout locked for this slot */
    bool      buf565_owned;             /* true if buf565 was allocated by screen.c and should be freed by it */
    bool      fullscreen;               /* asked for SCREEN_LAYOUT_FULLSCREEN: the layout while it has the focus */
    bool      detached;                 /* its window closed while its program still ran ('q' 0): freed when
                                         * the program lets go of it ('L' text) or ends ('q' 1) */
    uint16_t  drawn_w;                  /* the window width `backing` was drawn at; 0: none */
    char      name[SCREEN_CTITLE_MAX];  /* unique canvas name (e.g. "plot", "chess", "map") */
    char      title[SCREEN_CTITLE_MAX]; /* canvas window title */
} screen_canvas_slot_t;

typedef struct {
    canvas1_t backing;                  /* persistent backing store in PSRAM */
    bool      in_use;
    char      path[128];                /* document path / URL */
    char      title[SCREEN_CTITLE_MAX]; /* reader window title */
} screen_reader_slot_t;

#define SCREEN_STATUS_SD    (1u << 0)
#define SCREEN_STATUS_P9    (1u << 1)
#define SCREEN_STATUS_WIFI  (1u << 2)

typedef struct {
    canvas1_t cv;                       /* the whole buffer */
    canvas1_t cc;                       /* the canvas tile's drawable area */
    canvas1_t c_backing;                /* persistent backing store for primary canvas */
    uint8_t   layout;
    uint8_t   full_cols;                /* the text window's width in TEXT */
    uint8_t   cap_cols;                 /* every terminal's shadow: columns ... */
    uint8_t   cap_rows;                 /* ... and rows (SCREEN_SHADOW_*; slot t at t * cols * rows) */
    uint8_t   fs_overlay;               /* over a full-screen window: SCREEN_FS_HINT | SCREEN_FS_REVEAL */
    uint8_t   fs_told;                  /* SCREEN_FS_ON: full screen when the console last looked ('H' 3) */
    uint32_t  fs_until_ms;              /* when the overlay goes */
    char      fs_hint[64];              /* the hint's text */
    uint8_t   locked;                   /* 37.5a: 'w' and 'X' may not change the split */
    uint8_t   swapped;                  /* 37.5a: the canvas on the right, text left */
    uint16_t  damage;                   /* bumped whenever the canvas is lost */
    uint8_t   store_ok;                 /* 38.8: bit L-1 -- layout L's store holds its canvas */
    uint8_t   redrawing;                /* 38.8: between 'D' 1 and 'D' 0 */
    uint8_t   status_flags;             /* Top bar indicators: SD mounted, 9P active, Wi-Fi connected */
    uint8_t   osk_visible;              /* 1 if the on-screen keyboard is out (screen_osk_in() says where) */
    uint8_t   osk_view_type, osk_view_id; /* the view it is out for (its text field's), type 0: a terminal */
    uint8_t   osk_modifiers;            /* Bit 0: shift, Bit 1: ctrl, Bit 2: sym */
    int16_t   osk_pressed;              /* the key under a finger (osk_key_at()), -1: none */
    uint8_t  *store;                    /* 38.8: SCREEN_STORES slots, or NULL */
    uint32_t  store_slot;               /* bytes per slot */
    uint32_t  draw_gen;                 /* bumped by every drawing request */
    uint32_t  store_gen[SCREEN_STORES]; /* draw_gen when each slot was saved */
    int16_t   fx0, fy0, fx1, fy1;       /* the text tile's frame */
    int16_t   cx0, cy0, cx1, cy1;       /* the canvas tile's frame */
    uint32_t  title_drawn;              /* vt.title_seq the title bar shows */
    char      name[SCREEN_NAME_MAX];    /* the menu bar's left */
    char      right[SCREEN_RIGHT_MAX];  /* its indicators */
    char      ctitle[SCREEN_CTITLE_MAX];/* the canvas tile's title */
    ribbon_t  ribbon;                   /* 1D horizontal window ribbon */
    screen_canvas_slot_t canvases[SCREEN_MAX_CANVASES]; /* concurrent canvas backing stores */
    uint32_t  canvas_seq;               /* the last used_seq given out */
    screen_reader_slot_t readers[SCREEN_MAX_READERS];   /* concurrent reader backing stores */
    vtterm_t  vt;                       /* legacy reference to active terminal */
    struct {
        vtterm_t vt;
        uint8_t  vterm_id;
        bool     in_use;
    } terms[SCREEN_MAX_TERMS];          /* up to 8 concurrent terminal window instances */
    bool      animating;
    uint16_t  shadow[];                 /* shadow buffers for all 8 terminals (8 * cols * rows) */
} screen_t;

/* Backing stores: SCREEN_MAX_CANVASES for canvases, plus SCREEN_MAX_READERS for readers. */
#define SCREEN_STORE_BYTES(w, h) (SCREEN_STORES * ((uint32_t)(w) / 8u) * (uint32_t)(h))

/* What a screen of w x h pixels needs, shadow included for all 4 terminals. */
#define SCREEN_BYTES(w, h) SCREEN_BYTES_ROT(w, h, false)

/* A terminal's shadow: the grid of the whole screen (a terminal full
 * screen has no chrome), or on a panel that turns (`rotates`: w and h trade
 * places at run time) the wider and the taller of both, so a turn never
 * indexes past a terminal's share into the next one. */
#define SCREEN_GRID_COLS(w) ((unsigned)(w) / (unsigned)FONT_CELL_W)
#define SCREEN_GRID_ROWS(h) ((unsigned)(h) / (unsigned)FONT_CELL_H)
#define SCREEN_SHADOW_COLS(w, h, rotates) \
    ((rotates) && SCREEN_GRID_COLS(h) > SCREEN_GRID_COLS(w) ? SCREEN_GRID_COLS(h) : SCREEN_GRID_COLS(w))
#define SCREEN_SHADOW_ROWS(w, h, rotates) \
    ((rotates) && SCREEN_GRID_ROWS(w) > SCREEN_GRID_ROWS(h) ? SCREEN_GRID_ROWS(w) : SCREEN_GRID_ROWS(h))

/* What shows over a full-screen window (screen_t.fs_overlay): the hint how
 * to leave it, and the menu bar with the window's title bar, revealed from
 * the top edge. Both in the top SCREEN_FS_BAND rows. */
#define SCREEN_FS_HINT    0x01u
#define SCREEN_FS_REVEAL  0x02u
#define SCREEN_FS_ON      0x80u     /* screen_fs_state(): full screen when last told */
#define SCREEN_FS_HINT_MS   2500u
#define SCREEN_FS_REVEAL_MS 4000u
#define SCREEN_FS_BAND    (SCREEN_TILE_Y + SCREEN_TITLEBAR_H)
#define SCREEN_BYTES_ROT(w, h, rotates) \
    (sizeof(screen_t) + SCREEN_MAX_TERMS * SCREEN_SHADOW_COLS(w, h, rotates) * \
     SCREEN_SHADOW_ROWS(w, h, rotates) * sizeof(uint16_t))

/* Takes over the whole buffer and draws everything: the TEXT layout, the
 * terminal empty, the tile titled `LugalOS` until something says otherwise.
 * `scr` must hold SCREEN_BYTES(w, h); w must be a multiple of 8. */
void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h);
/* The same for a panel that turns: `scr` must hold SCREEN_BYTES_ROT(w, h, rotates). */
void screen_init_rot(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h, bool rotates);
void screen_reorient(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h);

/* 38.8: memory for the canvas stores, SCREEN_STORE_BYTES(w, h) of it;
 * before this, or with NULL, nothing is stored. */
void screen_set_store(screen_t *scr, void *mem, uint32_t bytes);

/* Terminal output; redraws the title bar if it changed the title. */
void screen_write(screen_t *scr, const char *s, uint32_t n);
void screen_write_vterm(screen_t *scr, uint8_t vterm_id, const char *s, uint32_t n);

void screen_set_title(screen_t *scr, const char *s, uint32_t n);
void screen_set_right(screen_t *scr, uint8_t flags, const char *s, uint32_t n);

/* Everything again: the chrome, every text cell from the shadow, and the
 * canvas cleared (damage + 1). */
void screen_repaint(screen_t *scr);

/* Changes the layout, repainting everything; false (and nothing changed)
 * for an unknown layout or a split the screen is too narrow for. Asking for
 * the layout already showing changes nothing, the canvas included. */
bool screen_set_layout(screen_t *scr, unsigned layout);
bool screen_set_layout_vterm(screen_t *scr, uint8_t vterm_id, unsigned layout);
bool screen_set_layout_vterm_named(screen_t *scr, uint8_t vterm_id, unsigned layout, const char *name);
bool screen_set_layout_vterm_named_fmt(screen_t *scr, uint8_t vterm_id, unsigned layout, const char *name, uint8_t format, int16_t caller_pid);

/* The text window's size, which is what a program may use. */
void screen_text_size(const screen_t *scr, unsigned *cols, unsigned *rows);

/* --- The canvas protocol (37.3b) -------------------------------------------
 *
 * One request, one reply, the same bytes whether they cross the lcdterm
 * channel on the RP2350-LCD-7 or a function call on a RAM screen. A request
 * is an op byte and its arguments, little-endian int16 unless noted;
 * colours are 0 white, 1 black, 2 grey (any other value is black).
 *
 *   'L' u8 layout                       change layout
 *   'T' bytes                           the canvas tile's title (UTF-8)
 *   'S'                                 nothing: just the reply
 *   'F' c                               the whole canvas in colour c
 *   'p' x y c                           a pixel
 *   'l' x0 y0 x1 y1 c                   a line, both ends included
 *   'r' x y w h c                       a filled rectangle
 *   'o' x y w h c                       a rectangle's outline
 *   'c' x y r c                         a circle's outline
 *   'i' x y w h                         invert a rectangle
 *   't' x y bold(u8) bytes              text, black, the glyph's top-left at x y
 *   'b' x y n scale bits...             a row of n pixels from packed bits
 *   'g' x y                             nothing drawn; the reply has the pixel
 *   'w' s8 dir                          37.5a: the divider one step left (-1)
 *                                       or right (+1), through five places:
 *                                       text only (TEXT), the three splits,
 *                                       canvas only (CANVAS) -- so at its ends
 *                                       it closes a pane. With the canvas on
 *                                       the left, left means more text; when
 *                                       swapped, less. Refused at an end or
 *                                       while locked.
 *   'X'                                 37.5a: in a split, swap the panes
 *                                       (canvas left or right); with one pane
 *                                       full, show the other one full (TEXT <->
 *                                       CANVAS); refused while locked
 *   'K' u8 lock                         37.5a: 1 keeps the layout as it is
 *                                       ('w' and 'X' refused) until a 'K' 0 or
 *                                       a change to TEXT -- chess's board fits
 *                                       only the 38-column split
 *   'Z'                                 38.8: forget every stored canvas -- a
 *                                       program has ended, and the next must
 *                                       not be shown its pixels
 *   'E'                                 the screen against a full redraw of
 *                                       its state: [1] 1 if they differed,
 *                                       [12..13] the first and last band of
 *                                       16 rows that did (the full redraw is
 *                                       kept)
 *   'v' type flags [title]              a window for view `type` (ribbon_is_view())
 *                                       beside the focused one, full width,
 *                                       backed by a reader slot; flags
 *                                       SCREEN_VIEW_*. [1] the slot, [2..5]
 *                                       the content's size; 'U' is the
 *                                       launcher's (singleton, no close box)
 *   'y' x y | (none) | 'r'              the on-screen keyboard's key under
 *                                       screen point x y shows pressed; bare,
 *                                       none does ([1] 1 if that changed);
 *                                       'r': the pressed key's repeat code
 *                                       ([6..7], [8] 0xBB), if it repeats
 *   'u' u8 type u8 slot                 view `type`'s window on reader slot
 *                                       `slot` closed by its program; the
 *                                       reply as 'C''s
 *   'W' u8 slot                         a reader's or the launcher's backing
 *                                       changed: copied into its tile only
 *                                       (refused when no such window)
 *   'D' u8 on                           38.8: 1 -- what follows is a redraw
 *                                       of the program's picture after damage,
 *                                       not a change to it, so the other
 *                                       layouts' stores stay good; 0 ends it,
 *                                       as do a layout change and 'Z'. Without
 *                                       it, a redraw handler's drawing made
 *                                       every other store stale, and each
 *                                       resize redrew (the owner, Lorenz)
 *
 * The reply, SCREEN_REPLY_LEN bytes: [0] 0 done, 1 refused (no canvas in
 * this layout, or a bad request); [1] the pixel ('g'); [2..3] canvas width;
 * [4..5] canvas height (both 0 in TEXT); [6..7] damage; [8] layout; [9] 1 if
 * the panes are swapped. */
#define SCREEN_REPLY_LEN 32u     /* 'R' with an index puts the window's title at [12..31] */

#define SCREEN_VIEW_SINGLETON 0x01u     /* 'v': focus the type's window if it has one */
#define SCREEN_VIEW_NO_CLOSE  0x02u     /* 'v': no close box */

void screen_canvas(screen_t *scr, const uint8_t *req, uint32_t n, uint8_t *reply);
/* Each ribbon window's identity, focus and visibility now, to the console
 * (console_windows_now()). Kernel code, called under console_lock after a
 * request: in a full-screen layout only the focused window shows. */
void screen_report_windows(const screen_t *scr);
/* The on-screen keyboard's rectangle at the focused terminal (touch boards). */
bool screen_osk_rect(const screen_t *scr, int *x0, int *y0, int *w, int *h);
/* Each ribbon window's identity, focus and visibility now, to the console
 * (console_windows_now()). Kernel code, called under console_lock after a
 * request: in a full-screen layout only the focused window shows. */
void screen_report_windows(const screen_t *scr);
/* The window under screen point (x, y): its ribbon type, its id and the
 * point in its content; false off every window (the menu bar, the chrome),
 * and in full screen off the full-screen view (a canvas is reached through
 * its origin). The caller holds console_lock or accepts a layout
 * that changes under it (a pointer event one window off, once). */
bool screen_window_at(const screen_t *scr, int x, int y, uint8_t *type, uint8_t *id, int *cx, int *cy);
/* How many pixel rows at the bottom of window idx's content the on-screen
 * keyboard takes: its height while it is out there (the focused terminal's,
 * or the focused view's that asked for it, 'k'), else 0. */
int  screen_osk_rows(const screen_t *scr, unsigned idx);
/* Window idx's frame on the screen now (its border, title bar included):
 * across a landscape screen it spans the tile rows, down a portrait one
 * (RIBBON_AXIS_Y) the full width. Its content is inside the border, under
 * the title bar: its top-left pixel and size. Whether it is drawn: any of it
 * on screen across, all of it down. idx < ribbon.count. */
void screen_win_frame(const screen_t *scr, unsigned idx, int *x0, int *y0, int *x1, int *y1);
void screen_win_content(const screen_t *scr, unsigned idx, int *cx, int *cy, int *cw, int *ch);
bool screen_win_shown(const screen_t *scr, unsigned idx);
/* The focused window's ribbon type and id; false when there is none. */
bool screen_focused_window(const screen_t *scr, uint8_t *type, uint8_t *id);
/* Whether a window covers the screen now (the focused one, full screen). */
bool screen_fullscreen(const screen_t *scr);
/* The rows at the top an overlay takes over a full-screen window now
 * (SCREEN_FS_BAND), else 0: a compositor leaves them to the frame. */
int  screen_overlay_band(const screen_t *scr);
/* For the console's look at full screen without a request: the overlay now
 * (SCREEN_FS_HINT | SCREEN_FS_REVEAL, 0 unless full screen) and
 * SCREEN_FS_ON if it was full screen at its last 'H' 3. */
uint8_t screen_fs_state(const screen_t *scr);
void screen_canvas_vterm(screen_t *scr, uint8_t vterm_id, const uint8_t *req, uint32_t n, uint8_t *reply, int16_t caller_pid);

#endif /* LUGALOS_DRIVERS_SCREEN_H */
