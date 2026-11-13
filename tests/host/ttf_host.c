#include "drivers/ttf.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* Construct a minimal valid TrueType font in `buf` with 3 glyphs:
 * 0: .notdef (empty)
 * 1: 'A' (0x41) — filled 500x700 rectangle (on-curve square)
 * 2: 'B' (0x42) — compound glyph referencing glyph 1 shifted by (100, 0)
 */
static uint32_t build_synthetic_ttf(uint8_t *buf, uint32_t cap) {
    memset(buf, 0, cap);
    const uint16_t num_tables = 7;
    uint32_t off = 12u + num_tables * 16u;

    put_be32(buf + 0, 0x00010000u);
    put_be16(buf + 4, num_tables);

    /* Table payloads */
    /* 1. head (54 bytes) */
    uint32_t head_off = off;
    put_be16(buf + head_off + 18, 1000); /* unitsPerEm */
    put_be16(buf + head_off + 50, 1);    /* indexToLocFormat = long */
    off += 56;

    /* 2. hhea (36 bytes) */
    uint32_t hhea_off = off;
    put_be16(buf + hhea_off + 4, 800);             /* ascent */
    put_be16(buf + hhea_off + 6, (uint16_t)-200);  /* descent */
    put_be16(buf + hhea_off + 34, 3);              /* numberOfHMetrics */
    off += 36;

    /* 3. maxp (6 bytes) */
    uint32_t maxp_off = off;
    put_be16(buf + maxp_off + 4, 3); /* numGlyphs */
    off += 8;

    /* 4. hmtx (3 * 4 = 12 bytes) */
    uint32_t hmtx_off = off;
    put_be16(buf + hmtx_off + 0, 500); /* gid 0 */
    put_be16(buf + hmtx_off + 4, 600); /* gid 1 ('A') */
    put_be16(buf + hmtx_off + 8, 700); /* gid 2 ('B') */
    off += 12;

    /* 5. cmap (format 4 mapping 0x41->1, 0x42->2) */
    uint32_t cmap_off = off;
    put_be16(buf + cmap_off + 2, 1);  /* numTables */
    put_be16(buf + cmap_off + 4, 3);  /* platformID = 3 */
    put_be16(buf + cmap_off + 6, 1);  /* encodingID = 1 */
    put_be32(buf + cmap_off + 8, 12); /* subtable offset */
    uint32_t sub = cmap_off + 12;
    put_be16(buf + sub + 0, 4);   /* format 4 */
    put_be16(buf + sub + 2, 32);  /* length */
    put_be16(buf + sub + 6, 4);   /* segCountX2 = 2 * 2 */
    /* endCount: 0x0042, 0xFFFF */
    put_be16(buf + sub + 14, 0x0042);
    put_be16(buf + sub + 16, 0xFFFF);
    /* startCount: 0x0041, 0xFFFF */
    put_be16(buf + sub + 20, 0x0041);
    put_be16(buf + sub + 22, 0xFFFF);
    /* idDelta: -0x40 (maps 0x41->1, 0x42->2), 1 */
    put_be16(buf + sub + 24, (uint16_t)(-0x40));
    put_be16(buf + sub + 26, 1);
    /* idRangeOffset: 0, 0 */
    off += 44;

    /* 6. glyf */
    uint32_t glyf_off = off;
    /* gid 0: empty (0 bytes) */
    uint32_t g1_rel = 0;
    /* gid 1: simple 1-contour 4-point clockwise square (100,0)->(100,700)->(500,700)->(500,0) */
    uint32_t g1 = glyf_off + g1_rel;
    put_be16(buf + g1 + 0, 1);   /* numberOfContours = 1 */
    put_be16(buf + g1 + 10, 3);  /* endPtsOfContours[0] = 3 */
    put_be16(buf + g1 + 12, 0);  /* instructionLength = 0 */
    /* flags: 4 points, all on-curve (0x01) */
    buf[g1 + 14] = 0x01;
    buf[g1 + 15] = 0x01;
    buf[g1 + 16] = 0x01;
    buf[g1 + 17] = 0x01;
    /* x deltas (16-bit): +100, 0, +400, 0 */
    put_be16(buf + g1 + 18, 100);
    put_be16(buf + g1 + 20, 0);
    put_be16(buf + g1 + 22, 400);
    put_be16(buf + g1 + 24, 0);
    /* y deltas (16-bit): 0, +700, 0, -700 */
    put_be16(buf + g1 + 26, 0);
    put_be16(buf + g1 + 28, 700);
    put_be16(buf + g1 + 30, 0);
    put_be16(buf + g1 + 32, (uint16_t)-700);
    uint32_t g2_rel = 34;

    /* gid 2: compound glyph referencing gid 1 with dx=100, dy=0 */
    uint32_t g2 = glyf_off + g2_rel;
    put_be16(buf + g2 + 0, (uint16_t)-1); /* numberOfContours = -1 */
    put_be16(buf + g2 + 10, 0x0002u);    /* ARGS_ARE_XY_VALUES */
    put_be16(buf + g2 + 12, 1);          /* glyphIndex = 1 */
    buf[g2 + 14] = 100;                  /* dx = 100 */
    buf[g2 + 15] = 0;                    /* dy = 0 */
    uint32_t glyf_len = g2_rel + 16;
    off += glyf_len;

    /* 7. loca (4 * 4 = 16 bytes) */
    uint32_t loca_off = off;
    put_be32(buf + loca_off + 0, 0);
    put_be32(buf + loca_off + 4, g1_rel);
    put_be32(buf + loca_off + 8, g2_rel);
    put_be32(buf + loca_off + 12, glyf_len);
    off += 16;

    /* Directory entries */
    struct { uint32_t tag, off, len; } dirs[7] = {
        { 0x636d6170u, cmap_off, 44 },
        { 0x676c7966u, glyf_off, glyf_len },
        { 0x68656164u, head_off, 54 },
        { 0x68686561u, hhea_off, 36 },
        { 0x686d7478u, hmtx_off, 12 },
        { 0x6c6f6361u, loca_off, 16 },
        { 0x6d617870u, maxp_off, 6 },
    };
    for (int i = 0; i < 7; i++) {
        uint8_t *d = buf + 12u + (uint32_t)i * 16u;
        put_be32(d + 0, dirs[i].tag);
        put_be32(d + 8, dirs[i].off);
        put_be32(d + 12, dirs[i].len);
    }
    return off;
}

static void test_synthetic(void) {
    uint8_t ttf_buf[512];
    uint32_t sz = build_synthetic_ttf(ttf_buf, sizeof(ttf_buf));

    ttf_font_t font;
    assert(ttf_open_mem(&font, ttf_buf, sz));
    assert(ttf_glyph_index(&font, 'A') == 1);
    assert(ttf_glyph_index(&font, 'B') == 2);
    assert(ttf_glyph_index(&font, 'Z') == 0);
    assert(ttf_advance_px(&font, 32, 'A') == 19);

    ttf_scratch_t *sc = calloc(1, sizeof(ttf_scratch_t));
    assert(sc != NULL);
    uint8_t aa[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H / 2];
    ttf_glyph_metrics_t m;

    assert(ttf_rasterize_glyph(&font, sc, 32, 'A', &m, aa, sizeof(aa)));
    assert(m.w > 0 && m.h > 0);
    /* Verify interior pixel is fully covered (15) */
    int mid_y = m.h / 2;
    int mid_x = m.w / 2;
    uint8_t byte = aa[mid_y * m.stride + (mid_x >> 1)];
    uint8_t cov = (mid_x & 1) ? (byte >> 4) : (byte & 0x0F);
    assert(cov == 15);

    /* Compound glyph 'B' */
    ttf_glyph_metrics_t mb;
    assert(ttf_rasterize_glyph(&font, sc, 32, 'B', &mb, aa, sizeof(aa)));
    assert(mb.w > 0 && mb.h > 0);
    assert(mb.xoff > m.xoff);

    free(sc);
}

static void test_noto_serif_if_present(void) {
    const char *path = "/usr/share/fonts/noto/NotoSerif-Regular.ttf";
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    assert(fsz > 0 && fsz < 8 * 1024 * 1024);
    uint8_t *data = malloc((size_t)fsz);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)fsz, fp) == (size_t)fsz);
    fclose(fp);

    ttf_font_t font;
    assert(ttf_open_mem(&font, data, (uint32_t)fsz));

    ttf_scratch_t *sc = calloc(1, sizeof(ttf_scratch_t));
    assert(sc != NULL);
    uint8_t aa[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H / 2];
    ttf_glyph_metrics_t m;

    const uint32_t cps[] = {
        ' ', 'A', 'g', 'e', 0x00E4 /* ä */, 0x00F6 /* ö */, 0x00FC /* ü */,
        0x00DF /* ß */, 0x2014 /* — */, 0x201C /* “ */, 0x2026 /* … */,
        0x03A9 /* Ω */, 0x0414 /* Д */
    };
    for (size_t i = 0; i < sizeof(cps) / sizeof(cps[0]); i++) {
        assert(ttf_glyph_index(&font, cps[i]) != 0);
        assert(ttf_rasterize_glyph(&font, sc, 32, cps[i], &m, aa, sizeof(aa)));
        assert(m.advance > 0);
        if (cps[i] != ' ') {
            assert(m.w > 0 && m.h > 0);
        }
    }

    free(sc);
    free(data);
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void test_fuzz(int iters, uint32_t seed) {
    uint8_t orig[512];
    uint32_t sz = build_synthetic_ttf(orig, sizeof(orig));
    ttf_scratch_t *sc = calloc(1, sizeof(ttf_scratch_t));
    assert(sc != NULL);
    uint8_t aa[TTF_MAX_GLYPH_W * TTF_MAX_GLYPH_H / 2];

    for (int i = 0; i < iters; i++) {
        uint8_t mut[512];
        memcpy(mut, orig, sz);
        int flips = 1 + (int)(xorshift32(&seed) % 8u);
        for (int f = 0; f < flips; f++) {
            uint32_t pos = xorshift32(&seed) % sz;
            mut[pos] ^= (uint8_t)(xorshift32(&seed) & 0xFFu);
        }
        ttf_font_t font;
        if (ttf_open_mem(&font, mut, sz)) {
            ttf_glyph_metrics_t m;
            (void)ttf_rasterize_glyph(&font, sc, 24, 'A', &m, aa, sizeof(aa));
            (void)ttf_rasterize_glyph(&font, sc, 24, 'B', &m, aa, sizeof(aa));
        }
    }
    free(sc);
}

int main(int argc, char **argv) {
    int iters = (argc > 1) ? atoi(argv[1]) : 1000;
    uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0x40c0ffeeu;
    test_synthetic();
    test_noto_serif_if_present();
    test_fuzz(iters, seed);
    printf("ttf_host: PASS (%d fuzz iters)\n", iters);
    return 0;
}
