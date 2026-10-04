#ifndef LUGALOS_DRIVERS_SCREEN_H
#define LUGALOS_DRIVERS_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/canvas1.h"
#include "drivers/vtterm.h"
#include "drivers/ribbon.h"

/* The whole 1-bpp screen as the terminal sees it -- 37.1/37.1a/37.3b,
 * plan/phase37_screen_layouts_and_apps.md §1: a menu bar, the grey
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
 * (38.8, plan/phase38_psram.md; screen_set_store()). A layout change saves
 * the canvas it leaves and restores the one it enters -- without bumping
 * `damage` -- if nothing has been drawn since that one was saved; otherwise,
 * and always without a store, the canvas comes back blank and `damage`
 * moves on, and whoever draws on it redraws (§2.1). A repaint clears it
 * too. The text is stored, in the cell shadow, and survives everything: in
 * CANVAS output still lands in the shadow, and a narrower window keeps each
 * row's left part. */

#define SCREEN_MENU_H        20
#define SCREEN_TILE_Y        22
#define SCREEN_TEXT_X        8
#define SCREEN_TEXT_Y        40
#define SCREEN_TEXT_COLS(w)  ((unsigned)(w) / 8u - 2u)
#define SCREEN_TEXT_ROWS(h)  (((unsigned)(h) - SCREEN_TEXT_Y - 3u) / 16u)

#define SCREEN_RIGHT_MAX  24u           /* bytes of UTF-8, with the NUL */
#define SCREEN_NAME_MAX   16u
#define SCREEN_CTITLE_MAX 32u

enum {
    SCREEN_LAYOUT_TEXT = 0,
    SCREEN_LAYOUT_CANVAS = 1,
    SCREEN_LAYOUT_SPLIT_WIDE = 2,
    SCREEN_LAYOUT_SPLIT_HALF = 3,
    SCREEN_LAYOUT_SPLIT_NARROW = 4,     /* 37.5a: a narrow canvas, 64 text columns */
};

#define SCREEN_MAX_TERMS 8u
#define SCREEN_MAX_CANVASES 4u

typedef struct {
    canvas1_t backing;                  /* persistent backing store in PSRAM */
    uint8_t   vterm_id;                 /* owner terminal ID */
    bool      in_use;
    bool      locked;                   /* canvas layout locked for this slot */
    uint16_t  drawn_w;                  /* the window width `backing` was drawn at; 0: none */
    char      title[SCREEN_CTITLE_MAX]; /* canvas window title */
} screen_canvas_slot_t;

#define SCREEN_STATUS_SD  (1u << 0)
#define SCREEN_STATUS_P9  (1u << 1)

typedef struct {
    canvas1_t cv;                       /* the whole buffer */
    canvas1_t cc;                       /* the canvas tile's drawable area */
    canvas1_t c_backing;                /* Phase 44: persistent backing store for primary canvas */
    uint8_t   layout;
    uint8_t   full_cols;                /* the text window's width in TEXT */
    uint8_t   locked;                   /* 37.5a: 'w' and 'X' may not change the split */
    uint8_t   swapped;                  /* 37.5a: the canvas on the right, text left */
    uint16_t  damage;                   /* bumped whenever the canvas is lost */
    uint8_t   store_ok;                 /* 38.8: bit L-1 -- layout L's store holds its canvas */
    uint8_t   redrawing;                /* 38.8: between 'D' 1 and 'D' 0 */
    uint8_t   status_flags;             /* Top bar indicators: SD mounted, 9P active */
    uint8_t  *store;                    /* 38.8: SCREEN_STORES slots, or NULL */
    uint32_t  store_slot;               /* bytes per slot */
    uint32_t  draw_gen;                 /* bumped by every drawing request */
    uint32_t  store_gen[4];             /* draw_gen when each slot was saved */
    int16_t   fx0, fy0, fx1, fy1;       /* the text tile's frame */
    int16_t   cx0, cy0, cx1, cy1;       /* the canvas tile's frame */
    uint32_t  title_drawn;              /* vt.title_seq the title bar shows */
    char      name[SCREEN_NAME_MAX];    /* the menu bar's left */
    char      right[SCREEN_RIGHT_MAX];  /* its indicators */
    char      ctitle[SCREEN_CTITLE_MAX];/* the canvas tile's title */
    ribbon_t  ribbon;                   /* Phase 44: 1D horizontal window ribbon */
    screen_canvas_slot_t canvases[SCREEN_MAX_CANVASES]; /* Phase 44.2: concurrent canvas backing stores */
    vtterm_t  vt;                       /* legacy reference to active terminal */
    struct {
        vtterm_t vt;
        uint8_t  vterm_id;
        bool     in_use;
    } terms[SCREEN_MAX_TERMS];          /* Phase 44: up to 8 concurrent terminal window instances */
    uint16_t  shadow[];                 /* shadow buffers for all 8 terminals (8 * cols * rows) */
} screen_t;

/* 38.8: the canvas stores -- one per layout with a canvas (CANVAS and the
 * three splits), each room for a whole frame of w x h, packed tile-local:
 * a swapped split restores at its new place. */
#define SCREEN_STORES 4u
#define SCREEN_STORE_BYTES(w, h) (SCREEN_STORES * ((uint32_t)(w) / 8u) * (uint32_t)(h))

/* What a screen of w x h pixels needs, shadow included for all 4 terminals. */
#define SCREEN_BYTES(w, h) \
    (sizeof(screen_t) + SCREEN_MAX_TERMS * SCREEN_TEXT_COLS(w) * SCREEN_TEXT_ROWS(h) * sizeof(uint16_t))

/* Takes over the whole buffer and draws everything: the TEXT layout, the
 * terminal empty, the tile titled `LugalOS` until something says otherwise.
 * `scr` must hold SCREEN_BYTES(w, h); w must be a multiple of 8. */
void screen_init(screen_t *scr, void *fb, uint32_t stride, unsigned w, unsigned h);

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
#define SCREEN_REPLY_LEN 12u

void screen_canvas(screen_t *scr, const uint8_t *req, uint32_t n, uint8_t *reply);
void screen_canvas_vterm(screen_t *scr, uint8_t vterm_id, const uint8_t *req, uint32_t n, uint8_t *reply);

#endif /* LUGALOS_DRIVERS_SCREEN_H */
