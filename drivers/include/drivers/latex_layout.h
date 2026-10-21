#ifndef LUGALOS_DRIVERS_LATEX_LAYOUT_H
#define LUGALOS_DRIVERS_LATEX_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "fs/latex_math.h"
#include "drivers/canvas1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Computes dimensions (w, h, d) and relative coordinates (x, y) for box hierarchy
 * following Knuth Appendix G rules using pure integer arithmetic.
 * base_scale: 1 for standard 16px typography, 2 for 32px HiDPI. */
int latex_layout(math_box_t *root, int base_scale);

/* Renders measured math box hierarchy to 1-bpp bilevel canvas */
void latex_render_canvas1(const math_box_t *root, int origin_x, int baseline_y,
                          const canvas1_t *c, uint8_t fg);

/* Renders measured math box hierarchy to 8-bpp GRAY8 framebuffer */
void latex_render_gray8(const math_box_t *root, int origin_x, int baseline_y,
                        uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                        uint8_t fg, uint8_t bg);

/* Renders measured math box hierarchy to 16-bpp RGB565 framebuffer */
void latex_render_rgb565(const math_box_t *root, int origin_x, int baseline_y,
                         uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                         uint16_t fg, uint16_t bg);

#ifdef __cplusplus
}
#endif

#endif /* LUGALOS_DRIVERS_LATEX_LAYOUT_H */
