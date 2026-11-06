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
} ugui_surface_t;

#define UGUI_HIT_DISABLED 0x01u  /* dimmed: no press feedback, no focus, a tap hits nothing */

typedef struct {
    ugui_rect_t rect;
    uint16_t    id;             /* Application-defined widget ID (non-zero) */
    int16_t     index;          /* Optional list/segment index */
    uint8_t     flags;          /* UGUI_HIT_* */
} ugui_hit_t;

typedef struct {
    ugui_hit_t  hits[UGUI_MAX_HITBOXES];
    int         count;
    uint16_t    active_id;      /* Highlighted / selected widget ID */
    uint16_t    armed_id;       /* the widget under a finger that is down (0: none); kept across passes */
    int16_t     armed_index;
    uint16_t    focus_id;       /* the keyboard's widget (0: none yet); kept across passes */
    int16_t     focus_index;
    bool        focus_shown;    /* a key moved the focus since the last touch: the ring shows */
    uint8_t     next_disabled;  /* ugui_disable_next()'s parts, taken by the next widget */
} ugui_ctx_t;

/* Reset hit-box registry before a draw pass */
void ugui_begin(ugui_ctx_t *ctx);

/* Register an explicit hit-box in the current draw pass */
void ugui_add_hit(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id, int16_t index);

/* The next widget drawn is dimmed in the given parts -- bit i its i-th part
 * in drawing order: a button or card has one (pass 1), a stepper two (bit 0
 * the -, bit 1 the +), a segmented control one per segment. A dimmed part
 * shows no press feedback, the focus skips it, and a tap on it hits nothing
 * (it is not passed to whatever lies under it). For a control that cannot
 * act now: Back with no history, a stepper at its limit. */
void ugui_disable_next(ugui_ctx_t *ctx, uint8_t parts);

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
bool ugui_selftest(void);

#endif /* LUGALOS_DRIVERS_UGUI_H */
