/* drivers/ttf.c — Freestanding integer/fixed-point TrueType (glyf) rasterizer.
 *
 * Parses SFNT tables (head, hhea, maxp, hmtx, cmap 4/12, loca, glyf) and
 * rasterizes quadratic Bezier outlines using 26.6 fixed-point analytic
 * scanline coverage. Zero floating-point, zero libc heap allocations.
 */

#if !defined(LUGALOS_HOST_TEST)
#include "lugalos_config.h"
#endif
#include "drivers/ttf.h"
#include <string.h>

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_PSRAM_BYTES) || defined(CONFIG_BOARD_VISIONFIVE2)

static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline int16_t bes16(const uint8_t *p) {
    return (int16_t)be16(p);
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int mem_read_fn(void *ctx, uint32_t off, void *dst, uint32_t len) {
    const uint8_t *buf = (const uint8_t *)ctx;
    memcpy(dst, buf + off, len);
    return 0;
}

static bool ttf_read(const ttf_font_t *f, uint32_t off, void *dst, uint32_t len) {
    if (!f || !f->read_fn) return false;
    if (off > f->file_size || len > f->file_size - off) return false;
    if (len == 0) return true;
    return f->read_fn(f->read_ctx, off, dst, len) == 0;
}

static bool ttf_read_u16(const ttf_font_t *f, uint32_t off, uint16_t *out) {
    uint8_t b[2];
    if (!ttf_read(f, off, b, 2)) return false;
    *out = be16(b);
    return true;
}

static bool ttf_read_u32(const ttf_font_t *f, uint32_t off, uint32_t *out) {
    uint8_t b[4];
    if (!ttf_read(f, off, b, 4)) return false;
    *out = be32(b);
    return true;
}

bool ttf_open_mem(ttf_font_t *f, const uint8_t *data, uint32_t size) {
    if (!data || size < 12) return false;
    return ttf_open_io(f, mem_read_fn, (void *)(uintptr_t)data, size);
}

bool ttf_open_io(ttf_font_t *f, ttf_read_fn read_fn, void *ctx, uint32_t size) {
    if (!f || !read_fn || size < 12) return false;
    memset(f, 0, sizeof(*f));
    f->read_fn = read_fn;
    f->read_ctx = ctx;
    f->file_size = size;

    uint8_t hdr[12];
    if (!ttf_read(f, 0, hdr, 12)) return false;
    uint32_t sfnt_ver = be32(hdr);
    /* Accept TrueType (0x00010000 or 'true'); reject CFF ('OTTO'). */
    if (sfnt_ver != 0x00010000u && sfnt_ver != 0x74727565u) return false;

    uint16_t num_tables = be16(hdr + 4);
    if (num_tables == 0 || num_tables > 64) return false;

    uint32_t head_off = 0, head_len = 0;
    uint32_t hhea_off = 0, hhea_len = 0;
    uint32_t maxp_off = 0, maxp_len = 0;
    uint32_t cmap_off = 0, cmap_len = 0;

    for (uint16_t i = 0; i < num_tables; i++) {
        uint8_t rec[16];
        if (!ttf_read(f, 12u + (uint32_t)i * 16u, rec, 16)) return false;
        uint32_t tag = be32(rec);
        uint32_t off = be32(rec + 8);
        uint32_t len = be32(rec + 12);
        if (off > size || len > size - off) continue;
        switch (tag) {
        case 0x68656164u: head_off = off; head_len = len; break; /* 'head' */
        case 0x68686561u: hhea_off = off; hhea_len = len; break; /* 'hhea' */
        case 0x6d617870u: maxp_off = off; maxp_len = len; break; /* 'maxp' */
        case 0x686d7478u: f->hmtx_off = off; f->hmtx_len = len; break; /* 'hmtx' */
        case 0x6c6f6361u: f->loca_off = off; f->loca_len = len; break; /* 'loca' */
        case 0x676c7966u: f->glyf_off = off; f->glyf_len = len; break; /* 'glyf' */
        case 0x636d6170u: cmap_off = off; cmap_len = len; break; /* 'cmap' */
        case 0x47535542u: f->gsub_off = off; f->gsub_len = len; break; /* 'GSUB' */
        case 0x47504f53u: f->gpos_off = off; f->gpos_len = len; break; /* 'GPOS' */
        case 0x6b65726eu: f->kern_off = off; f->kern_len = len; break; /* 'kern' */
        default: break;
        }
    }

    if (head_len < 54 || hhea_len < 36 || maxp_len < 6 ||
        f->hmtx_len < 4 || f->loca_len < 4 || f->glyf_len == 0 || cmap_len < 4) {
        return false;
    }

    uint8_t head_buf[54];
    if (!ttf_read(f, head_off, head_buf, sizeof(head_buf))) return false;
    f->units_per_em = be16(head_buf + 18);
    f->loca_fmt = be16(head_buf + 50);
    if (f->units_per_em == 0 || f->loca_fmt > 1) return false;

    uint8_t hhea_buf[36];
    if (!ttf_read(f, hhea_off, hhea_buf, sizeof(hhea_buf))) return false;
    f->ascent = bes16(hhea_buf + 4);
    f->descent = bes16(hhea_buf + 6);
    f->line_gap = bes16(hhea_buf + 8);
    f->num_hmetrics = be16(hhea_buf + 34);
    if (f->num_hmetrics == 0) return false;

    uint8_t maxp_buf[6];
    if (!ttf_read(f, maxp_off, maxp_buf, sizeof(maxp_buf))) return false;
    f->num_glyphs = be16(maxp_buf + 4);
    if (f->num_glyphs == 0) return false;

    /* Select best cmap subtable: prefer format 12 (full Unicode), else format 4 (BMP). */
    uint16_t cmap_tables = 0;
    if (!ttf_read_u16(f, cmap_off + 2u, &cmap_tables)) return false;
    if (cmap_tables > 64) cmap_tables = 64;

    uint32_t best_off = 0;
    uint16_t best_fmt = 0;
    int best_prio = -1;

    for (uint16_t i = 0; i < cmap_tables; i++) {
        uint8_t enc[8];
        if (!ttf_read(f, cmap_off + 4u + (uint32_t)i * 8u, enc, 8)) break;
        uint16_t pid = be16(enc);
        uint16_t eid = be16(enc + 2);
        uint32_t sub_rel = be32(enc + 4);
        if (sub_rel >= cmap_len || cmap_len - sub_rel < 4) continue;
        uint32_t sub_off = cmap_off + sub_rel;
        uint16_t fmt = 0;
        if (!ttf_read_u16(f, sub_off, &fmt)) continue;

        int prio = -1;
        if (fmt == 12 && ((pid == 3 && eid == 10) || pid == 0)) prio = 3;
        else if (fmt == 4 && ((pid == 3 && eid == 1) || pid == 0)) prio = 2;
        else if (fmt == 4) prio = 1;

        if (prio > best_prio) {
            best_prio = prio;
            best_off = sub_off;
            best_fmt = fmt;
        }
    }

    if (best_prio < 0) return false;
    f->cmap_sub_off = best_off;
    f->cmap_sub_len = cmap_len - (best_off - cmap_off);
    f->cmap_fmt = best_fmt;
    return true;
}

static uint16_t cmap_lookup_fmt4(const ttf_font_t *f, uint32_t cp) {
    if (cp > 0xFFFFu || f->cmap_sub_len < 16) return 0;
    uint16_t seg_count_x2 = 0;
    if (!ttf_read_u16(f, f->cmap_sub_off + 6u, &seg_count_x2)) return 0;
    uint16_t seg_count = seg_count_x2 / 2u;
    if (seg_count == 0) return 0;

    uint32_t end_off = f->cmap_sub_off + 14u;
    uint32_t start_off = end_off + (uint32_t)seg_count * 2u + 2u;
    uint32_t delta_off = start_off + (uint32_t)seg_count * 2u;
    uint32_t range_off = delta_off + (uint32_t)seg_count * 2u;
    if (range_off + (uint32_t)seg_count * 2u > f->cmap_sub_off + f->cmap_sub_len) return 0;

    int lo = 0, hi = (int)seg_count - 1;
    int found = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint16_t ec = 0;
        if (!ttf_read_u16(f, end_off + (uint32_t)mid * 2u, &ec)) return 0;
        if (ec >= cp) {
            found = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    if (found < 0) return 0;

    uint16_t sc = 0, id_delta = 0, id_range = 0;
    if (!ttf_read_u16(f, start_off + (uint32_t)found * 2u, &sc)) return 0;
    if (cp < sc) return 0;
    if (!ttf_read_u16(f, delta_off + (uint32_t)found * 2u, &id_delta)) return 0;
    if (!ttf_read_u16(f, range_off + (uint32_t)found * 2u, &id_range)) return 0;

    if (id_range == 0) {
        return (uint16_t)((cp + id_delta) & 0xFFFFu);
    }
    uint32_t gid_off = range_off + (uint32_t)found * 2u + id_range + (cp - sc) * 2u;
    uint16_t gid = 0;
    if (!ttf_read_u16(f, gid_off, &gid) || gid == 0) return 0;
    return (uint16_t)((gid + id_delta) & 0xFFFFu);
}

static uint16_t cmap_lookup_fmt12(const ttf_font_t *f, uint32_t cp) {
    if (f->cmap_sub_len < 16) return 0;
    uint32_t num_groups = 0;
    if (!ttf_read_u32(f, f->cmap_sub_off + 12u, &num_groups)) return 0;
    if (num_groups == 0 || 16u + (uint64_t)num_groups * 12u > f->cmap_sub_len) return 0;

    int lo = 0, hi = (int)num_groups - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint8_t grp[12];
        if (!ttf_read(f, f->cmap_sub_off + 16u + (uint32_t)mid * 12u, grp, 12)) return 0;
        uint32_t sc = be32(grp);
        uint32_t ec = be32(grp + 4);
        uint32_t sg = be32(grp + 8);
        if (cp < sc) {
            hi = mid - 1;
        } else if (cp > ec) {
            lo = mid + 1;
        } else {
            return (uint16_t)(sg + (cp - sc));
        }
    }
    return 0;
}

uint16_t ttf_glyph_index(const ttf_font_t *f, uint32_t codepoint) {
    if (!f) return 0;
    uint16_t gid = (f->cmap_fmt == 12) ? cmap_lookup_fmt12(f, codepoint)
                                       : cmap_lookup_fmt4(f, codepoint);
    return (gid < f->num_glyphs) ? gid : 0;
}

static void ttf_hmetrics_raw(const ttf_font_t *f, uint16_t gid, int32_t *out_adv, int32_t *out_lsb) {
    uint16_t adv = 0;
    int16_t lsb = 0;
    if (gid < f->num_hmetrics) {
        uint8_t b[4];
        if (ttf_read(f, f->hmtx_off + (uint32_t)gid * 4u, b, 4)) {
            adv = be16(b);
            lsb = bes16(b + 2);
        }
    } else {
        uint8_t b[2];
        if (ttf_read(f, f->hmtx_off + (uint32_t)(f->num_hmetrics - 1u) * 4u, b, 2)) {
            adv = be16(b);
        }
        uint32_t lsb_off = f->hmtx_off + (uint32_t)f->num_hmetrics * 4u +
                           (uint32_t)(gid - f->num_hmetrics) * 2u;
        if (ttf_read(f, lsb_off, b, 2)) {
            lsb = bes16(b);
        }
    }
    *out_adv = (int32_t)adv;
    *out_lsb = (int32_t)lsb;
}

static int32_t ttf_em_span(const ttf_font_t *f) {
    int32_t span = (int32_t)f->ascent - (int32_t)f->descent;
    return (span > 0) ? span : (int32_t)f->units_per_em;
}

int ttf_baseline_px(const ttf_font_t *f, int cell_h) {
    if (!f || cell_h <= 0) return 0;
    int32_t span = ttf_em_span(f);
    return (int)(((int64_t)f->ascent * cell_h + span / 2) / span);
}

int ttf_gid_advance_px(const ttf_font_t *f, int cell_h, uint16_t gid) {
    if (!f || cell_h <= 0) return 0;
    int32_t adv = 0, lsb = 0;
    ttf_hmetrics_raw(f, gid, &adv, &lsb);
    int32_t span = ttf_em_span(f);
    int px = (int)(((int64_t)adv * cell_h + span / 2) / span);
    if (px == 0 && adv > 0) px = 1;
    return px;
}

int ttf_advance_px(const ttf_font_t *f, int cell_h, uint32_t codepoint) {
    if (!f || cell_h <= 0) return 0;
    return ttf_gid_advance_px(f, cell_h, ttf_glyph_index(f, codepoint));
}

static bool glyph_loca_range(const ttf_font_t *f, uint16_t gid, uint32_t *out_off, uint32_t *out_len) {
    if (gid >= f->num_glyphs) return false;
    uint32_t o0 = 0, o1 = 0;
    if (f->loca_fmt == 0) {
        uint16_t a = 0, b = 0;
        if (!ttf_read_u16(f, f->loca_off + (uint32_t)gid * 2u, &a)) return false;
        if (!ttf_read_u16(f, f->loca_off + (uint32_t)(gid + 1u) * 2u, &b)) return false;
        o0 = (uint32_t)a * 2u;
        o1 = (uint32_t)b * 2u;
    } else {
        if (!ttf_read_u32(f, f->loca_off + (uint32_t)gid * 4u, &o0)) return false;
        if (!ttf_read_u32(f, f->loca_off + (uint32_t)(gid + 1u) * 4u, &o1)) return false;
    }
    if (o1 < o0 || o1 > f->glyf_len) return false;
    *out_off = f->glyf_off + o0;
    *out_len = o1 - o0;
    return true;
}

/* Decode simple or compound glyph into sc->px/py/on/end_pts in font units. */
static bool decode_glyph_outline(const ttf_font_t *f, ttf_scratch_t *sc, uint16_t gid,
                                 int depth, int *io_npts, int *io_ncontours,
                                 int32_t m00, int32_t m01, int32_t m10, int32_t m11,
                                 int32_t dx, int32_t dy) {
    if (depth > 8) return false;
    uint32_t goff = 0, glen = 0;
    if (!glyph_loca_range(f, gid, &goff, &glen)) return false;
    if (glen == 0) return true; /* empty glyph (e.g. space) */
    if (glen < 10) return false;

    uint8_t ghdr[10];
    if (!ttf_read(f, goff, ghdr, 10)) return false;
    int16_t ncontours = bes16(ghdr);

    if (ncontours >= 0) {
        if (ncontours == 0) return true;
        if (*io_ncontours + ncontours > TTF_MAX_CONTOURS) return false;

        uint32_t pos = goff + 10u;
        uint32_t end_off = goff + glen;
        int base_pt = *io_npts;

        uint16_t prev_end = 0;
        for (int i = 0; i < ncontours; i++) {
            uint16_t ep = 0;
            if (pos + 2u > end_off || !ttf_read_u16(f, pos, &ep)) return false;
            pos += 2u;
            if (i > 0 && ep < prev_end) return false;
            prev_end = ep;
            sc->end_pts[*io_ncontours + i] = (uint16_t)(base_pt + ep);
        }

        int num_pts = (int)prev_end + 1;
        if (base_pt + num_pts > TTF_MAX_POINTS) return false;

        uint16_t ins_len = 0;
        if (pos + 2u > end_off || !ttf_read_u16(f, pos, &ins_len)) return false;
        pos += 2u + (uint32_t)ins_len;
        if (pos > end_off) return false;

        /* Flags */
        int p = 0;
        while (p < num_pts) {
            uint8_t fl = 0;
            if (pos >= end_off || !ttf_read(f, pos, &fl, 1)) return false;
            pos++;
            sc->on[base_pt + p] = fl;
            p++;
            if (fl & 0x08u) {
                uint8_t rep = 0;
                if (pos >= end_off || !ttf_read(f, pos, &rep, 1)) return false;
                pos++;
                if (p + rep > num_pts) return false;
                for (uint8_t r = 0; r < rep; r++) sc->on[base_pt + p++] = fl;
            }
        }

        /* X coordinates (relative) */
        int32_t x = 0;
        for (int i = 0; i < num_pts; i++) {
            uint8_t fl = sc->on[base_pt + i];
            int32_t d = 0;
            if (fl & 0x02u) {
                uint8_t v = 0;
                if (pos >= end_off || !ttf_read(f, pos, &v, 1)) return false;
                pos++;
                d = (fl & 0x10u) ? (int32_t)v : -(int32_t)v;
            } else if (!(fl & 0x10u)) {
                uint8_t b[2];
                if (pos + 2u > end_off || !ttf_read(f, pos, b, 2)) return false;
                pos += 2u;
                d = (int32_t)bes16(b);
            }
            x += d;
            sc->px[base_pt + i] = x;
        }

        /* Y coordinates (relative) */
        int32_t y = 0;
        for (int i = 0; i < num_pts; i++) {
            uint8_t fl = sc->on[base_pt + i];
            int32_t d = 0;
            if (fl & 0x04u) {
                uint8_t v = 0;
                if (pos >= end_off || !ttf_read(f, pos, &v, 1)) return false;
                pos++;
                d = (fl & 0x20u) ? (int32_t)v : -(int32_t)v;
            } else if (!(fl & 0x20u)) {
                uint8_t b[2];
                if (pos + 2u > end_off || !ttf_read(f, pos, b, 2)) return false;
                pos += 2u;
                d = (int32_t)bes16(b);
            }
            y += d;
            sc->py[base_pt + i] = y;
        }

        /* Apply component transform (m00..m11 in 2.14 fixed-point) and normalize on-curve bit. */
        for (int i = 0; i < num_pts; i++) {
            int32_t ox = sc->px[base_pt + i];
            int32_t oy = sc->py[base_pt + i];
            sc->px[base_pt + i] = (int32_t)(((int64_t)ox * m00 + (int64_t)oy * m01) >> 14) + dx;
            sc->py[base_pt + i] = (int32_t)(((int64_t)ox * m10 + (int64_t)oy * m11) >> 14) + dy;
            sc->on[base_pt + i] = (uint8_t)(sc->on[base_pt + i] & 1u);
        }

        *io_npts += num_pts;
        *io_ncontours += ncontours;
        return true;
    }

    /* Compound glyph */
    uint32_t pos = goff + 10u;
    uint32_t end_off = goff + glen;
    for (;;) {
        uint8_t chdr[4];
        if (pos + 4u > end_off || !ttf_read(f, pos, chdr, 4)) return false;
        pos += 4u;
        uint16_t flags = be16(chdr);
        uint16_t sub_gid = be16(chdr + 2);

        int32_t arg1 = 0, arg2 = 0;
        if (flags & 0x0001u) { /* ARG_1_AND_2_ARE_WORDS */
            uint8_t b[4];
            if (pos + 4u > end_off || !ttf_read(f, pos, b, 4)) return false;
            pos += 4u;
            arg1 = bes16(b);
            arg2 = bes16(b + 2);
        } else {
            uint8_t b[2];
            if (pos + 2u > end_off || !ttf_read(f, pos, b, 2)) return false;
            pos += 2u;
            arg1 = (int8_t)b[0];
            arg2 = (int8_t)b[1];
        }

        int32_t sm00 = 16384, sm01 = 0, sm10 = 0, sm11 = 16384;
        if (flags & 0x0008u) { /* WE_HAVE_A_SCALE */
            uint8_t b[2];
            if (pos + 2u > end_off || !ttf_read(f, pos, b, 2)) return false;
            pos += 2u;
            sm00 = sm11 = bes16(b);
        } else if (flags & 0x0040u) { /* WE_HAVE_AN_X_AND_Y_SCALE */
            uint8_t b[4];
            if (pos + 4u > end_off || !ttf_read(f, pos, b, 4)) return false;
            pos += 4u;
            sm00 = bes16(b);
            sm11 = bes16(b + 2);
        } else if (flags & 0x0080u) { /* WE_HAVE_A_TWO_BY_TWO */
            uint8_t b[8];
            if (pos + 8u > end_off || !ttf_read(f, pos, b, 8)) return false;
            pos += 8u;
            sm00 = bes16(b);
            sm10 = bes16(b + 2);
            sm01 = bes16(b + 4);
            sm11 = bes16(b + 6);
        }

        int32_t sdx = (flags & 0x0002u) ? arg1 : 0; /* ARGS_ARE_XY_VALUES */
        int32_t sdy = (flags & 0x0002u) ? arg2 : 0;

        int32_t cm00 = (int32_t)(((int64_t)m00 * sm00 + (int64_t)m01 * sm10) >> 14);
        int32_t cm01 = (int32_t)(((int64_t)m00 * sm01 + (int64_t)m01 * sm11) >> 14);
        int32_t cm10 = (int32_t)(((int64_t)m10 * sm00 + (int64_t)m11 * sm10) >> 14);
        int32_t cm11 = (int32_t)(((int64_t)m10 * sm01 + (int64_t)m11 * sm11) >> 14);
        int32_t cdx = (int32_t)(((int64_t)m00 * sdx + (int64_t)m01 * sdy) >> 14) + dx;
        int32_t cdy = (int32_t)(((int64_t)m10 * sdx + (int64_t)m11 * sdy) >> 14) + dy;

        if (!decode_glyph_outline(f, sc, sub_gid, depth + 1, io_npts, io_ncontours,
                                  cm00, cm01, cm10, cm11, cdx, cdy)) {
            return false;
        }
        if (!(flags & 0x0020u)) break; /* MORE_COMPONENTS */
    }
    return true;
}

static void push_edge(ttf_scratch_t *sc, int *nedges, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (y0 == y1 || *nedges >= TTF_MAX_EDGES) return;
    ttf_edge_t *e = &sc->edges[*nedges];
    e->x0 = x0;
    e->y0 = y0;
    e->x1 = x1;
    e->y1 = y1;
    (*nedges)++;
}

static void flatten_qbezier(ttf_scratch_t *sc, int *nedges,
                            int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1,
                            int32_t x2, int32_t y2, int depth) {
    int32_t dx = x0 - 2 * x1 + x2;
    int32_t dy = y0 - 2 * y1 + y2;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (depth >= 6 || dx + dy <= 16) {
        push_edge(sc, nedges, x0, y0, x2, y2);
        return;
    }
    int32_t x01 = (x0 + x1) / 2;
    int32_t y01 = (y0 + y1) / 2;
    int32_t x12 = (x1 + x2) / 2;
    int32_t y12 = (y1 + y2) / 2;
    int32_t xm  = (x01 + x12) / 2;
    int32_t ym  = (y01 + y12) / 2;
    flatten_qbezier(sc, nedges, x0, y0, x01, y01, xm, ym, depth + 1);
    flatten_qbezier(sc, nedges, xm, ym, x12, y12, x2, y2, depth + 1);
}

static inline int32_t floor_div64(int32_t v) {
    return (v >= 0) ? (v >> 6) : -(((-v) + 63) >> 6);
}

static inline int32_t ceil_div64(int32_t v) {
    return (v >= 0) ? ((v + 63) >> 6) : -((-v) >> 6);
}

/* Accumulate a single scanline segment (sx0, sy0) -> (sx1, sy1) within
 * scanline row `py` (sy0, sy1 in [0..64]). */
static void raster_scanline_seg(ttf_scratch_t *sc, int py, int gw,
                                int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1) {
    int32_t dy = sy1 - sy0;
    if (dy == 0) return;

    int row_off = py * gw;
    if (sx0 == sx1) {
        int32_t ix = floor_div64(sx0);
        if (ix < 0) {
            sc->cover[row_off] += dy;
        } else if (ix < gw) {
            int32_t fx = sx0 - (ix << 6);
            sc->cover[row_off + ix] += dy;
            sc->area[row_off + ix] += dy * (fx * 2);
        }
        return;
    }

    int32_t cur_x = sx0;
    int32_t cur_y = sy0;
    int32_t dx_tot = sx1 - sx0;
    int32_t dy_tot = sy1 - sy0;
    int step = (sx1 > sx0) ? 1 : -1;

    while (cur_x != sx1) {
        int32_t ix = floor_div64(cur_x);
        int32_t next_bx;
        if (step > 0) {
            next_bx = (ix + 1) << 6;
            if (next_bx > sx1) next_bx = sx1;
        } else {
            next_bx = (cur_x == (ix << 6)) ? ((ix - 1) << 6) : (ix << 6);
            if (next_bx < sx1) next_bx = sx1;
            if (cur_x == (ix << 6)) ix--;
        }

        int32_t next_y = (next_bx == sx1)
                         ? sy1
                         : (sy0 + (int32_t)(((int64_t)(next_bx - sx0) * dy_tot) / dx_tot));
        int32_t seg_dy = next_y - cur_y;

        if (ix < 0) {
            sc->cover[row_off] += seg_dy;
        } else if (ix < gw) {
            int32_t fx0 = cur_x - (ix << 6);
            int32_t fx1 = next_bx - (ix << 6);
            sc->cover[row_off + ix] += seg_dy;
            sc->area[row_off + ix] += seg_dy * (fx0 + fx1);
        }

        cur_x = next_bx;
        cur_y = next_y;
    }
}

bool ttf_rasterize_glyph_id(const ttf_font_t *f, ttf_scratch_t *sc, int cell_h,
                            uint16_t gid, ttf_glyph_metrics_t *out_m,
                            uint8_t *out_aa, uint32_t out_cap) {
    if (!f || !sc || !out_m || cell_h <= 0) return false;
    memset(out_m, 0, sizeof(*out_m));

    int32_t adv_fu = 0, lsb_fu = 0;
    ttf_hmetrics_raw(f, gid, &adv_fu, &lsb_fu);

    int32_t span = ttf_em_span(f);
    int adv_px = (int)(((int64_t)adv_fu * cell_h + span / 2) / span);
    if (adv_px == 0 && adv_fu > 0) adv_px = 1;
    if (adv_px > 255) adv_px = 255;
    out_m->advance = (uint8_t)adv_px;

    int npts = 0, ncontours = 0;
    if (!decode_glyph_outline(f, sc, gid, 0, &npts, &ncontours,
                              16384, 0, 0, 16384, 0, 0)) {
        return false;
    }
    if (npts == 0 || ncontours == 0) {
        return true; /* empty glyph: zero bitmap, valid advance */
    }

    /* Scale points to 26.6 fixed-point in cell space (+y down, top = ascent). */
    int32_t min_x = INT32_MAX, max_x = INT32_MIN;
    int32_t min_y = INT32_MAX, max_y = INT32_MIN;
    for (int i = 0; i < npts; i++) {
        int32_t sx = (int32_t)(((int64_t)sc->px[i] * cell_h * 64) / span);
        int32_t sy = (int32_t)(((int64_t)((int32_t)f->ascent - sc->py[i]) * cell_h * 64) / span);
        sc->px[i] = sx;
        sc->py[i] = sy;
        if (sx < min_x) min_x = sx;
        if (sx > max_x) max_x = sx;
        if (sy < min_y) min_y = sy;
        if (sy > max_y) max_y = sy;
    }

    int32_t bx0 = floor_div64(min_x) - 1;
    int32_t bx1 = ceil_div64(max_x) + 1;
    int32_t by0 = floor_div64(min_y) - 1;
    int32_t by1 = ceil_div64(max_y) + 1;

    int gw = (int)(bx1 - bx0);
    int gh = (int)(by1 - by0);
    if (gw <= 0 || gh <= 0) return true;
    if (gw > TTF_MAX_GLYPH_W) gw = TTF_MAX_GLYPH_W;
    if (gh > TTF_MAX_GLYPH_H) gh = TTF_MAX_GLYPH_H;

    uint8_t stride = (uint8_t)((gw + 1) / 2);
    if (!out_aa || (uint32_t)gh * stride > out_cap) return false;

    for (int i = 0; i < npts; i++) {
        sc->px[i] -= bx0 * 64;
        sc->py[i] -= by0 * 64;
    }

    /* Flatten contours into line segments. */
    int nedges = 0;
    int start = 0;
    for (int c = 0; c < ncontours; c++) {
        int end = sc->end_pts[c];
        int cnt = end - start + 1;
        if (cnt < 2) {
            start = end + 1;
            continue;
        }

        /* Find an on-curve starting point, or synthesize midpoint if all off-curve. */
        int first_on = -1;
        for (int i = 0; i < cnt; i++) {
            if (sc->on[start + i]) { first_on = i; break; }
        }

        int32_t sx, sy;
        int idx;
        if (first_on >= 0) {
            idx = first_on;
            sx = sc->px[start + idx];
            sy = sc->py[start + idx];
        } else {
            idx = 0;
            int next = (cnt > 1) ? 1 : 0;
            sx = (sc->px[start] + sc->px[start + next]) / 2;
            sy = (sc->py[start] + sc->py[start + next]) / 2;
        }

        int32_t cur_x = sx, cur_y = sy;
        int steps = cnt;
        int i = (first_on >= 0) ? ((idx + 1) % cnt) : 0;
        while (steps > 0) {
            int p_idx = start + i;
            if (sc->on[p_idx]) {
                push_edge(sc, &nedges, cur_x, cur_y, sc->px[p_idx], sc->py[p_idx]);
                cur_x = sc->px[p_idx];
                cur_y = sc->py[p_idx];
                i = (i + 1) % cnt;
                steps--;
            } else {
                int32_t cx = sc->px[p_idx];
                int32_t cy = sc->py[p_idx];
                int next_i = (i + 1) % cnt;
                int n_idx = start + next_i;
                if (sc->on[n_idx]) {
                    flatten_qbezier(sc, &nedges, cur_x, cur_y, cx, cy,
                                    sc->px[n_idx], sc->py[n_idx], 0);
                    cur_x = sc->px[n_idx];
                    cur_y = sc->py[n_idx];
                    i = (next_i + 1) % cnt;
                    steps -= 2;
                } else {
                    int32_t mx = (cx + sc->px[n_idx]) / 2;
                    int32_t my = (cy + sc->py[n_idx]) / 2;
                    flatten_qbezier(sc, &nedges, cur_x, cur_y, cx, cy, mx, my, 0);
                    cur_x = mx;
                    cur_y = my;
                    i = next_i;
                    steps--;
                }
            }
        }
        if (cur_x != sx || cur_y != sy) {
            push_edge(sc, &nedges, cur_x, cur_y, sx, sy);
        }
        start = end + 1;
    }

    /* Rasterize edges into cover/area cells. */
    memset(sc->cover, 0, (size_t)gw * (size_t)gh * sizeof(int32_t));
    memset(sc->area, 0, (size_t)gw * (size_t)gh * sizeof(int32_t));

    for (int e = 0; e < nedges; e++) {
        int32_t x0 = sc->edges[e].x0;
        int32_t y0 = sc->edges[e].y0;
        int32_t x1 = sc->edges[e].x1;
        int32_t y1 = sc->edges[e].y1;
        int32_t dx_tot = x1 - x0;
        int32_t dy_tot = y1 - y0;
        if (dy_tot == 0) continue;

        int32_t py0 = floor_div64(y0);
        int32_t py1 = floor_div64(y1);
        int ystep = (y1 > y0) ? 1 : -1;
        int32_t cy = y0;
        int32_t cx = x0;

        while (cy != y1) {
            int32_t py = floor_div64(cy);
            int32_t next_by;
            if (ystep > 0) {
                next_by = (py + 1) << 6;
                if (next_by > y1) next_by = y1;
            } else {
                next_by = (cy == (py << 6)) ? ((py - 1) << 6) : (py << 6);
                if (next_by < y1) next_by = y1;
                if (cy == (py << 6)) py--;
            }
            (void)py0;
            (void)py1;

            int32_t next_x = (next_by == y1)
                             ? x1
                             : (x0 + (int32_t)(((int64_t)(next_by - y0) * dx_tot) / dy_tot));

            if (py >= 0 && py < gh) {
                raster_scanline_seg(sc, (int)py, gw,
                                    cx, cy - (py << 6),
                                    next_x, next_by - (py << 6));
            }
            cx = next_x;
            cy = next_by;
        }
    }

    /* Convert signed area/cover into packed 4-bit coverage (0..15). */
    memset(out_aa, 0, (size_t)gh * stride);
    for (int y = 0; y < gh; y++) {
        int row_off = y * gw;
        uint8_t *dst = out_aa + (size_t)y * stride;
        int32_t sum = 0;
        for (int x = 0; x < gw; x++) {
            int32_t val = sum + sc->cover[row_off + x] * 128 - sc->area[row_off + x];
            sum += sc->cover[row_off + x] * 128;
            int32_t cov = (val < 0) ? -val : val;
            uint32_t a = (uint32_t)((cov * 15 + 4096) / 8192);
            if (a > 15u) a = 15u;
            if (x & 1) dst[x >> 1] |= (uint8_t)(a << 4);
            else       dst[x >> 1] = (uint8_t)a;
        }
    }

    out_m->xoff = (int16_t)bx0;
    out_m->yoff = (int16_t)by0;
    out_m->w = (uint8_t)gw;
    out_m->h = (uint8_t)gh;
    out_m->stride = stride;
    return true;
}

bool ttf_rasterize_glyph(const ttf_font_t *f, ttf_scratch_t *sc, int cell_h,
                         uint32_t codepoint, ttf_glyph_metrics_t *out_m,
                         uint8_t *out_aa, uint32_t out_cap) {
    if (!f) return false;
    return ttf_rasterize_glyph_id(f, sc, cell_h, ttf_glyph_index(f, codepoint),
                                  out_m, out_aa, out_cap);
}

#endif /* LUGALOS_HOST_TEST || CONFIG_PSRAM_BYTES || CONFIG_BOARD_VISIONFIVE2 */
