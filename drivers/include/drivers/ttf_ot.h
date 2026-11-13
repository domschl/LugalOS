#ifndef LUGALOS_DRIVERS_TTF_OT_H
#define LUGALOS_DRIVERS_TTF_OT_H

#include "drivers/ttf.h"
#include <stdbool.h>
#include <stdint.h>

/* Freestanding OpenType GSUB/GPOS/kern shaping subset for TrueType fonts.
 * Supports GSUB types 1 (Single), 2 (Multiple), 4 (Ligature), 6 (Chaining
 * Contextual), 7 (Extension), and GPOS type 2 (PairPos kern) + legacy kern. */

typedef struct {
    uint8_t *gsub;
    uint32_t gsub_len;
    uint8_t *gpos;
    uint32_t gpos_len;
    uint8_t *kern;
    uint32_t kern_len;
    uint32_t script_tag;        /* e.g. 'tibt', 'latn', 'DFLT' */
} ttf_ot_t;

/* Compute total PSRAM/heap bytes needed to hold GSUB + GPOS + kern tables. */
uint32_t ttf_ot_required_bytes(const ttf_font_t *f);

/* Load GSUB/GPOS/kern tables from `f` into caller-provided `buf` (`buf_cap`
 * bytes). `script_tag` selects the primary OpenType script (falls back to
 * 'DFLT' or first script). */
bool ttf_ot_init(ttf_ot_t *ot, const ttf_font_t *f, uint32_t script_tag,
                 uint8_t *buf, uint32_t buf_cap);

/* Shape `count` Unicode codepoints into glyph IDs + per-glyph horizontal kerning
 * in font units (`out_kern_fu`, optional NULL). Returns shaped glyph count. */
int ttf_ot_shape(const ttf_font_t *f, const ttf_ot_t *ot,
                 const uint32_t *cps, int count,
                 uint16_t *out_gids, int16_t *out_kern_fu, int max_out);

/* Scale a font-unit kerning delta to integer pixels for `cell_h`. */
int ttf_ot_scale_fu(const ttf_font_t *f, int cell_h, int32_t fu);

#endif /* LUGALOS_DRIVERS_TTF_OT_H */
