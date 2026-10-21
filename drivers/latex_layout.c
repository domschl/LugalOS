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
    b->scale = (uint8_t)base_scale;

    switch (b->type) {
    case MATH_BOX_CHAR: {
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g) {
            if (base_scale > 1) {
                int scale = (b->style == STYLE_SCRIPT || b->style == STYLE_SCRIPTSCRIPT) ? 1 : base_scale;
                if (scale < 1) scale = 1;
                b->w = (int16_t)(g->advance * scale);
                b->h = (int16_t)(g->bearing_y * scale);
                b->d = (int16_t)((g->h - g->bearing_y) * scale);
            } else {
                if (b->style == STYLE_SCRIPT) {
                    b->w = (int16_t)((g->advance * 7 + 5) / 10);
                    if (b->w < 3) b->w = 3;
                    b->h = (int16_t)((g->bearing_y * 7 + 5) / 10);
                    b->d = (int16_t)(((g->h - g->bearing_y) * 7 + 5) / 10);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    b->w = (int16_t)((g->advance * 5 + 5) / 10);
                    if (b->w < 2) b->w = 2;
                    b->h = (int16_t)((g->bearing_y * 5 + 5) / 10);
                    b->d = (int16_t)(((g->h - g->bearing_y) * 5 + 5) / 10);
                } else {
                    b->w = (int16_t)g->advance;
                    b->h = (int16_t)g->bearing_y;
                    b->d = (int16_t)(g->h - g->bearing_y);
                }
            }
            if (b->d < 0) b->d = 0;
        } else {
            /* Standard serif font metrics fallback */
#if MATH_USE_HIDPI
            if (base_scale > 1) {
                if (b->style == STYLE_SCRIPT) {
                    /* In HiDPI, scripts use the 16px standard serif font */
                    uint8_t sw = font_serif_char_width((char)b->u.ch.codepoint);
                    b->w = (int16_t)sw;
                    b->h = 12;
                    b->d = 4;
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    /* In HiDPI, scriptscript uses 10px font */
                    uint8_t sw = font_serif_char_width((char)b->u.ch.codepoint);
                    b->w = (int16_t)((sw * 7 + 5) / 10);
                    if (b->w < 3) b->w = 3;
                    b->h = 8;
                    b->d = 2;
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
                    b->w = (int16_t)((sw * 7 + 5) / 10);
                    if (b->w < 3) b->w = 3;
                    b->h = 8;
                    b->d = 2;
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    b->w = (int16_t)((sw * 5 + 5) / 10);
                    if (b->w < 2) b->w = 2;
                    b->h = 5;
                    b->d = 2;
                } else {
                    b->w = (int16_t)sw;
                    b->h = 12;
                    b->d = 4;
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

static void draw_line_canvas1(const canvas1_t *c, int x0, int y0, int x1, int y1, int width) {
    if (width <= 1) {
        canvas1_line(c, x0, y0, x1, y1, CANVAS1_BLACK);
    } else {
        for (int wy = 0; wy < width; wy++) {
            for (int wx = 0; wx < width; wx++) {
                canvas1_line(c, x0 + wx, y0 + wy, x1 + wx, y1 + wy, CANVAS1_BLACK);
            }
        }
    }
}

static void draw_line_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                            int x0, int y0, int x1, int y1, uint8_t fg, int width) {
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        for (int wy = 0; wy < width; wy++) {
            for (int wx = 0; wx < width; wx++) {
                int px = x0 + wx;
                int py = y0 + wy;
                if (px >= 0 && px < clip_w && py >= 0 && py < clip_h) {
                    fb[(uint32_t)py * stride + px] = fg;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_line_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                             int x0, int y0, int x1, int y1, uint16_t fg, int width) {
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        for (int wy = 0; wy < width; wy++) {
            for (int wx = 0; wx < width; wx++) {
                int px = x0 + wx;
                int py = y0 + wy;
                if (px >= 0 && px < clip_w && py >= 0 && py < clip_h) {
                    fb[(uint32_t)py * stride_pixels + px] = fg;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void render_serif_scaled_canvas1(const canvas1_t *c, int abs_x, int abs_y, char ch,
                                        int dst_w, int dst_h, int base_offset, bool italic) {
    uint8_t src_w = font_serif_char_width(ch);
    const uint16_t *g = font_serif_get_glyph(ch);
    int top_y = abs_y - base_offset;

    for (int dy = 0; dy < dst_h; dy++) {
        int py = top_y + dy;
        if (py < 0 || py >= (int)c->h) continue;
        int slant = italic ? ((dst_h - 1 - dy) / 4) : 0;

        int sy0 = (dy * 16) / dst_h;
        int sy1 = ((dy + 1) * 16 + dst_h - 1) / dst_h;
        if (sy1 > 16) sy1 = 16;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int px = abs_x + dx + slant;
            if (px < 0 || px >= (int)c->w) continue;

            int sx0 = (dx * (int)src_w) / dst_w;
            int sx1 = ((dx + 1) * (int)src_w + dst_w - 1) / dst_w;
            if (sx1 > (int)src_w) sx1 = (int)src_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            bool lit = false;
            for (int sy = sy0; sy < sy1 && !lit; sy++) {
                uint16_t bits = g[sy];
                for (int sc = sx0; sc < sx1; sc++) {
                    if ((bits >> sc) & 1) {
                        lit = true;
                        break;
                    }
                }
            }
            if (lit) {
                canvas1_put_pixel(c, px, py);
            }
        }
    }
}

static void render_serif_scaled_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                      int abs_x, int abs_y, char ch, uint8_t fg, uint8_t bg,
                                      int dst_w, int dst_h, int base_offset, bool italic) {
    uint8_t src_w = font_serif_char_width(ch);
    const uint8_t *g_aa = font_serif_get_aa_glyph(ch);
    int delta = (int)fg - (int)bg;
    int top_y = abs_y - base_offset;

    for (int dy = 0; dy < dst_h; dy++) {
        int py = top_y + dy;
        if (py < 0 || py >= clip_h) continue;
        int slant = italic ? ((dst_h - 1 - dy) / 4) : 0;

        int sy0 = (dy * 16) / dst_h;
        int sy1 = ((dy + 1) * 16 + dst_h - 1) / dst_h;
        if (sy1 > 16) sy1 = 16;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int px = abs_x + dx + slant;
            if (px < 0 || px >= clip_w) continue;

            int sx0 = (dx * (int)src_w) / dst_w;
            int sx1 = ((dx + 1) * (int)src_w + dst_w - 1) / dst_w;
            if (sx1 > (int)src_w) sx1 = (int)src_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            uint8_t max_a = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *src_row = g_aa + sy * 8;
                for (int sc = sx0; sc < sx1 && sc < 16; sc++) {
                    uint8_t a = (sc & 1) ? (src_row[sc >> 1] >> 4) : (src_row[sc >> 1] & 0x0f);
                    if (a > max_a) max_a = a;
                }
            }

            if (max_a == 15) {
                fb[(uint32_t)py * stride + px] = fg;
            } else if (max_a > 0) {
                fb[(uint32_t)py * stride + px] = (uint8_t)(bg + ((delta * (int)max_a + 8) >> 4));
            }
        }
    }
}

static void render_serif_scaled_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                       int abs_x, int abs_y, char ch, uint16_t fg, uint16_t bg,
                                       int dst_w, int dst_h, int base_offset, bool italic) {
    uint8_t src_w = font_serif_char_width(ch);
    const uint8_t *g_aa = font_serif_get_aa_glyph(ch);
    int top_y = abs_y - base_offset;

    int fg_r = (fg >> 11) & 0x1f;
    int fg_g = (fg >> 5) & 0x3f;
    int fg_b = fg & 0x1f;

    int bg_r = (bg >> 11) & 0x1f;
    int bg_g = (bg >> 5) & 0x3f;
    int bg_b = bg & 0x1f;

    int dr = fg_r - bg_r;
    int dg = fg_g - bg_g;
    int db = fg_b - bg_b;

    for (int dy = 0; dy < dst_h; dy++) {
        int py = top_y + dy;
        if (py < 0 || py >= clip_h) continue;
        int slant = italic ? ((dst_h - 1 - dy) / 4) : 0;

        int sy0 = (dy * 16) / dst_h;
        int sy1 = ((dy + 1) * 16 + dst_h - 1) / dst_h;
        if (sy1 > 16) sy1 = 16;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int px = abs_x + dx + slant;
            if (px < 0 || px >= clip_w) continue;

            int sx0 = (dx * (int)src_w) / dst_w;
            int sx1 = ((dx + 1) * (int)src_w + dst_w - 1) / dst_w;
            if (sx1 > (int)src_w) sx1 = (int)src_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            uint8_t max_a = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint8_t *src_row = g_aa + sy * 8;
                for (int sc = sx0; sc < sx1 && sc < 16; sc++) {
                    uint8_t a = (sc & 1) ? (src_row[sc >> 1] >> 4) : (src_row[sc >> 1] & 0x0f);
                    if (a > max_a) max_a = a;
                }
            }

            if (max_a == 15) {
                fb[(uint32_t)py * stride_pixels + px] = fg;
            } else if (max_a > 0) {
                int r_val = bg_r + ((dr * (int)max_a + 8) >> 4);
                int g_val = bg_g + ((dg * (int)max_a + 8) >> 4);
                int b_val = bg_b + ((db * (int)max_a + 8) >> 4);
                fb[(uint32_t)py * stride_pixels + px] = (uint16_t)((r_val << 11) | (g_val << 5) | b_val);
            }
        }
    }
}

static void render_glyph_scaled_1bpp(const canvas1_t *c, int abs_x, int abs_y,
                                     const uint16_t *rows, int src_w, int src_h,
                                     int dst_w, int dst_h, int bearing_y_scaled) {
    int top_y = abs_y - bearing_y_scaled;
    for (int dy = 0; dy < dst_h; dy++) {
        int py = top_y + dy;
        if (py < 0 || py >= (int)c->h) continue;

        int sy0 = (dy * src_h) / dst_h;
        int sy1 = ((dy + 1) * src_h + dst_h - 1) / dst_h;
        if (sy1 > src_h) sy1 = src_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int px = abs_x + dx;
            if (px < 0 || px >= (int)c->w) continue;

            int sx0 = (dx * src_w) / dst_w;
            int sx1 = ((dx + 1) * src_w + dst_w - 1) / dst_w;
            if (sx1 > src_w) sx1 = src_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            bool lit = false;
            for (int sy = sy0; sy < sy1 && !lit; sy++) {
                uint16_t bits = rows[sy];
                for (int sc = sx0; sc < sx1; sc++) {
                    if (bits & (0x8000 >> sc)) {
                        lit = true;
                        break;
                    }
                }
            }
            if (lit) {
                canvas1_put_pixel(c, px, py);
            }
        }
    }
}

static void render_glyph_scaled_gray8(uint8_t *fb, uint32_t stride, int clip_w, int clip_h,
                                      int abs_x, int abs_y, const uint16_t *rows,
                                      int src_w, int src_h, int dst_w, int dst_h,
                                      int bearing_y_scaled, uint8_t fg) {
    int top_y = abs_y - bearing_y_scaled;
    for (int dy = 0; dy < dst_h; dy++) {
        int py = top_y + dy;
        if (py < 0 || py >= clip_h) continue;

        int sy0 = (dy * src_h) / dst_h;
        int sy1 = ((dy + 1) * src_h + dst_h - 1) / dst_h;
        if (sy1 > src_h) sy1 = src_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int px = abs_x + dx;
            if (px < 0 || px >= clip_w) continue;

            int sx0 = (dx * src_w) / dst_w;
            int sx1 = ((dx + 1) * src_w + dst_w - 1) / dst_w;
            if (sx1 > src_w) sx1 = src_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            bool lit = false;
            for (int sy = sy0; sy < sy1 && !lit; sy++) {
                uint16_t bits = rows[sy];
                for (int sc = sx0; sc < sx1; sc++) {
                    if (bits & (0x8000 >> sc)) {
                        lit = true;
                        break;
                    }
                }
            }
            if (lit) {
                fb[(uint32_t)py * stride + px] = fg;
            }
        }
    }
}

static void render_glyph_scaled_rgb565(uint16_t *fb, uint32_t stride_pixels, int clip_w, int clip_h,
                                       int abs_x, int abs_y, const uint16_t *rows,
                                       int src_w, int src_h, int dst_w, int dst_h,
                                       int bearing_y_scaled, uint16_t fg) {
    int top_y = abs_y - bearing_y_scaled;
    for (int dy = 0; dy < dst_h; dy++) {
        int py = top_y + dy;
        if (py < 0 || py >= clip_h) continue;

        int sy0 = (dy * src_h) / dst_h;
        int sy1 = ((dy + 1) * src_h + dst_h - 1) / dst_h;
        if (sy1 > src_h) sy1 = src_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int px = abs_x + dx;
            if (px < 0 || px >= clip_w) continue;

            int sx0 = (dx * src_w) / dst_w;
            int sx1 = ((dx + 1) * src_w + dst_w - 1) / dst_w;
            if (sx1 > src_w) sx1 = src_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            bool lit = false;
            for (int sy = sy0; sy < sy1 && !lit; sy++) {
                uint16_t bits = rows[sy];
                for (int sc = sx0; sc < sx1; sc++) {
                    if (bits & (0x8000 >> sc)) {
                        lit = true;
                        break;
                    }
                }
            }
            if (lit) {
                fb[(uint32_t)py * stride_pixels + px] = fg;
            }
        }
    }
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
        int scale = (b->scale > 0) ? b->scale : 1;
        bool italic = (b->u.ch.font_family == 0 &&
                       ((b->u.ch.codepoint >= 'a' && b->u.ch.codepoint <= 'z') ||
                        (b->u.ch.codepoint >= 'A' && b->u.ch.codepoint <= 'Z')));
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g && g->rows) {
            if (scale > 1) {
                int glyph_scale = (b->style == STYLE_SCRIPT || b->style == STYLE_SCRIPTSCRIPT) ? 1 : scale;
                int gy = abs_y - g->bearing_y * glyph_scale;
                render_glyph_1bpp(c, abs_x, gy, g->rows, g->w, g->h, glyph_scale);
            } else {
                if (b->style == STYLE_SCRIPT) {
                    render_glyph_scaled_1bpp(c, abs_x, abs_y, g->rows, g->w, g->h,
                                             b->w, (g->h * 7 + 5) / 10, (g->bearing_y * 7 + 5) / 10);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_glyph_scaled_1bpp(c, abs_x, abs_y, g->rows, g->w, g->h,
                                             b->w, (g->h * 5 + 5) / 10, (g->bearing_y * 5 + 5) / 10);
                } else {
                    int gy = abs_y - g->bearing_y;
                    render_glyph_1bpp(c, abs_x, gy, g->rows, g->w, g->h, 1);
                }
            }
        } else {
#if MATH_USE_HIDPI
            if (scale > 1) {
                if (b->style == STYLE_SCRIPT) {
                    canvas1_serif_glyph(c, abs_x, abs_y - 12, (char)b->u.ch.codepoint, false, italic);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_serif_scaled_canvas1(c, abs_x, abs_y, (char)b->u.ch.codepoint,
                                                b->w, 10, 8, italic);
                } else {
                    canvas1_noto_serif_glyph(c, abs_x, abs_y - 24, (char)b->u.ch.codepoint, false, italic);
                }
            } else
#endif
            {
                if (b->style == STYLE_SCRIPT) {
                    render_serif_scaled_canvas1(c, abs_x, abs_y, (char)b->u.ch.codepoint,
                                                b->w, 10, 8, italic);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_serif_scaled_canvas1(c, abs_x, abs_y, (char)b->u.ch.codepoint,
                                                b->w, 7, 5, italic);
                } else {
                    canvas1_serif_glyph(c, abs_x, abs_y - 12, (char)b->u.ch.codepoint, false, italic);
                }
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
            int scale = (b->scale > 0) ? b->scale : 1;
            int axis = font_math_axis_height((math_style_t)b->style) * scale;
            int bar_y = abs_y - axis;
            for (int dy = 0; dy < scale; dy++) {
                canvas1_hline(c, abs_x, abs_x + b->w - 1, bar_y + dy, CANVAS1_BLACK);
            }
        }
        break;
    }

    case MATH_BOX_RADICAL: {
        int scale = (b->scale > 0) ? b->scale : 1;
        if (b->u.radical.body) {
            latex_render_canvas1(b->u.radical.body, abs_x, abs_y, c, fg);
        }
        int top_y = abs_y - b->h;
        int bot_y = abs_y + b->d;
        int mid_y = bot_y - (b->h + b->d) / 3;

        /* Vinculum overbar */
        for (int dy = 0; dy < scale; dy++) {
            canvas1_hline(c, abs_x + 8 * scale, abs_x + b->w - 1, top_y + dy, CANVAS1_BLACK);
        }
        /* Left entry tick */
        for (int dy = 0; dy < scale; dy++) {
            canvas1_hline(c, abs_x, abs_x + 1 * scale, mid_y + dy, CANVAS1_BLACK);
        }
        /* Hook down to bottom */
        draw_line_canvas1(c, abs_x + 1 * scale, mid_y, abs_x + 4 * scale, bot_y, scale);
        /* Main diagonal stem up to vinculum */
        draw_line_canvas1(c, abs_x + 4 * scale, bot_y, abs_x + 8 * scale, top_y, scale);
        break;
    }

    case MATH_BOX_SCRIPT: {
        if (b->u.script.nucleus) latex_render_canvas1(b->u.script.nucleus, abs_x, abs_y, c, fg);
        if (b->u.script.sup) latex_render_canvas1(b->u.script.sup, abs_x, abs_y, c, fg);
        if (b->u.script.sub) latex_render_canvas1(b->u.script.sub, abs_x, abs_y, c, fg);
        break;
    }

    case MATH_BOX_BIGOP: {
        int scale = (b->scale > 0) ? b->scale : 1;
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
        int scale = (b->scale > 0) ? b->scale : 1;
        int top = abs_y - b->h;
        int bot = abs_y + b->d;
        for (int dy = 0; dy < scale; dy++) {
            canvas1_vline(c, abs_x + 2 * scale + dy, top, bot, CANVAS1_BLACK);
        }
        canvas1_put_pixel(c, abs_x + 3 * scale, top);
        canvas1_put_pixel(c, abs_x + 3 * scale, bot);
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

    int abs_x = origin_x + b->x;
    int abs_y = baseline_y - b->y;

    switch (b->type) {
    case MATH_BOX_CHAR: {
        int scale = (b->scale > 0) ? b->scale : 1;
        bool italic = (b->u.ch.font_family == 0 &&
                       ((b->u.ch.codepoint >= 'a' && b->u.ch.codepoint <= 'z') ||
                        (b->u.ch.codepoint >= 'A' && b->u.ch.codepoint <= 'Z')));
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g && g->rows) {
            if (scale > 1) {
                int glyph_scale = (b->style == STYLE_SCRIPT || b->style == STYLE_SCRIPTSCRIPT) ? 1 : scale;
                int gy = abs_y - g->bearing_y * glyph_scale;
                for (int r = 0; r < g->h; r++) {
                    uint16_t bits = g->rows[r];
                    for (int col = 0; col < g->w; col++) {
                        if (bits & (0x8000 >> col)) {
                            for (int dy = 0; dy < glyph_scale; dy++) {
                                int py = gy + r * glyph_scale + dy;
                                if (py < 0 || py >= clip_h) continue;
                                for (int dx = 0; dx < glyph_scale; dx++) {
                                    int px = abs_x + col * glyph_scale + dx;
                                    if (px >= 0 && px < clip_w) {
                                        fb[(uint32_t)py * stride + px] = fg;
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                if (b->style == STYLE_SCRIPT) {
                    render_glyph_scaled_gray8(fb, stride, clip_w, clip_h, abs_x, abs_y,
                                              g->rows, g->w, g->h, b->w, (g->h * 7 + 5) / 10,
                                              (g->bearing_y * 7 + 5) / 10, fg);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_glyph_scaled_gray8(fb, stride, clip_w, clip_h, abs_x, abs_y,
                                              g->rows, g->w, g->h, b->w, (g->h * 5 + 5) / 10,
                                              (g->bearing_y * 5 + 5) / 10, fg);
                } else {
                    int gy = abs_y - g->bearing_y;
                    for (int r = 0; r < g->h; r++) {
                        uint16_t bits = g->rows[r];
                        for (int col = 0; col < g->w; col++) {
                            if (bits & (0x8000 >> col)) {
                                int py = gy + r;
                                if (py >= 0 && py < clip_h) {
                                    int px = abs_x + col;
                                    if (px >= 0 && px < clip_w) {
                                        fb[(uint32_t)py * stride + px] = fg;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
#if MATH_USE_HIDPI
            if (scale > 1) {
                if (b->style == STYLE_SCRIPT) {
                    font_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h,
                                              abs_x, abs_y - 12, (char)b->u.ch.codepoint, fg, bg, false, italic);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_serif_scaled_gray8(fb, stride, clip_w, clip_h,
                                              abs_x, abs_y, (char)b->u.ch.codepoint, fg, bg,
                                              b->w, 10, 8, italic);
                } else {
                    font_noto_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h,
                                                   abs_x, abs_y - 24, (char)b->u.ch.codepoint, fg, bg, false, italic);
                }
            } else
#endif
            {
                if (b->style == STYLE_SCRIPT) {
                    render_serif_scaled_gray8(fb, stride, clip_w, clip_h,
                                              abs_x, abs_y, (char)b->u.ch.codepoint, fg, bg,
                                              b->w, 10, 8, italic);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_serif_scaled_gray8(fb, stride, clip_w, clip_h,
                                              abs_x, abs_y, (char)b->u.ch.codepoint, fg, bg,
                                              b->w, 7, 5, italic);
                } else {
                    font_serif_aa_glyph_gray8(fb, stride, clip_w, clip_h,
                                              abs_x, abs_y - 12, (char)b->u.ch.codepoint, fg, bg, false, italic);
                }
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
            int scale = (b->scale > 0) ? b->scale : 1;
            int axis = font_math_axis_height((math_style_t)b->style) * scale;
            int bar_y = abs_y - axis;
            for (int dy = 0; dy < scale; dy++) {
                int py = bar_y + dy;
                if (py >= 0 && py < clip_h) {
                    for (int k = abs_x; k < abs_x + b->w && k < clip_w; k++) {
                        if (k >= 0) fb[(uint32_t)py * stride + k] = fg;
                    }
                }
            }
        }
        return;
    }

    case MATH_BOX_RADICAL: {
        int scale = (b->scale > 0) ? b->scale : 1;
        if (b->u.radical.body) latex_render_gray8(b->u.radical.body, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        int top_y = abs_y - b->h;
        int bot_y = abs_y + b->d;
        int mid_y = bot_y - (b->h + b->d) / 3;

        /* Vinculum overbar */
        for (int dy = 0; dy < scale; dy++) {
            int py = top_y + dy;
            if (py >= 0 && py < clip_h) {
                for (int k = abs_x + 8 * scale; k < abs_x + b->w && k < clip_w; k++) {
                    if (k >= 0) fb[(uint32_t)py * stride + k] = fg;
                }
            }
        }
        /* Left entry tick */
        for (int dy = 0; dy < scale; dy++) {
            int py = mid_y + dy;
            if (py >= 0 && py < clip_h) {
                for (int k = abs_x; k < abs_x + 1 * scale && k < clip_w; k++) {
                    if (k >= 0) fb[(uint32_t)py * stride + k] = fg;
                }
            }
        }
        /* Hook down to bottom */
        draw_line_gray8(fb, stride, clip_w, clip_h, abs_x + 1 * scale, mid_y, abs_x + 4 * scale, bot_y, fg, scale);
        /* Main diagonal stem up to vinculum */
        draw_line_gray8(fb, stride, clip_w, clip_h, abs_x + 4 * scale, bot_y, abs_x + 8 * scale, top_y, fg, scale);
        return;
    }

    case MATH_BOX_SCRIPT: {
        if (b->u.script.nucleus) latex_render_gray8(b->u.script.nucleus, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.script.sup) latex_render_gray8(b->u.script.sup, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        if (b->u.script.sub) latex_render_gray8(b->u.script.sub, abs_x, abs_y, fb, stride, clip_w, clip_h, fg, bg);
        return;
    }

    case MATH_BOX_BIGOP: {
        int scale = (b->scale > 0) ? b->scale : 1;
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
                                    fb[(uint32_t)py * stride + px] = fg;
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
        int scale = (b->scale > 0) ? b->scale : 1;
        int top = abs_y - b->h;
        int bot = abs_y + b->d;
        for (int y = top; y <= bot && y < clip_h; y++) {
            if (y >= 0) {
                for (int dx = 0; dx < scale; dx++) {
                    int px = abs_x + 2 * scale + dx;
                    if (px >= 0 && px < clip_w) fb[(uint32_t)y * stride + px] = fg;
                }
            }
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
        int scale = (b->scale > 0) ? b->scale : 1;
        bool italic = (b->u.ch.font_family == 0 &&
                       ((b->u.ch.codepoint >= 'a' && b->u.ch.codepoint <= 'z') ||
                        (b->u.ch.codepoint >= 'A' && b->u.ch.codepoint <= 'Z')));
        const math_glyph_t *g = font_math_lookup(b->u.ch.codepoint, (math_style_t)b->style);
        if (g && g->rows) {
            if (scale > 1) {
                int glyph_scale = (b->style == STYLE_SCRIPT || b->style == STYLE_SCRIPTSCRIPT) ? 1 : scale;
                int gy = abs_y - g->bearing_y * glyph_scale;
                for (int r = 0; r < g->h; r++) {
                    uint16_t bits = g->rows[r];
                    for (int col = 0; col < g->w; col++) {
                        if (bits & (0x8000 >> col)) {
                            for (int dy = 0; dy < glyph_scale; dy++) {
                                int py = gy + r * glyph_scale + dy;
                                if (py < 0 || py >= clip_h) continue;
                                for (int dx = 0; dx < glyph_scale; dx++) {
                                    int px = abs_x + col * glyph_scale + dx;
                                    if (px >= 0 && px < clip_w) {
                                        fb[(uint32_t)py * stride_pixels + px] = fg;
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                if (b->style == STYLE_SCRIPT) {
                    render_glyph_scaled_rgb565(fb, stride_pixels, clip_w, clip_h, abs_x, abs_y,
                                               g->rows, g->w, g->h, b->w, (g->h * 7 + 5) / 10,
                                               (g->bearing_y * 7 + 5) / 10, fg);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_glyph_scaled_rgb565(fb, stride_pixels, clip_w, clip_h, abs_x, abs_y,
                                               g->rows, g->w, g->h, b->w, (g->h * 5 + 5) / 10,
                                               (g->bearing_y * 5 + 5) / 10, fg);
                } else {
                    int gy = abs_y - g->bearing_y;
                    for (int r = 0; r < g->h; r++) {
                        uint16_t bits = g->rows[r];
                        for (int col = 0; col < g->w; col++) {
                            if (bits & (0x8000 >> col)) {
                                int py = gy + r;
                                if (py >= 0 && py < clip_h) {
                                    int px = abs_x + col;
                                    if (px >= 0 && px < clip_w) {
                                        fb[(uint32_t)py * stride_pixels + px] = fg;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
#if MATH_USE_HIDPI
            if (scale > 1) {
                if (b->style == STYLE_SCRIPT) {
                    font_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h,
                                               abs_x, abs_y - 12, (char)b->u.ch.codepoint, fg, bg, false, italic);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_serif_scaled_rgb565(fb, stride_pixels, clip_w, clip_h,
                                               abs_x, abs_y, (char)b->u.ch.codepoint, fg, bg,
                                               b->w, 10, 8, italic);
                } else {
                    font_noto_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h,
                                                    abs_x, abs_y - 24, (char)b->u.ch.codepoint, fg, bg, false, italic);
                }
            } else
#endif
            {
                if (b->style == STYLE_SCRIPT) {
                    render_serif_scaled_rgb565(fb, stride_pixels, clip_w, clip_h,
                                               abs_x, abs_y, (char)b->u.ch.codepoint, fg, bg,
                                               b->w, 10, 8, italic);
                } else if (b->style == STYLE_SCRIPTSCRIPT) {
                    render_serif_scaled_rgb565(fb, stride_pixels, clip_w, clip_h,
                                               abs_x, abs_y, (char)b->u.ch.codepoint, fg, bg,
                                               b->w, 7, 5, italic);
                } else {
                    font_serif_aa_glyph_rgb565(fb, stride_pixels, clip_w, clip_h,
                                               abs_x, abs_y - 12, (char)b->u.ch.codepoint, fg, bg, false, italic);
                }
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
            int scale = (b->scale > 0) ? b->scale : 1;
            int axis = font_math_axis_height((math_style_t)b->style) * scale;
            int bar_y = abs_y - axis;
            for (int dy = 0; dy < scale; dy++) {
                int py = bar_y + dy;
                if (py >= 0 && py < clip_h) {
                    for (int k = abs_x; k < abs_x + b->w && k < clip_w; k++) {
                        if (k >= 0) fb[(uint32_t)py * stride_pixels + k] = fg;
                    }
                }
            }
        }
        return;
    }

    case MATH_BOX_RADICAL: {
        int scale = (b->scale > 0) ? b->scale : 1;
        if (b->u.radical.body) latex_render_rgb565(b->u.radical.body, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        int top_y = abs_y - b->h;
        int bot_y = abs_y + b->d;
        int mid_y = bot_y - (b->h + b->d) / 3;

        /* Vinculum overbar */
        for (int dy = 0; dy < scale; dy++) {
            int py = top_y + dy;
            if (py >= 0 && py < clip_h) {
                for (int k = abs_x + 8 * scale; k < abs_x + b->w && k < clip_w; k++) {
                    if (k >= 0) fb[(uint32_t)py * stride_pixels + k] = fg;
                }
            }
        }
        /* Left entry tick */
        for (int dy = 0; dy < scale; dy++) {
            int py = mid_y + dy;
            if (py >= 0 && py < clip_h) {
                for (int k = abs_x; k < abs_x + 1 * scale && k < clip_w; k++) {
                    if (k >= 0) fb[(uint32_t)py * stride_pixels + k] = fg;
                }
            }
        }
        /* Hook down to bottom */
        draw_line_rgb565(fb, stride_pixels, clip_w, clip_h, abs_x + 1 * scale, mid_y, abs_x + 4 * scale, bot_y, fg, scale);
        /* Main diagonal stem up to vinculum */
        draw_line_rgb565(fb, stride_pixels, clip_w, clip_h, abs_x + 4 * scale, bot_y, abs_x + 8 * scale, top_y, fg, scale);
        return;
    }

    case MATH_BOX_SCRIPT: {
        if (b->u.script.nucleus) latex_render_rgb565(b->u.script.nucleus, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.script.sup) latex_render_rgb565(b->u.script.sup, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        if (b->u.script.sub) latex_render_rgb565(b->u.script.sub, abs_x, abs_y, fb, stride_pixels, clip_w, clip_h, fg, bg);
        return;
    }

    case MATH_BOX_BIGOP: {
        int scale = (b->scale > 0) ? b->scale : 1;
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
                                    fb[(uint32_t)py * stride_pixels + px] = fg;
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
        int scale = (b->scale > 0) ? b->scale : 1;
        int top = abs_y - b->h;
        int bot = abs_y + b->d;
        for (int y = top; y <= bot && y < clip_h; y++) {
            if (y >= 0) {
                for (int dx = 0; dx < scale; dx++) {
                    int px = abs_x + 2 * scale + dx;
                    if (px >= 0 && px < clip_w) fb[(uint32_t)y * stride_pixels + px] = fg;
                }
            }
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
