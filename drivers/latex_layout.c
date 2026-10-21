/* drivers/latex_layout.c — Knuth Appendix G Pure Integer Box-and-Glue Layout Engine.
 *
 * Computes bounding boxes (w, h, d) and (x, y) coordinates for mathematical
 * formulas. Pure integer arithmetic with Zero Floating-Point in Kernel / M-Mode.
 */

#ifndef LUGALOS_HOST_TEST
#include "lugalos_config.h"
#endif

#include "drivers/latex_layout.h"
#include "drivers/font_math.h"
#include "drivers/font_serif.h"
#if defined(CONFIG_FONT_HIDPI) || (defined(CONFIG_FONT_CELL_H) && (CONFIG_FONT_CELL_H == 32)) || defined(LUGALOS_HOST_TEST)
#include "drivers/font_noto_serif.h"
#define MATH_USE_HIDPI 1
#else
#define MATH_USE_HIDPI 0
#endif
#include "drivers/canvas1.h"

int latex_layout(math_box_t *b, int base_scale) {
    if (!b) return 0;
    if (base_scale <= 0) base_scale = 1;

    switch (b->type) {
    case MATH_BOX_CHAR: {
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g) {
            int scale = (b->style == STYLE_SCRIPT) ? (base_scale * 7) / 10 :
                        (b->style == STYLE_SCRIPTSCRIPT) ? (base_scale * 5) / 10 : base_scale;
            if (scale < 1) scale = 1;
            b->w = (int16_t)(g->advance * scale);
            b->h = (int16_t)(g->bearing_y * scale);
            b->d = (int16_t)((g->h - g->bearing_y) * scale);
            if (b->d < 0) b->d = 0;
        } else {
            /* Standard serif font metrics fallback */
#if MATH_USE_HIDPI
            if (base_scale > 1) {
                if (b->style == STYLE_SCRIPT || b->style == STYLE_SCRIPTSCRIPT) {
                    /* In HiDPI, scripts use the 16px standard serif font */
                    uint8_t sw = font_serif_char_width((char)b->u.ch.codepoint);
                    b->w = (int16_t)sw;
                    b->h = 12;
                    b->d = 3;
                } else {
                    /* Main text in HiDPI uses 32px Noto serif font */
                    uint8_t nw = font_noto_serif_char_width((char)b->u.ch.codepoint);
                    b->w = (int16_t)nw;
                    b->h = 24;
                    b->d = 6;
                }
            } else
#endif
            {
                uint8_t sw = font_serif_char_width((char)b->u.ch.codepoint);
                if (b->style == STYLE_SCRIPT) {
                    b->w = (int16_t)((sw * 7 * base_scale) / 10);
                    b->h = (int16_t)(10 * base_scale);
                    b->d = (int16_t)(2 * base_scale);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    b->w = (int16_t)((sw * 5 * base_scale) / 10);
                    b->h = (int16_t)(7 * base_scale);
                    b->d = (int16_t)(1 * base_scale);
                } else {
                    b->w = (int16_t)(sw * base_scale);
                    b->h = (int16_t)(14 * base_scale);
                    b->d = (int16_t)(3 * base_scale);
                }
            }
        }
        break;
    }

    case MATH_BOX_HLIST: {
        int16_t cur_x = 0;
        int16_t max_h = 0;
        int16_t max_d = 0;

        math_box_t *child = b->u.list.head;
        while (child) {
            latex_layout(child, base_scale);
            child->x = cur_x;
            child->y = 0;
            cur_x = (int16_t)(cur_x + child->w);
            if (child->h > max_h) max_h = child->h;
            if (child->d > max_d) max_d = child->d;
            child = child->next;
        }
        b->w = cur_x;
        b->h = max_h;
        b->d = max_d;
        break;
    }

    case MATH_BOX_FRAC: {
        math_box_t *num = b->u.frac.num;
        math_box_t *den = b->u.frac.den;
        latex_layout(num, base_scale);
        latex_layout(den, base_scale);

        int16_t pad = (int16_t)(2 * base_scale);
        int16_t content_w = (num && den) ? ((num->w > den->w) ? num->w : den->w) :
                            (num ? num->w : (den ? den->w : 8));
        b->w = (int16_t)(content_w + pad * 2);

        int16_t axis = (int16_t)(font_math_axis_height((math_style_t)b->style) * base_scale);
        int16_t clearance = (int16_t)(2 * base_scale);

        if (num) {
            num->x = (int16_t)((b->w - num->w) / 2);
            num->y = (int16_t)(axis + clearance + num->d);
        }
        if (den) {
            den->x = (int16_t)((b->w - den->w) / 2);
            den->y = (int16_t)(axis - clearance - den->h);
        }

        b->h = (num) ? (int16_t)(num->y + num->h - axis + 6 * base_scale) : (int16_t)(10 * base_scale);
        b->d = (den) ? (int16_t)(axis - den->y + den->d) : (int16_t)(6 * base_scale);
        break;
    }

    case MATH_BOX_RADICAL: {
        math_box_t *body = b->u.radical.body;
        latex_layout(body, base_scale);

        int16_t surd_w = (int16_t)(9 * base_scale);
        int16_t pad = (int16_t)(2 * base_scale);
        b->w = (int16_t)(surd_w + (body ? body->w : 8) + pad);

        if (body) {
            body->x = (int16_t)(surd_w + pad);
            body->y = 0;
            b->h = (int16_t)(body->h + 3 * base_scale);
            b->d = body->d;
        } else {
            b->h = (int16_t)(14 * base_scale);
            b->d = (int16_t)(3 * base_scale);
        }
        break;
    }

    case MATH_BOX_SCRIPT: {
        math_box_t *nuc = b->u.script.nucleus;
        math_box_t *sup = b->u.script.sup;
        math_box_t *sub = b->u.script.sub;

        latex_layout(nuc, base_scale);
        if (nuc) {
            nuc->x = 0;
            nuc->y = 0;
            b->h = nuc->h;
            b->d = nuc->d;
        }

        int16_t sup_w = 0;
        int16_t sub_w = 0;

        if (sup) {
            latex_layout(sup, base_scale);
            sup->x = nuc ? nuc->w : 0;
            int16_t shift = (int16_t)(nuc ? ((nuc->h * 45) / 100) : (4 * base_scale));
            if (shift < 3 * base_scale) shift = (int16_t)(3 * base_scale);
            sup->y = shift;
            sup_w = sup->w;
            if (sup->y + sup->h > b->h) b->h = (int16_t)(sup->y + sup->h);
        }
        if (sub) {
            latex_layout(sub, base_scale);
            sub->x = nuc ? nuc->w : 0;
            int16_t shift = (int16_t)(nuc ? ((nuc->d * 50) / 100 + 2 * base_scale) : (2 * base_scale));
            sub->y = (int16_t)(-shift);
            sub_w = sub->w;
            if (-sub->y + sub->d > b->d) b->d = (int16_t)(-sub->y + sub->d);
        }

        int16_t script_w = (sup_w > sub_w) ? sup_w : sub_w;
        b->w = (int16_t)((nuc ? nuc->w : 0) + script_w);
        break;
    }

    case MATH_BOX_BIGOP: {
        int16_t op_w = (int16_t)(14 * base_scale);
        int16_t op_h = (int16_t)(16 * base_scale);

        math_box_t *sup = b->u.bigop.sup;
        math_box_t *sub = b->u.bigop.sub;

        if (b->style == STYLE_DISPLAY) {
            latex_layout(sup, base_scale);
            latex_layout(sub, base_scale);

            int16_t max_w = op_w;
            if (sup && sup->w > max_w) max_w = sup->w;
            if (sub && sub->w > max_w) max_w = sub->w;

            b->w = max_w;
            if (sup) {
                sup->x = (int16_t)((max_w - sup->w) / 2);
                sup->y = (int16_t)(op_h + 2 * base_scale);
            }
            if (sub) {
                sub->x = (int16_t)((max_w - sub->w) / 2);
                sub->y = (int16_t)(-4 * base_scale - sub->h);
            }
            b->h = (sup) ? (int16_t)(sup->y + sup->h) : op_h;
            b->d = (sub) ? (int16_t)(-sub->y + sub->d) : (int16_t)(4 * base_scale);
        } else {
            /* Text style: limits to the right */
            latex_layout(sup, base_scale);
            latex_layout(sub, base_scale);
            if (sup) {
                sup->x = op_w;
                sup->y = (int16_t)((op_h * 45) / 100);
            }
            if (sub) {
                sub->x = op_w;
                sub->y = (int16_t)(-4 * base_scale);
            }
            int16_t script_w = (sup && sub) ? ((sup->w > sub->w) ? sup->w : sub->w) :
                               (sup ? sup->w : (sub ? sub->w : 0));
            b->w = (int16_t)(op_w + script_w);
            b->h = op_h;
            b->d = (int16_t)(4 * base_scale);
        }
        break;
    }

    case MATH_BOX_DELIM: {
        b->w = (int16_t)(6 * base_scale);
        b->h = (b->u.delim.target_h > 0) ? b->u.delim.target_h : (int16_t)(14 * base_scale);
        b->d = (b->u.delim.target_d > 0) ? b->u.delim.target_d : (int16_t)(3 * base_scale);
        break;
    }

    case MATH_BOX_SPACE: {
        b->w = (int16_t)(b->u.space.width * base_scale);
        b->h = 0;
        b->d = 0;
        break;
    }

    case MATH_BOX_MATRIX: {
        int16_t total_w = 0;
        int16_t total_h = 0;
        int16_t gap = (int16_t)(8 * base_scale);

        for (uint8_t c = 0; c < b->u.matrix.cols; c++) {
            b->u.matrix.col_w[c] = (int16_t)(12 * base_scale);
            for (uint8_t r = 0; r < b->u.matrix.rows; r++) {
                math_box_t *cell = b->u.matrix.cells ? b->u.matrix.cells[r * MATH_MATRIX_MAX_COLS + c] : NULL;
                if (cell) {
                    latex_layout(cell, base_scale);
                    if (cell->w > b->u.matrix.col_w[c]) b->u.matrix.col_w[c] = cell->w;
                }
            }
            total_w = (int16_t)(total_w + b->u.matrix.col_w[c]);
            if (c > 0) total_w = (int16_t)(total_w + gap);
        }

        for (uint8_t r = 0; r < b->u.matrix.rows; r++) {
            b->u.matrix.row_h[r] = (int16_t)(14 * base_scale);
            total_h = (int16_t)(total_h + b->u.matrix.row_h[r]);
        }

        b->w = (int16_t)(total_w + 12 * base_scale); /* add delimiter padding */
        b->h = (int16_t)(total_h / 2);
        b->d = (int16_t)(total_h / 2);
        break;
    }

    default:
        b->w = (int16_t)(8 * base_scale);
        b->h = (int16_t)(12 * base_scale);
        b->d = (int16_t)(2 * base_scale);
        break;
    }

    return (int)b->w;
}

/* --- Canvas 1-bpp Bilevel Renderer --- */

static inline void canvas1_put_pixel(const canvas1_t *c, int x, int y) {
    canvas1_hline(c, x, x, y, CANVAS1_BLACK);
}

static void render_glyph_1bpp(const canvas1_t *c, int px, int py,
                              const uint16_t *rows, int w, int h, int scale) {
    if (!c || !rows) return;
    for (int r = 0; r < h; r++) {
        uint16_t bits = rows[r];
        for (int col = 0; col < w; col++) {
            if (bits & (0x8000 >> col)) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        canvas1_put_pixel(c, px + col * scale + dx, py + r * scale + dy);
                    }
                }
            }
        }
    }
}

void latex_render_canvas1(const math_box_t *b, int origin_x, int baseline_y,
                          const canvas1_t *c, uint8_t fg) {
    (void)fg;
    if (!b || !c) return;

    int abs_x = origin_x + b->x;
    int abs_y = baseline_y - b->y;

    switch (b->type) {
    case MATH_BOX_CHAR: {
        int scale = (b->h > 18) ? 2 : 1;
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g && g->rows) {
            int gy = abs_y - g->bearing_y * scale;
            render_glyph_1bpp(c, abs_x, gy, g->rows, g->w, g->h, scale);
        } else {
#if MATH_USE_HIDPI
            if (scale > 1) {
                canvas1_noto_serif_glyph(c, abs_x, abs_y - 24, (char)b->u.ch.codepoint, false, false);
            } else
#endif
            {
                canvas1_serif_glyph(c, abs_x, abs_y - 12, (char)b->u.ch.codepoint, false, false);
            }
        }
        break;
    }

    case MATH_BOX_HLIST: {
        const math_box_t *child = b->u.list.head;
        while (child) {
            latex_render_canvas1(child, abs_x, abs_y, c, fg);
            child = child->next;
        }
        break;
    }

    case MATH_BOX_FRAC: {
        if (b->u.frac.num) latex_render_canvas1(b->u.frac.num, abs_x, abs_y, c, fg);
        if (b->u.frac.den) latex_render_canvas1(b->u.frac.den, abs_x, abs_y, c, fg);

        /* Draw fraction rule bar */
        if (b->u.frac.has_bar) {
            int scale = (b->h > 24) ? 2 : 1;
            int axis = font_math_axis_height((math_style_t)b->style) * scale;
            int bar_y = abs_y - axis;
            for (int dy = 0; dy < scale; dy++) {
                canvas1_hline(c, abs_x, abs_x + b->w - 1, bar_y + dy, CANVAS1_BLACK);
            }
        }
        break;
    }

    case MATH_BOX_RADICAL: {
        int scale = (b->h > 24) ? 2 : 1;
        if (b->u.radical.body) {
            latex_render_canvas1(b->u.radical.body, abs_x, abs_y, c, fg);
        }
        /* Draw radical surd and vinculum overbar */
        int top_y = abs_y - b->h;
        for (int dy = 0; dy < scale; dy++) {
            canvas1_hline(c, abs_x + 9 * scale, abs_x + b->w - 1, top_y + dy, CANVAS1_BLACK);
        }
        /* Surd hook */
        canvas1_put_pixel(c, abs_x + 8 * scale, top_y + 1);
        canvas1_put_pixel(c, abs_x + 7 * scale, top_y + 3);
        canvas1_put_pixel(c, abs_x + 6 * scale, abs_y + b->d);
        canvas1_put_pixel(c, abs_x + 5 * scale, abs_y + b->d - 2);
        canvas1_put_pixel(c, abs_x + 4 * scale, abs_y + b->d - 4);
        canvas1_put_pixel(c, abs_x + 3 * scale, abs_y + b->d - 3);
        break;
    }

    case MATH_BOX_SCRIPT: {
        if (b->u.script.nucleus) latex_render_canvas1(b->u.script.nucleus, abs_x, abs_y, c, fg);
        if (b->u.script.sup) latex_render_canvas1(b->u.script.sup, abs_x, abs_y, c, fg);
        if (b->u.script.sub) latex_render_canvas1(b->u.script.sub, abs_x, abs_y, c, fg);
        break;
    }

    case MATH_BOX_BIGOP: {
        int scale = (b->h > 24) ? 2 : 1;
        uint16_t op_cp = (b->u.bigop.op == BIGOP_SUM) ? 0x2211 :
                         (b->u.bigop.op == BIGOP_PROD) ? 0x220F : 0x222B;
        const math_glyph_t *g = font_math_lookup(op_cp, (math_style_t)b->style);
        if (g && g->rows) {
            render_glyph_1bpp(c, abs_x, abs_y - g->bearing_y * scale, g->rows, g->w, g->h, scale);
        }
        if (b->u.bigop.sup) latex_render_canvas1(b->u.bigop.sup, abs_x, abs_y, c, fg);
        if (b->u.bigop.sub) latex_render_canvas1(b->u.bigop.sub, abs_x, abs_y, c, fg);
        break;
    }

    case MATH_BOX_DELIM: {
        /* Render vertical delimiter bracket or paren */
        int top = abs_y - b->h;
        int bot = abs_y + b->d;
        canvas1_vline(c, abs_x + 2, top, bot, CANVAS1_BLACK);
        canvas1_put_pixel(c, abs_x + 3, top);
        canvas1_put_pixel(c, abs_x + 3, bot);
        break;
    }

    case MATH_BOX_MATRIX: {
        int cur_x = abs_x + 6;
        for (uint8_t col = 0; col < b->u.matrix.cols; col++) {
            int cur_y = abs_y - b->h;
            for (uint8_t row = 0; row < b->u.matrix.rows; row++) {
                const math_box_t *cell = b->u.matrix.cells ? b->u.matrix.cells[row * MATH_MATRIX_MAX_COLS + col] : NULL;
                if (cell) {
                    latex_render_canvas1(cell, cur_x, cur_y + b->u.matrix.row_h[row] - cell->d, c, fg);
                }
                cur_y += b->u.matrix.row_h[row];
            }
            cur_x += b->u.matrix.col_w[col] + 8;
        }
        break;
    }

    default:
        break;
    }
}

/* --- Gray8 & RGB565 Framebuffer Renderers --- */

void latex_render_gray8(const math_box_t *b, int origin_x, int baseline_y,
                        uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                        uint8_t fg, uint8_t bg) {
    (void)bg;
    if (!b || !fb || clip_w <= 0 || clip_h <= 0) return;

    /* Render bilevel pixels onto gray8 */
    int abs_x = origin_x + b->x;
    int abs_y = baseline_y - b->y;

    switch (b->type) {
    case MATH_BOX_CHAR: {
        int scale = (b->h > 18) ? 2 : 1;
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g && g->rows) {
            int gy = abs_y - g->bearing_y * scale;
            for (int r = 0; r < g->h; r++) {
                uint16_t bits = g->rows[r];
                for (int col = 0; col < g->w; col++) {
                    if (bits & (0x8000 >> col)) {
                        for (int dy = 0; dy < scale; dy++) {
                            int py = gy + r * scale + dy;
                            if (py < 0 || py >= clip_h) continue;
                            for (int dx = 0; dx < scale; dx++) {
                                int px = abs_x + col * scale + dx;
                                if (px >= 0 && px < clip_w) {
                                    fb[py * stride + px] = fg;
                                }
                            }
                        }
                    }
                }
            }
        } else {
#if MATH_USE_HIDPI
            if (scale > 1) {
                font_noto_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h,
                                               abs_x, abs_y - 24, (char)b->u.ch.codepoint, fg, bg, false, false);
            } else
#endif
            {
                font_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h,
                                          abs_x, abs_y - 12, (char)b->u.ch.codepoint, fg, bg, false, false);
            }
        }
        return;
    }

    case MATH_BOX_HLIST: {
        const math_box_t *child = b->u.list.head;
        while (child) {
            latex_render_gray8(child, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
            child = child->next;
        }
        return;
    }

    case MATH_BOX_FRAC: {
        if (b->u.frac.num) latex_render_gray8(b->u.frac.num, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.frac.den) latex_render_gray8(b->u.frac.den, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.frac.has_bar) {
            int scale = (b->h > 24) ? 2 : 1;
            int axis = font_math_axis_height((math_style_t)b->style) * scale;
            int bar_y = abs_y - axis;
            for (int dy = 0; dy < scale; dy++) {
                int py = bar_y + dy;
                if (py >= 0 && py < clip_h) {
                    for (int k = abs_x; k < abs_x + b->w && k < clip_w; k++) {
                        if (k >= 0) fb[py * stride + k] = fg;
                    }
                }
            }
        }
        return;
    }

    case MATH_BOX_RADICAL: {
        int scale = (b->h > 24) ? 2 : 1;
        if (b->u.radical.body) latex_render_gray8(b->u.radical.body, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        int top_y = abs_y - b->h;
        for (int dy = 0; dy < scale; dy++) {
            int py = top_y + dy;
            if (py >= 0 && py < clip_h) {
                for (int k = abs_x + 9 * scale; k < abs_x + b->w && k < clip_w; k++) {
                    if (k >= 0) fb[py * stride + k] = fg;
                }
            }
        }
        return;
    }

    case MATH_BOX_SCRIPT: {
        if (b->u.script.nucleus) latex_render_gray8(b->u.script.nucleus, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.script.sup) latex_render_gray8(b->u.script.sup, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.script.sub) latex_render_gray8(b->u.script.sub, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        return;
    }

    case MATH_BOX_BIGOP: {
        int scale = (b->h > 24) ? 2 : 1;
        uint16_t op_cp = (b->u.bigop.op == BIGOP_SUM) ? 0x2211 :
                         (b->u.bigop.op == BIGOP_PROD) ? 0x220F : 0x222B;
        const math_glyph_t *g = font_math_lookup(op_cp, (math_style_t)b->style);
        if (g && g->rows) {
            int gy = abs_y - g->bearing_y * scale;
            for (int r = 0; r < g->h; r++) {
                uint16_t bits = g->rows[r];
                for (int col = 0; col < g->w; col++) {
                    if (bits & (0x8000 >> col)) {
                        for (int dy = 0; dy < scale; dy++) {
                            int py = gy + r * scale + dy;
                            if (py < 0 || py >= clip_h) continue;
                            for (int dx = 0; dx < scale; dx++) {
                                int px = abs_x + col * scale + dx;
                                if (px >= 0 && px < clip_w) {
                                    fb[py * stride + px] = fg;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (b->u.bigop.sup) latex_render_gray8(b->u.bigop.sup, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.bigop.sub) latex_render_gray8(b->u.bigop.sub, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        return;
    }

    case MATH_BOX_DELIM: {
        int top = abs_y - b->h;
        int bot = abs_y + b->d;
        for (int y = top; y <= bot && y < clip_h; y++) {
            if (y >= 0 && abs_x + 2 < clip_w) fb[y * stride + abs_x + 2] = fg;
        }
        return;
    }

    case MATH_BOX_MATRIX: {
        int cur_x = abs_x + 6;
        for (uint8_t col = 0; col < b->u.matrix.cols; col++) {
            int cur_y = abs_y - b->h;
            for (uint8_t row = 0; row < b->u.matrix.rows; row++) {
                const math_box_t *cell = b->u.matrix.cells ? b->u.matrix.cells[row * MATH_MATRIX_MAX_COLS + col] : NULL;
                if (cell) {
                    latex_render_gray8(cell, cur_x, cur_y + b->u.matrix.row_h[row] - cell->d, fb, stride, clip_w, clip_h, fg, bg);
                }
                cur_y += b->u.matrix.row_h[row];
            }
            cur_x += b->u.matrix.col_w[col] + 8;
        }
        return;
    }

    default:
        break;
    }
}

void latex_render_rgb565(const math_box_t *b, int origin_x, int baseline_y,
                         uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                         uint16_t fg, uint16_t bg) {
    (void)bg;
    if (!b || !fb || clip_w <= 0 || clip_h <= 0) return;

    int abs_x = origin_x + b->x;
    int abs_y = baseline_y - b->y;

    switch (b->type) {
    case MATH_BOX_CHAR: {
        int scale = (b->h > 18) ? 2 : 1;
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g && g->rows) {
            int gy = abs_y - g->bearing_y * scale;
            for (int r = 0; r < g->h; r++) {
                uint16_t bits = g->rows[r];
                for (int col = 0; col < g->w; col++) {
                    if (bits & (0x8000 >> col)) {
                        for (int dy = 0; dy < scale; dy++) {
                            int py = gy + r * scale + dy;
                            if (py < 0 || py >= clip_h) continue;
                            for (int dx = 0; dx < scale; dx++) {
                                int px = abs_x + col * scale + dx;
                                if (px >= 0 && px < clip_w) {
                                    fb[py * stride_pixels + px] = fg;
                                }
                            }
                        }
                    }
                }
            }
        } else {
#if MATH_USE_HIDPI
            if (scale > 1) {
                font_noto_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h,
                                                abs_x, abs_y - 24, (char)b->u.ch.codepoint, fg, bg, false, false);
            } else
#endif
            {
                font_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h,
                                           abs_x, abs_y - 12, (char)b->u.ch.codepoint, fg, bg, false, false);
            }
        }
        return;
    }

    case MATH_BOX_HLIST: {
        const math_box_t *child = b->u.list.head;
        while (child) {
            latex_render_rgb565(child, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
            child = child->next;
        }
        return;
    }

    case MATH_BOX_FRAC: {
        if (b->u.frac.num) latex_render_rgb565(b->u.frac.num, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.frac.den) latex_render_rgb565(b->u.frac.den, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.frac.has_bar) {
            int scale = (b->h > 24) ? 2 : 1;
            int axis = font_math_axis_height((math_style_t)b->style) * scale;
            int bar_y = abs_y - axis;
            for (int dy = 0; dy < scale; dy++) {
                int py = bar_y + dy;
                if (py >= 0 && py < clip_h) {
                    for (int k = abs_x; k < abs_x + b->w && k < clip_w; k++) {
                        if (k >= 0) fb[py * stride_pixels + k] = fg;
                    }
                }
            }
        }
        return;
    }

    case MATH_BOX_RADICAL: {
        int scale = (b->h > 24) ? 2 : 1;
        if (b->u.radical.body) latex_render_rgb565(b->u.radical.body, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        int top_y = abs_y - b->h;
        for (int dy = 0; dy < scale; dy++) {
            int py = top_y + dy;
            if (py >= 0 && py < clip_h) {
                for (int k = abs_x + 9 * scale; k < abs_x + b->w && k < clip_w; k++) {
                    if (k >= 0) fb[py * stride_pixels + k] = fg;
                }
            }
        }
        return;
    }

    case MATH_BOX_SCRIPT: {
        if (b->u.script.nucleus) latex_render_rgb565(b->u.script.nucleus, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.script.sup) latex_render_rgb565(b->u.script.sup, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.script.sub) latex_render_rgb565(b->u.script.sub, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        return;
    }

    case MATH_BOX_BIGOP: {
        int scale = (b->h > 24) ? 2 : 1;
        uint16_t op_cp = (b->u.bigop.op == BIGOP_SUM) ? 0x2211 :
                         (b->u.bigop.op == BIGOP_PROD) ? 0x220F : 0x222B;
        const math_glyph_t *g = font_math_lookup(op_cp, (math_style_t)b->style);
        if (g && g->rows) {
            int gy = abs_y - g->bearing_y * scale;
            for (int r = 0; r < g->h; r++) {
                uint16_t bits = g->rows[r];
                for (int col = 0; col < g->w; col++) {
                    if (bits & (0x8000 >> col)) {
                        for (int dy = 0; dy < scale; dy++) {
                            int py = gy + r * scale + dy;
                            if (py < 0 || py >= clip_h) continue;
                            for (int dx = 0; dx < scale; dx++) {
                                int px = abs_x + col * scale + dx;
                                if (px >= 0 && px < clip_w) {
                                    fb[py * stride_pixels + px] = fg;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (b->u.bigop.sup) latex_render_rgb565(b->u.bigop.sup, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.bigop.sub) latex_render_rgb565(b->u.bigop.sub, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        return;
    }

    case MATH_BOX_DELIM: {
        int top = abs_y - b->h;
        int bot = abs_y + b->d;
        for (int y = top; y <= bot && y < clip_h; y++) {
            if (y >= 0 && abs_x + 2 < clip_w) fb[y * stride_pixels + abs_x + 2] = fg;
        }
        return;
    }

    case MATH_BOX_MATRIX: {
        int cur_x = abs_x + 6;
        for (uint8_t col = 0; col < b->u.matrix.cols; col++) {
            int cur_y = abs_y - b->h;
            for (uint8_t row = 0; row < b->u.matrix.rows; row++) {
                const math_box_t *cell = b->u.matrix.cells ? b->u.matrix.cells[row * MATH_MATRIX_MAX_COLS + col] : NULL;
                if (cell) {
                    latex_render_rgb565(cell, cur_x, cur_y + b->u.matrix.row_h[row] - cell->d, fb, stride_pixels, clip_w, clip_h, fg, bg);
                }
                cur_y += b->u.matrix.row_h[row];
            }
            cur_x += b->u.matrix.col_w[col] + 8;
        }
        return;
    }

    default:
        break;
    }
}
