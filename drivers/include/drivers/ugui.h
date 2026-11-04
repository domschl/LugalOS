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

#define UGUI_MAX_HITBOXES   48

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

typedef struct {
    ugui_rect_t rect;
    uint16_t    id;             /* Application-defined widget ID (non-zero) */
    int16_t     index;          /* Optional list/segment index */
} ugui_hit_t;

typedef struct {
    ugui_hit_t  hits[UGUI_MAX_HITBOXES];
    int         count;
    uint16_t    active_id;      /* Highlighted / selected widget ID */
} ugui_ctx_t;

/* Reset hit-box registry before a draw pass */
void ugui_begin(ugui_ctx_t *ctx);

/* Register an explicit hit-box in the current draw pass */
void ugui_add_hit(ugui_ctx_t *ctx, ugui_rect_t r, uint16_t id, int16_t index);

/* Hit-test a client-relative tap (x, y); returns matching id (or 0) and writes *out_index */
uint16_t ugui_hit_test(const ugui_ctx_t *ctx, int x, int y, int16_t *out_index);

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
