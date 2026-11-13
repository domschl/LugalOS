#ifndef LUGALOS_DRIVERS_TTF_CACHE_H
#define LUGALOS_DRIVERS_TTF_CACHE_H

#include "drivers/ttf.h"
#include <stdbool.h>
#include <stdint.h>

/* PSRAM-backed TrueType font loader and 4-bit AA glyph cache.
 * Zero Flash/SRAM cost on non-PSRAM targets (RP2350, ESP32-C6). */

bool ttf_cache_available(void);
bool ttf_cache_load(const char *path);
void ttf_cache_unload(void);
bool ttf_cache_ensure_default(void);
void ttf_cache_reset_probe(void);

/* Lookup or rasterize a glyph at `cell_h` px; returns NULL only if no font
 * is loaded or the cache cannot allocate PSRAM. `out_aa` receives the
 * 4-bit packed coverage pointer (NULL for empty glyphs like space). */
const ttf_glyph_metrics_t *ttf_cache_get(int cell_h, uint32_t codepoint,
                                         const uint8_t **out_aa);

/* Pre-rasterize ASCII 0x20..0x7E for `cell_h` px. */
void ttf_cache_warm_ascii(int cell_h);

/* Cache statistics for `ttf` / `reader` diagnostics. */
void ttf_cache_stats(uint32_t *out_hits, uint32_t *out_misses,
                     uint32_t *out_entries, uint32_t *out_arena_used,
                     uint32_t *out_arena_cap);

#endif /* LUGALOS_DRIVERS_TTF_CACHE_H */
