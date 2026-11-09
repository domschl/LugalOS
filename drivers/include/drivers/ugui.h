#ifndef LUGALOS_DRIVERS_UGUI_H
#define LUGALOS_DRIVERS_UGUI_H

/* drivers/ugui.h — Stateless Classic-Macintosh Micro-Toolkit
 *
 * Immediate-mode drawing primitives and hit-box registry for Classic Mac
 * monochrome / paper-and-inkGRAY8 surfaces. Renders simultaneously onto
 * a 1-bpp canvas1_t (for screenshots, RP2350, and RAM-screen verification)
 * and an optional 8-bpp anti-aliased GRAY8 surface (for ESP32-P4 MIPI-DSI).
 *
 * All widget dimensions and stroke thicknesses scale with `s->scale`
 * (typically CHROME_SCALE: 1 on 8x16 panels, 2 on 16x32 HiDPI panels).
 * Freestanding C, zero heap allocations, zero FPU.
 */

#include <stdbool.h>
#include <stdint.h>
#include "drivers/canvas1.h"

#define UGUI_MAX_HITBOXES   128

/* Paper-and-ink GRAY8 palette */
#define UGUI_GRAY_PAPER     250u    /* Soft book-grade paper/cream (0xFA) */
#define UGUI_GRAY_CARD      254u    /* Crisp card interior highlight */
#define UGUI_GRAY_TOOLBAR   242u    /* Subtle toolbar / header strip */
#define UGUI_GRAY_TRACK     230u    /* Scrollbar / recessed track */
#define UGUI_GRAY_BORDER    30u     /* Crisp border ink */
#define UGUI_GRAY_SHADOW    90u     /* Drop-shadow and scrollbar thumb */
#define UGUI_GRAY_MUTED     110u    /* Secondary subtitle / rule ink */
#define UGUI_GRAY_INK       10u     /* Deep charcoal dark slate ink (0x0A) */

typedef struct {
    int16_t x, y, w, h;
} ugui_rect_t;

typedef struct {
    const canvas1_t *c1;        /* 1-bpp canvas (screenshots / bilevel fallback) */
    uint8_t         *aa_fb;     /* Optional 8-bpp GRAY8 surface (PSRAM) */
    uint32_t         aa_stride;
    int              w, h;
    int              scale;     /* 1 on 8x16 panels, 2 on 16x32 HiDPI panels */
    bool             clipped;   /* drawing and hit-boxes stay inside clip (ugui_clip()) */
    ugui_rect_t      clip;
} ugui_surface_t;

/* The surface s, clipped to r as well (r in s's coordinates): what is drawn
 * through it lands only inside r and inside any clip s already has, and the
 * hit-boxes its widgets register are cut to it, their touch areas kept in it.
 * A copy, so clips nest by scope: draw a scroll container's children through
 * ugui_clip(s, viewport) and the rest through s. Coordinates do not move. */
ugui_surface_t ugui_clip(const ugui_surface_t *s, ugui_rect_t r);

#define UGUI_HIT_DISABLED 0x01u  /* dimmed: no press feedback, no focus, a tap hits nothing */
#define UGUI_HIT_DRAG     0x02u  /* follows a finger (a slider): its contact is not a gesture */

typedef struct {
    ugui_rect_t rect;
    uint16_t    id;             /* Application-defined widget ID (non-zero) */
    int16_t     index;          /* Optional list/segment index */
    uint8_t     flags;          /* UGUI_HIT_* */
    uint8_t     bound;          /* its touch area's bounds: ctx->bounds[bound - 1]; 0 none */
} ugui_hit_t;

#define UGUI_MAX_BOUNDS   4
#define UGUI_TARGET_MM    7     /* the smallest touch target, as phones' guidelines have it */

typedef struct {
    ugui_hit_t  hits[UGUI_MAX_HITBOXES];
    int         count;
    uint16_t    active_id;      /* Highlighted / selected widget ID */
    uint16_t    armed_id;       /* the widget under a finger that is down (0: none); kept across passes */
    int16_t     armed_index;
    uint16_t    hover_id;       /* the widget under a mouse pointer, no button held (0: none); kept */
    int16_t     hover_index;
    uint16_t    focus_id;       /* the keyboard's widget (0: none yet); kept across passes */
    int16_t     focus_index;
    bool        focus_shown;    /* a key moved the focus since the last touch: the ring shows */
    uint8_t     next_disabled;  /* ugui_disable_next()'s parts, taken by the next widget */
    int16_t     min_target;     /* touch areas grow to this many px (0: as drawn); kept across passes */
    uint8_t     nbounds, cur_bound;
    ugui_rect_t bounds[UGUI_MAX_BOUNDS];
    bool        modal;          /* ugui_modal_begin() this pass: only hits from modal_from count */
    int16_t     modal_from;
} ugui_ctx_t;

/* Reset hit-box registry before a draw pass */
void ugui_begin(ugui_ctx_t *ctx);

/* Register an explicit hit-box in the current draw pass */
void ugui_add_hit(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id, int16_t index);
/* ... one that follows a finger or a held mouse button (UGUI_HIT_DRAG: a
 * scrollbar the caller draws itself) */
void ugui_add_drag_hit(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id);

/* The next widget drawn is dimmed in the given parts -- bit i its i-th part
 * in drawing order: a button or card has one (pass 1), a stepper two (bit 0
 * the -, bit 1 the +), a segmented control one per segment. A dimmed part
 * shows no press feedback, the focus skips it, and a tap on it hits nothing
 * (it is not passed to whatever lies under it). For a control that cannot
 * act now: Back with no history, a stepper at its limit. */
void ugui_disable_next(ugui_ctx_t *ctx, uint8_t parts);

/* Touch targets. A widget drawn smaller than min_px (ugui_target_px():
 * UGUI_TARGET_MM at the panel's DPI) is still touched over an area that
 * big, centred on it -- drawn sizes stay as they are. Where grown areas
 * overlap, the widget drawn nearest the finger has it; a finger on a widget
 * as drawn always gets that one. ugui_slop_bounds() keeps the areas of the
 * widgets that follow inside r (a toolbar: not over the text under it),
 * until ugui_slop_bounds_end() or the next pass. */
int  ugui_target_px(void);
/* The screen's pixels per inch at run time (a panel's, an HDMI monitor's
 * from its EDID; 0 for a screen in RAM), and tenths of a millimetre on it
 * in pixels (0 without a DPI): for sizes a finger or an eye judges. */
int  ugui_dpi(void);
int  ugui_mm_px(int tenths_mm);
void ugui_set_min_target(ugui_ctx_t *ctx, int min_px);
void ugui_slop_bounds(ugui_ctx_t *ctx, ugui_rect_t r);
void ugui_slop_bounds_end(ugui_ctx_t *ctx);
/* The widget under (x, y) as drawn only, ignoring the grown areas: for a
 * caller with targets of its own (the reader's links) that must win over a
 * widget's grown area but not over the widget itself. */
uint16_t ugui_hit_test_drawn(const ugui_ctx_t *ctx, int x, int y, int16_t *out_index);

/* A mouse pointer at (x, y) with no button held: the widget under it as
 * drawn (a pointer is exact; the grown areas are for fingers) is hovered,
 * and ugui_end() outlines it. (-1, -1): the pointer left. True when the
 * hovered widget changed (the caller draws again). */
bool ugui_hover(ugui_ctx_t *ctx, int x, int y);
/* The hovered widget's outline on s; ugui_end() draws it, a pass that does
 * not end there (a toolbar drawn alone) calls this after its widgets. */
void ugui_draw_hover(const ugui_surface_t *s, const ugui_ctx_t *ctx);

/* Whether a finger down at (x, y) is on a widget that follows it
 * (UGUI_HIT_DRAG): the caller then sends it the moves, not gestures. */
bool ugui_drag_at(const ugui_ctx_t *ctx, int x, int y);

/* Hit i's touch area (its rectangle grown and bounded). */
ugui_rect_t ugui_touch_rect(const ugui_ctx_t *ctx, int i);

/* Hit-test a client-relative tap (x, y); returns matching id (or 0) and writes *out_index */
uint16_t ugui_hit_test(const ugui_ctx_t *ctx, int x, int y, int16_t *out_index);

/* Press feedback: a finger down at (x, y) arms the widget under it, which
 * draws inverted until the finger lifts or moves away (ugui_disarm()). Both
 * return whether that changed what the next pass draws. The tap itself still
 * acts as before (ugui_hit_test() at the release). */
bool ugui_arm(ugui_ctx_t *ctx, int x, int y);
bool ugui_disarm(ugui_ctx_t *ctx);
bool ugui_is_armed(const ugui_ctx_t *ctx, uint16_t id, int16_t index);

/* Keyboard focus. Every hit-box is focusable, in the order the pass added
 * them. ugui_nav() takes a key (kernel/keyseq.h's codes and mods):
 *   Tab / Shift-Tab   next / previous widget, wrapping
 *   arrows            the nearest widget in that direction
 *   Enter, Space      activate the focused widget (*id, *index)
 *   Esc               back
 * The first navigation key, Enter included, only shows the focus where it
 * is (on the first widget if there was none). An arrow with nothing beyond answers
 * UGUI_NAV_EDGE, for the caller to scroll. A touch (ugui_arm()) hides the
 * ring until the next key. */
typedef enum {
    UGUI_NAV_NONE = 0,          /* not a navigation key */
    UGUI_NAV_MOVED,             /* the focus moved or showed: draw again */
    UGUI_NAV_ACTIVATE,          /* *id / *index: act as a tap on it would */
    UGUI_NAV_BACK,
    UGUI_NAV_EDGE,              /* an arrow found nothing in its direction */
} ugui_nav_t;

ugui_nav_t ugui_nav(ugui_ctx_t *ctx, uint32_t key, uint8_t mods, uint16_t *id, int16_t *index);
bool ugui_is_focused(const ugui_ctx_t *ctx, uint16_t id, int16_t index);

/* After a pass: the focus is kept on a widget that exists (the first one if
 * its widget is gone) and, when shown, drawn as a dotted ring inside it that
 * inverts what is under it, so it shows on plain and inverted widgets. */
void ugui_end(const ugui_surface_t *s, ugui_ctx_t *ctx);

/* Surface drawing primitives */
void ugui_fill(const ugui_surface_t *s, ugui_rect_t r, uint8_t gray, unsigned pat1);
void ugui_hline(const ugui_surface_t *s, int x0, int x1, int y, int thick, uint8_t gray, unsigned pat1);
void ugui_vline(const ugui_surface_t *s, int x, int y0, int y1, int thick, uint8_t gray, unsigned pat1);

/* Classic Mac bordered box with bottom-right drop shadow */
void ugui_box_shadow(const ugui_surface_t *s, ugui_rect_t r, bool inverted);

/* Typography helpers (proportional serif and monospace) */
int ugui_serif_line_h(const ugui_surface_t *s);
int ugui_mono_cell_w(const ugui_surface_t *s);
int ugui_mono_cell_h(const ugui_surface_t *s);
int ugui_text_serif_width(const ugui_surface_t *s, const char *str, uint32_t len);
int ugui_text_serif(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                    bool bold, bool italic, bool inverted);
int ugui_text_mono(const ugui_surface_t *s, int x, int y, const char *str, uint32_t len,
                   bool bold, bool inverted);

/* Render a 16x16 1-bpp icon bitmap scaled by s->scale * extra_scale */
void ugui_icon16(const ugui_surface_t *s, int x, int y, const uint16_t icon[16],
                 int extra_scale, bool inverted);

/* Standard Classic-Mac widgets (draw and register hit-box) */
void ugui_button(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                 const char *label, uint16_t id, bool active);

void ugui_toolbar_button(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                         const char *label, uint16_t id, bool active);

void ugui_card(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
               const uint16_t icon16[16], const char *title, const char *subtitle,
               uint16_t id, bool active);

void ugui_section_header(const ugui_surface_t *s, int x, int y, int w, const char *title);

void ugui_stepper(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                  const char *label, const char *value_str,
                  uint16_t dec_id, uint16_t inc_id);

void ugui_segmented(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                    const char *const *labels, int count, int selected_idx,
                    uint16_t base_id);

void ugui_list_row(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r,
                   const char *prefix, const char *title, const char *right_meta,
                   uint16_t id, int16_t index, bool selected);

void ugui_scrollbar(const ugui_surface_t *s, ugui_rect_t track, int scroll_y,
                    int total_h, int view_h);

/* Built-in unit self-test */
/* A virtual list: count rows of row_h px scrolled by *scroll_px inside
 * view. Only the rows that show are drawn, each by row() through a surface
 * clipped to view (so a row cut at the edge is cut, its hit-box with it).
 * *scroll_px is clamped to the list (0 .. count * row_h - view.h). Returns
 * how many rows were drawn. */
typedef void (*ugui_row_fn)(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t row, int index, void *user);
int ugui_vlist(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t view, int count, int row_h,
               int *scroll_px, ugui_row_fn row, void *user);

/* The scroll that shows row index whole, as near the current one as can be. */
int ugui_vlist_reveal(int scroll_px, int view_h, int row_h, int index);

/* --- Images ----------------------------------------------------------------
 * 4-bit gray, two pixels a byte (the left one in the high nibble), rows
 * padded to a byte: 0 ink .. 14 white, 15 transparent (drivers/icons.h). */
typedef struct {
    uint16_t       w, h;
    const uint8_t *px;
} ugui_image_t;

/* img at (x, y), each pixel scale x scale: on GRAY8 its gray between ink and
 * white, on 1-bpp an ordered dither of it; transparent pixels left as they
 * are. `inverted`: drawn as on an armed widget. */
void ugui_image4(const ugui_surface_t *s, int x, int y, const ugui_image_t *img, int scale, bool inverted);

/* One entry of an icon grid: its icon (drawn scale x) and its label. */
typedef struct {
    ugui_image_t img;
    int          scale;
    const char  *label;
} ugui_grid_item_t;

/* An icon grid in r: cells of cell_w x cell_h, left to right and down, each
 * its icon over its label (centred, cut to fit). Item i is hit id `id`,
 * index i -- so a press shows it inverted, hover outlines it, and the
 * keyboard's focus moves among the cells (arrows to the nearest that way);
 * a long press or a right click comes as the caller's context event on it.
 * `selected` (-1: none) is drawn framed. Returns how many items fit. */
int ugui_icon_grid(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const ugui_grid_item_t *items, int n,
                   int cell_w, int cell_h, int selected, uint16_t id);

/* A card with an image left of its title and subtitle (the launcher's
 * applications), as ugui_card() otherwise. */
void ugui_card_img(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const ugui_image_t *img,
                   int img_scale, const char *title, const char *subtitle, uint16_t id, bool active);

/* --- Layout ----------------------------------------------------------------
 * Rectangle cuts: take a slice off one side of *r and return it, *r
 * shrinking to the rest (never below zero). A layout is a sequence of cuts,
 * so the same description fits landscape and portrait: cut a column off the
 * left in one, off the top in the other, and lay it out the same. */
ugui_rect_t ugui_cut_top(ugui_rect_t *r, int h);
ugui_rect_t ugui_cut_bottom(ugui_rect_t *r, int h);
ugui_rect_t ugui_cut_left(ugui_rect_t *r, int w);
ugui_rect_t ugui_cut_right(ugui_rect_t *r, int w);
ugui_rect_t ugui_inset(ugui_rect_t r, int dx, int dy);
/* Cell i (row-major) of a cols x rows grid over r, gap apart. */
ugui_rect_t ugui_grid_cell(ugui_rect_t r, int cols, int rows, int gap, int i);

/* --- Controls -------------------------------------------------------------
 * Stateless, as the widgets above: the caller keeps the value and changes it
 * when its id is hit (a tap, or Enter on the focus). */

/* A row: label on the left, an on/off switch on the right. */
void ugui_toggle(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *label, bool on, uint16_t id);
/* A box with a cross when checked, the label after it. */
void ugui_checkbox(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *label, bool checked, uint16_t id);
/* A track with a thumb at value (min..max); ugui_slider_value() is the value
 * a tap at x sets. Left / Right on its focus are the caller's steps. */
void ugui_slider(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, int value, int min, int max, uint16_t id);
int  ugui_slider_value(const ugui_surface_t *s, ugui_rect_t r, int x, int min, int max);
/* A bar filled permille / 1000 of the way. No hit-box. */
void ugui_progress(const ugui_surface_t *s, ugui_rect_t r, int permille);
/* Tabs of equal width; the selected one open toward the content under it.
 * Tab i is id base_id + i, index i. */
void ugui_tab_bar(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *const *labels,
                  int count, int selected, uint16_t base_id);
/* A path as tappable segments ("/" the root, then each directory): segment
 * i (0 the root) is id `id`, index i -- the path up to and including it.
 * Leading segments that do not fit give way to an ellipsis. */
void ugui_path_bar(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const char *path, uint16_t id);
/* The length of path's first n segments as ugui_path_bar() counts them
 * ("/sd0/docs", 2 -> 4: "/sd0"; 0 -> 1: "/"). */
int  ugui_path_prefix(const char *path, int n);

/* A single-line text field over the caller's buffer. The caret and the
 * selection (anchor..caret) show while the field has the focus. */
typedef struct {
    char    *buf;               /* NUL-terminated, cap bytes with the NUL */
    uint16_t cap, len;
    uint16_t caret, anchor;     /* anchor == caret: no selection */
} ugui_text_t;

typedef enum { UGUI_TEXT_NONE = 0, UGUI_TEXT_CHANGED, UGUI_TEXT_MOVED, UGUI_TEXT_SUBMIT, UGUI_TEXT_CANCEL } ugui_text_ev_t;

void ugui_text_init(ugui_text_t *t, char *buf, uint16_t cap);
/* A key for the field (kernel/keyseq.h's codes): text inserted over the
 * selection, Backspace / Delete, Left / Right / Home / End (with Shift:
 * selecting), Enter submits, Esc cancels; Cmd+C / X / V copy, cut and paste
 * through the system clipboard (one line of it), Cmd+A selects all. NONE:
 * not a key it takes. */
ugui_text_ev_t ugui_text_key(ugui_text_t *t, uint32_t key, uint8_t mods);
void ugui_textfield(const ugui_surface_t *s, ugui_ctx_t *ctx, ugui_rect_t r, const ugui_text_t *t, uint16_t id);

/* Moves the focus to (id, index), shown or not as it was: a tap on a text
 * field gives it the caret. */
void ugui_set_focus(ugui_ctx_t *ctx, uint16_t id, int16_t index);

/* Modal: the widgets after ugui_modal_begin() are the only ones touched or
 * focused this pass; a tap anywhere else hits nothing. ugui_dialog() and
 * ugui_action_sheet() begin it themselves, over the rest dimmed. */
void ugui_modal_begin(const ugui_surface_t *s, ugui_ctx_t *ctx);
/* A dialog in the middle of s: title, message (lines split at '\n'), and
 * buttons from the right, the last label the default (framed bold); button
 * i is id base_id + i. Returns its rectangle. */
ugui_rect_t ugui_dialog(const ugui_surface_t *s, ugui_ctx_t *ctx, const char *title, const char *message,
                        const char *const *buttons, int count, uint16_t base_id);
/* As ugui_dialog(), with an alert's icon left of the text (NULL: none),
 * each of its pixels icon_scale x icon_scale. */
ugui_rect_t ugui_alert(const ugui_surface_t *s, ugui_ctx_t *ctx, const ugui_image_t *icon, int icon_scale,
                       const char *title, const char *message, const char *const *buttons, int count,
                       uint16_t base_id);
/* An action sheet: the choices stacked full width at the bottom of s, item
 * i id base_id + i. Returns its rectangle. */
ugui_rect_t ugui_action_sheet(const ugui_surface_t *s, ugui_ctx_t *ctx, const char *title,
                              const char *const *items, int count, uint16_t base_id);

bool ugui_selftest(void);

#endif /* LUGALOS_DRIVERS_UGUI_H */
