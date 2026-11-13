#ifndef LUGALOS_DRIVERS_TTF_CACHE_H
#define LUGALOS_DRIVERS_TTF_CACHE_H

#include "drivers/ttf.h"
#include "drivers/ttf_ot.h"
#include <stdbool.h>
#include <stdint.h>

/* PSRAM-backed TrueType font loader, OpenType shaper, and 4-bit AA glyph
 * cache. Zero Flash/SRAM cost on non-PSRAM targets (RP2350, ESP32-C6). */

#define TTF_FONT_PRIMARY  0u
#define TTF_FONT_TIBETAN  1u
#define TTF_FONT_SLOTS    2u
#define TTF_MAX_SHAPED    128

typedef struct {
    uint16_t gid;
    int16_t  kern_px;
} ttf_shaped_glyph_t;

bool ttf_cache_available(void);
bool ttf_cache_load(const char *path);
bool ttf_cache_load_slot(uint8_t font_id, const char *path, uint32_t script_tag);
void ttf_cache_unload(void);
bool ttf_cache_ensure_default(void);
bool ttf_cache_ensure_tibetan(void);
void ttf_cache_reset_probe(void);

/* Lookup or rasterize a glyph by codepoint or shaped glyph ID at `cell_h` px;
 * returns NULL only if no font is loaded or the cache cannot allocate PSRAM.
 * `out_aa` receives the 4-bit packed coverage pointer (NULL for empty glyphs). */
const ttf_glyph_metrics_t *ttf_cache_get(int cell_h, uint32_t codepoint,
                                         const uint8_t **out_aa);
const ttf_glyph_metrics_t *ttf_cache_get_gid(uint8_t font_id, int cell_h,
                                             uint16_t gid, const uint8_t **out_aa);

/* Shape a single-script codepoint run (`cps[0..count-1]`) using GSUB/GPOS
 * and return the font slot used (`out_font_id`) + shaped glyph count. */
int ttf_cache_shape_run(const uint32_t *cps, int count, int cell_h,
                        uint8_t *out_font_id, ttf_shaped_glyph_t *out, int max_out);

/* Pre-rasterize ASCII 0x20..0x7E for `cell_h` px. */
void ttf_cache_warm_ascii(int cell_h);

/* Cache statistics for `ttf` / `reader` diagnostics. */
void ttf_cache_stats(uint32_t *out_hits, uint32_t *out_misses,
                     uint32_t *out_entries, uint32_t *out_arena_used,
                     uint32_t *out_arena_cap);

#endif /* LUGALOS_DRIVERS_TTF_CACHE_H */
