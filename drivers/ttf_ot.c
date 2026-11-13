#include "lugalos_config.h"
#include "drivers/ttf_ot.h"
#include <string.h>

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_PSRAM_BYTES) || defined(CONFIG_BOARD_VISIONFIVE2)

#define OT_TAG(a, b, c, d) \
    (((uint32_t)(uint8_t)(a) << 24) | ((uint32_t)(uint8_t)(b) << 16) | \
     ((uint32_t)(uint8_t)(c) << 8)  | (uint32_t)(uint8_t)(d))

#define OT_MAX_LOOKUPS 512

static inline uint16_t ot_u16(const uint8_t *b, uint32_t len, uint32_t off) {
    if (off + 2u > len) return 0;
    return (uint16_t)(((uint16_t)b[off] << 8) | b[off + 1u]);
}

static inline int16_t ot_s16(const uint8_t *b, uint32_t len, uint32_t off) {
    return (int16_t)ot_u16(b, len, off);
}

static inline uint32_t ot_u32(const uint8_t *b, uint32_t len, uint32_t off) {
    if (off + 4u > len) return 0;
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1u] << 16) |
           ((uint32_t)b[off + 2u] << 8) | (uint32_t)b[off + 3u];
}

uint32_t ttf_ot_required_bytes(const ttf_font_t *f) {
    if (!f) return 0;
    uint32_t total = 0;
    if (f->gsub_len >= 10u) total += (f->gsub_len + 3u) & ~3u;
    if (f->gpos_len >= 10u) total += (f->gpos_len + 3u) & ~3u;
    if (f->kern_len >= 4u)  total += (f->kern_len + 3u) & ~3u;
    return total;
}

bool ttf_ot_init(ttf_ot_t *ot, const ttf_font_t *f, uint32_t script_tag,
                 uint8_t *buf, uint32_t buf_cap) {
    if (!ot || !f) return false;
    memset(ot, 0, sizeof(*ot));
    ot->script_tag = script_tag ? script_tag : OT_TAG('D', 'F', 'L', 'T');
    if (!buf || buf_cap == 0) return true;

    uint32_t pos = 0;
    if (f->gsub_len >= 10u && pos + f->gsub_len <= buf_cap) {
        if (f->read_fn(f->read_ctx, f->gsub_off, buf + pos, f->gsub_len) == 0) {
            ot->gsub = buf + pos;
            ot->gsub_len = f->gsub_len;
            pos = (pos + f->gsub_len + 3u) & ~3u;
        }
    }
    if (f->gpos_len >= 10u && pos + f->gpos_len <= buf_cap) {
        if (f->read_fn(f->read_ctx, f->gpos_off, buf + pos, f->gpos_len) == 0) {
            ot->gpos = buf + pos;
            ot->gpos_len = f->gpos_len;
            pos = (pos + f->gpos_len + 3u) & ~3u;
        }
    }
    if (f->kern_len >= 4u && pos + f->kern_len <= buf_cap) {
        if (f->read_fn(f->read_ctx, f->kern_off, buf + pos, f->kern_len) == 0) {
            ot->kern = buf + pos;
            ot->kern_len = f->kern_len;
        }
    }
    return true;
}

static int ot_coverage(const uint8_t *t, uint32_t len, uint32_t off, uint16_t gid) {
    if (off + 4u > len) return -1;
    uint16_t fmt = ot_u16(t, len, off);
    uint16_t cnt = ot_u16(t, len, off + 2u);
    if (fmt == 1u) {
        if (off + 4u + (uint32_t)cnt * 2u > len) return -1;
        int lo = 0, hi = (int)cnt - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint16_t g = ot_u16(t, len, off + 4u + (uint32_t)mid * 2u);
            if (g == gid) return mid;
            if (g < gid) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }
    if (fmt == 2u) {
        if (off + 4u + (uint32_t)cnt * 6u > len) return -1;
        int lo = 0, hi = (int)cnt - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t rec = off + 4u + (uint32_t)mid * 6u;
            uint16_t s = ot_u16(t, len, rec);
            uint16_t e = ot_u16(t, len, rec + 2u);
            if (gid < s) hi = mid - 1;
            else if (gid > e) lo = mid + 1;
            else return (int)ot_u16(t, len, rec + 4u) + (int)(gid - s);
        }
        return -1;
    }
    return -1;
}

static uint16_t ot_classdef(const uint8_t *t, uint32_t len, uint32_t off, uint16_t gid) {
    if (off + 4u > len) return 0;
    uint16_t fmt = ot_u16(t, len, off);
    if (fmt == 1u) {
        if (off + 6u > len) return 0;
        uint16_t start = ot_u16(t, len, off + 2u);
        uint16_t cnt = ot_u16(t, len, off + 4u);
        if (gid < start || gid >= start + cnt) return 0;
        return ot_u16(t, len, off + 6u + (uint32_t)(gid - start) * 2u);
    }
    if (fmt == 2u) {
        uint16_t cnt = ot_u16(t, len, off + 2u);
        if (off + 4u + (uint32_t)cnt * 6u > len) return 0;
        int lo = 0, hi = (int)cnt - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t rec = off + 4u + (uint32_t)mid * 6u;
            uint16_t s = ot_u16(t, len, rec);
            uint16_t e = ot_u16(t, len, rec + 2u);
            if (gid < s) hi = mid - 1;
            else if (gid > e) lo = mid + 1;
            else return ot_u16(t, len, rec + 4u);
        }
        return 0;
    }
    return 0;
}

static bool ot_tag_in_list(uint32_t tag, const uint32_t *wanted, int n_wanted) {
    for (int i = 0; i < n_wanted; i++) {
        if (wanted[i] == tag) return true;
    }
    return false;
}

/* Collect active lookup indices for `wanted_feats` under `script_tag` (or DFLT). */
static uint16_t ot_collect_lookups(const uint8_t *t, uint32_t len,
                                   uint32_t script_tag,
                                   const uint32_t *wanted_feats, int n_wanted,
                                   uint8_t *active_bits, uint16_t *out_lcount) {
    memset(active_bits, 0, OT_MAX_LOOKUPS / 8);
    *out_lcount = 0;
    if (!t || len < 10u) return 0;

    uint16_t sl_off = ot_u16(t, len, 4u);
    uint16_t fl_off = ot_u16(t, len, 6u);
    uint16_t ll_off = ot_u16(t, len, 8u);
    if (sl_off + 2u > len || fl_off + 2u > len || ll_off + 2u > len) return 0;

    uint16_t lcount = ot_u16(t, len, ll_off);
    if (lcount > OT_MAX_LOOKUPS) lcount = OT_MAX_LOOKUPS;
    *out_lcount = lcount;

    uint16_t scnt = ot_u16(t, len, sl_off);
    if (sl_off + 2u + (uint32_t)scnt * 6u > len || scnt == 0) return 0;

    uint32_t script_rel = 0;
    uint32_t dflt_rel = 0;
    uint32_t first_rel = ot_u16(t, len, sl_off + 6u);
    for (uint16_t i = 0; i < scnt; i++) {
        uint32_t rec = sl_off + 2u + (uint32_t)i * 6u;
        uint32_t tag = ot_u32(t, len, rec);
        uint16_t srel = ot_u16(t, len, rec + 4u);
        if (tag == script_tag) script_rel = srel;
        if (tag == OT_TAG('D', 'F', 'L', 'T')) dflt_rel = srel;
    }
    uint32_t chosen_script = sl_off + (script_rel ? script_rel : (dflt_rel ? dflt_rel : first_rel));
    if (chosen_script + 4u > len) return 0;

    uint16_t lang_rel = ot_u16(t, len, chosen_script);
    if (lang_rel == 0) {
        uint16_t lsys_cnt = ot_u16(t, len, chosen_script + 2u);
        if (lsys_cnt == 0 || chosen_script + 10u > len) return 0;
        lang_rel = ot_u16(t, len, chosen_script + 8u);
    }
    uint32_t lsys = chosen_script + lang_rel;
    if (lsys + 6u > len) return 0;

    uint16_t req_feat = ot_u16(t, len, lsys + 2u);
    uint16_t fidx_cnt = ot_u16(t, len, lsys + 4u);
    uint16_t fl_cnt = ot_u16(t, len, fl_off);

    uint16_t n_active = 0;
    for (uint16_t fi = 0; fi <= fidx_cnt; fi++) {
        uint16_t feat_idx;
        if (fi == 0) {
            if (req_feat == 0xFFFFu) continue;
            feat_idx = req_feat;
        } else {
            if (lsys + 6u + (uint32_t)(fi - 1u) * 2u + 2u > len) break;
            feat_idx = ot_u16(t, len, lsys + 6u + (uint32_t)(fi - 1u) * 2u);
        }
        if (feat_idx >= fl_cnt) continue;
        uint32_t frec = fl_off + 2u + (uint32_t)feat_idx * 6u;
        if (frec + 6u > len) continue;
        uint32_t ftag = ot_u32(t, len, frec);
        if (fi != 0 && !ot_tag_in_list(ftag, wanted_feats, n_wanted)) continue;

        uint32_t ftab = fl_off + ot_u16(t, len, frec + 4u);
        if (ftab + 4u > len) continue;
        uint16_t lref_cnt = ot_u16(t, len, ftab + 2u);
        for (uint16_t k = 0; k < lref_cnt; k++) {
            if (ftab + 4u + (uint32_t)k * 2u + 2u > len) break;
            uint16_t lidx = ot_u16(t, len, ftab + 4u + (uint32_t)k * 2u);
            if (lidx < lcount) {
                if (!(active_bits[lidx >> 3] & (1u << (lidx & 7u)))) {
                    active_bits[lidx >> 3] |= (uint8_t)(1u << (lidx & 7u));
                    n_active++;
                }
            }
        }
    }
    return n_active;
}

static int gsub_apply_lookup_at(const uint8_t *t, uint32_t len, uint32_t ll_off,
                                uint16_t lidx, uint16_t *gids, int *plen,
                                int max_out, int pos, int depth);

static bool gsub_match_seq(const uint8_t *t, uint32_t len,
                           const uint16_t *gids, int glen, int pos,
                           uint16_t back_cnt, uint32_t back_off,
                           uint16_t inp_cnt, uint32_t inp_off,
                           uint16_t look_cnt, uint32_t look_off,
                           bool is_class, uint32_t back_cd, uint32_t inp_cd, uint32_t look_cd,
                           bool is_cov) {
    if (pos < (int)back_cnt) return false;
    if (pos + (int)inp_cnt + (int)look_cnt > glen) return false;

    for (uint16_t i = 0; i < back_cnt; i++) {
        uint16_t g = gids[pos - 1 - (int)i];
        if (is_cov) {
            uint32_t cov_off = back_cd + ot_u16(t, len, back_off + (uint32_t)i * 2u);
            if (ot_coverage(t, len, cov_off, g) < 0) return false;
        } else if (is_class) {
            if (ot_classdef(t, len, back_cd, g) != ot_u16(t, len, back_off + (uint32_t)i * 2u)) return false;
        } else {
            if (g != ot_u16(t, len, back_off + (uint32_t)i * 2u)) return false;
        }
    }

    uint16_t start_inp = is_cov ? 0u : 1u;
    for (uint16_t i = start_inp; i < inp_cnt; i++) {
        uint16_t g = gids[pos + (int)i];
        uint32_t idx = (uint32_t)(i - start_inp) * 2u;
        if (is_cov) {
            uint32_t cov_off = back_cd + ot_u16(t, len, inp_off + idx);
            if (ot_coverage(t, len, cov_off, g) < 0) return false;
        } else if (is_class) {
            if (ot_classdef(t, len, inp_cd, g) != ot_u16(t, len, inp_off + idx)) return false;
        } else {
            if (g != ot_u16(t, len, inp_off + idx)) return false;
        }
    }

    for (uint16_t i = 0; i < look_cnt; i++) {
        uint16_t g = gids[pos + (int)inp_cnt + (int)i];
        if (is_cov) {
            uint32_t cov_off = back_cd + ot_u16(t, len, look_off + (uint32_t)i * 2u);
            if (ot_coverage(t, len, cov_off, g) < 0) return false;
        } else if (is_class) {
            if (ot_classdef(t, len, look_cd, g) != ot_u16(t, len, look_off + (uint32_t)i * 2u)) return false;
        } else {
            if (g != ot_u16(t, len, look_off + (uint32_t)i * 2u)) return false;
        }
    }
    return true;
}

static int gsub_apply_subst_records(const uint8_t *t, uint32_t len, uint32_t ll_off,
                                    uint16_t scnt, uint32_t srec_off,
                                    uint16_t *gids, int *plen, int max_out,
                                    int pos, int inp_len, int depth) {
    int delta_total = 0;
    for (uint16_t i = 0; i < scnt; i++) {
        uint32_t rec = srec_off + (uint32_t)i * 4u;
        if (rec + 4u > len) break;
        uint16_t seq_idx = ot_u16(t, len, rec);
        uint16_t sub_lidx = ot_u16(t, len, rec + 2u);
        int target = pos + (int)seq_idx + delta_total;
        if (target >= pos && target < *plen) {
            int before = *plen;
            (void)gsub_apply_lookup_at(t, len, ll_off, sub_lidx, gids, plen, max_out, target, depth + 1);
            delta_total += (*plen - before);
        }
    }
    int adv = inp_len + delta_total;
    return adv > 0 ? adv : 1;
}

static int gsub_apply_subtable(const uint8_t *t, uint32_t len, uint32_t ll_off,
                               uint16_t ltype, uint32_t sub,
                               uint16_t *gids, int *plen, int max_out,
                               int pos, int depth) {
    if (sub + 4u > len || pos < 0 || pos >= *plen) return 0;
    if (ltype == 7u) {
        if (sub + 8u > len) return 0;
        uint16_t ext_type = ot_u16(t, len, sub + 2u);
        uint32_t ext_off = ot_u32(t, len, sub + 4u);
        return gsub_apply_subtable(t, len, ll_off, ext_type, sub + ext_off,
                                   gids, plen, max_out, pos, depth);
    }

    uint16_t fmt = ot_u16(t, len, sub);
    if (ltype == 1u) {
        int cov = ot_coverage(t, len, sub + ot_u16(t, len, sub + 2u), gids[pos]);
        if (cov < 0) return 0;
        if (fmt == 1u) {
            gids[pos] = (uint16_t)((int32_t)gids[pos] + ot_s16(t, len, sub + 4u));
            return 1;
        }
        if (fmt == 2u) {
            uint16_t gc = ot_u16(t, len, sub + 4u);
            if ((uint16_t)cov >= gc) return 0;
            gids[pos] = ot_u16(t, len, sub + 6u + (uint32_t)cov * 2u);
            return 1;
        }
    } else if (ltype == 2u && fmt == 1u) {
        int cov = ot_coverage(t, len, sub + ot_u16(t, len, sub + 2u), gids[pos]);
        if (cov < 0) return 0;
        uint16_t scnt = ot_u16(t, len, sub + 4u);
        if ((uint16_t)cov >= scnt) return 0;
        uint32_t seq = sub + ot_u16(t, len, sub + 6u + (uint32_t)cov * 2u);
        if (seq + 2u > len) return 0;
        uint16_t gcnt = ot_u16(t, len, seq);
        if (gcnt == 0 || seq + 2u + (uint32_t)gcnt * 2u > len) return 0;
        int extra = (int)gcnt - 1;
        if (*plen + extra > max_out) return 0;
        if (extra > 0 && pos + 1 < *plen) {
            memmove(&gids[pos + gcnt], &gids[pos + 1], (size_t)(*plen - (pos + 1)) * sizeof(uint16_t));
        }
        for (uint16_t k = 0; k < gcnt; k++) {
            gids[pos + (int)k] = ot_u16(t, len, seq + 2u + (uint32_t)k * 2u);
        }
        *plen += extra;
        return (int)gcnt;
    } else if (ltype == 4u && fmt == 1u) {
        int cov = ot_coverage(t, len, sub + ot_u16(t, len, sub + 2u), gids[pos]);
        if (cov < 0) return 0;
        uint16_t lscnt = ot_u16(t, len, sub + 4u);
        if ((uint16_t)cov >= lscnt) return 0;
        uint32_t lset = sub + ot_u16(t, len, sub + 6u + (uint32_t)cov * 2u);
        if (lset + 2u > len) return 0;
        uint16_t lcnt = ot_u16(t, len, lset);
        for (uint16_t i = 0; i < lcnt; i++) {
            uint32_t lig = lset + ot_u16(t, len, lset + 2u + (uint32_t)i * 2u);
            if (lig + 4u > len) continue;
            uint16_t lig_gid = ot_u16(t, len, lig);
            uint16_t comp_cnt = ot_u16(t, len, lig + 2u);
            if (comp_cnt == 0 || pos + (int)comp_cnt > *plen) continue;
            bool ok = true;
            for (uint16_t c = 1; c < comp_cnt; c++) {
                if (gids[pos + (int)c] != ot_u16(t, len, lig + 4u + (uint32_t)(c - 1u) * 2u)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                gids[pos] = lig_gid;
                int drop = (int)comp_cnt - 1;
                if (drop > 0 && pos + (int)comp_cnt < *plen) {
                    memmove(&gids[pos + 1], &gids[pos + comp_cnt],
                            (size_t)(*plen - (pos + (int)comp_cnt)) * sizeof(uint16_t));
                }
                *plen -= drop;
                return 1;
            }
        }
    } else if (ltype == 6u) {
        if (fmt == 1u || fmt == 2u) {
            bool is_cls = (fmt == 2u);
            uint32_t cov_off = sub + ot_u16(t, len, sub + 2u);
            if (ot_coverage(t, len, cov_off, gids[pos]) < 0) return 0;
            uint32_t back_cd = 0, inp_cd = 0, look_cd = 0;
            uint32_t set_cnt_off;
            if (is_cls) {
                if (sub + 12u > len) return 0;
                back_cd = sub + ot_u16(t, len, sub + 4u);
                inp_cd  = sub + ot_u16(t, len, sub + 6u);
                look_cd = sub + ot_u16(t, len, sub + 8u);
                set_cnt_off = sub + 10u;
            } else {
                set_cnt_off = sub + 4u;
            }
            uint16_t rscnt = ot_u16(t, len, set_cnt_off);
            uint16_t set_idx = is_cls
                ? ot_classdef(t, len, inp_cd, gids[pos])
                : (uint16_t)ot_coverage(t, len, cov_off, gids[pos]);
            if (set_idx >= rscnt) return 0;
            uint16_t rs_rel = ot_u16(t, len, set_cnt_off + 2u + (uint32_t)set_idx * 2u);
            if (rs_rel == 0) return 0;
            uint32_t rset = sub + rs_rel;
            uint16_t rcnt = ot_u16(t, len, rset);
            for (uint16_t r = 0; r < rcnt; r++) {
                uint32_t rule = rset + ot_u16(t, len, rset + 2u + (uint32_t)r * 2u);
                if (rule + 8u > len) continue;
                uint16_t bc = ot_u16(t, len, rule);
                uint32_t bo = rule + 2u;
                uint16_t ic = ot_u16(t, len, bo + (uint32_t)bc * 2u);
                if (ic == 0) continue;
                uint32_t io = bo + (uint32_t)bc * 2u + 2u;
                uint16_t lc = ot_u16(t, len, io + (uint32_t)(ic - 1u) * 2u);
                uint32_t lo = io + (uint32_t)(ic - 1u) * 2u + 2u;
                uint16_t sc = ot_u16(t, len, lo + (uint32_t)lc * 2u);
                uint32_t so = lo + (uint32_t)lc * 2u + 2u;
                if (gsub_match_seq(t, len, gids, *plen, pos, bc, bo, ic, io, lc, lo,
                                   is_cls, back_cd, inp_cd, look_cd, false)) {
                    return gsub_apply_subst_records(t, len, ll_off, sc, so, gids, plen, max_out, pos, (int)ic, depth);
                }
            }
        } else if (fmt == 3u) {
            uint16_t bc = ot_u16(t, len, sub + 2u);
            uint32_t bo = sub + 4u;
            uint16_t ic = ot_u16(t, len, bo + (uint32_t)bc * 2u);
            if (ic == 0) return 0;
            uint32_t io = bo + (uint32_t)bc * 2u + 2u;
            uint16_t lc = ot_u16(t, len, io + (uint32_t)ic * 2u);
            uint32_t lo = io + (uint32_t)ic * 2u + 2u;
            uint16_t sc = ot_u16(t, len, lo + (uint32_t)lc * 2u);
            uint32_t so = lo + (uint32_t)lc * 2u + 2u;
            if (gsub_match_seq(t, len, gids, *plen, pos, bc, bo, ic, io, lc, lo,
                               false, sub, 0, 0, true)) {
                return gsub_apply_subst_records(t, len, ll_off, sc, so, gids, plen, max_out, pos, (int)ic, depth);
            }
        }
    }
    return 0;
}

static int gsub_apply_lookup_at(const uint8_t *t, uint32_t len, uint32_t ll_off,
                                uint16_t lidx, uint16_t *gids, int *plen,
                                int max_out, int pos, int depth) {
    if (depth > 8) return 0;
    uint16_t lcount = ot_u16(t, len, ll_off);
    if (lidx >= lcount) return 0;
    uint32_t loff = ll_off + ot_u16(t, len, ll_off + 2u + (uint32_t)lidx * 2u);
    if (loff + 6u > len) return 0;
    uint16_t ltype = ot_u16(t, len, loff);
    uint16_t sub_cnt = ot_u16(t, len, loff + 4u);
    for (uint16_t s = 0; s < sub_cnt; s++) {
        uint32_t sub = loff + ot_u16(t, len, loff + 6u + (uint32_t)s * 2u);
        int adv = gsub_apply_subtable(t, len, ll_off, ltype, sub, gids, plen, max_out, pos, depth);
        if (adv > 0) return adv;
    }
    return 0;
}

static uint32_t gpos_valrec_bytes(uint16_t vfmt) {
    uint32_t cnt = 0;
    for (uint32_t b = 0; b < 8u; b++) {
        if (vfmt & (1u << b)) cnt++;
    }
    return cnt * 2u;
}

static int16_t gpos_valrec_xadv(const uint8_t *t, uint32_t len, uint32_t off, uint16_t vfmt) {
    if (!(vfmt & 0x0004u)) return 0;
    uint32_t skip = 0;
    if (vfmt & 0x0001u) skip += 2u;
    if (vfmt & 0x0002u) skip += 2u;
    return ot_s16(t, len, off + skip);
}

static int16_t gpos_pair_kern(const uint8_t *t, uint32_t len, uint32_t sub,
                              uint16_t g1, uint16_t g2, bool *matched) {
    if (sub + 10u > len) return 0;
    uint16_t fmt = ot_u16(t, len, sub);
    if (fmt == 9u) {
        uint16_t ext_type = ot_u16(t, len, sub + 2u);
        uint32_t ext_off = ot_u32(t, len, sub + 4u);
        if (ext_type == 2u) return gpos_pair_kern(t, len, sub + ext_off, g1, g2, matched);
        return 0;
    }
    int cov = ot_coverage(t, len, sub + ot_u16(t, len, sub + 2u), g1);
    if (cov < 0) return 0;

    uint16_t vf1 = ot_u16(t, len, sub + 4u);
    uint16_t vf2 = ot_u16(t, len, sub + 6u);
    uint32_t vs1 = gpos_valrec_bytes(vf1);
    uint32_t vs2 = gpos_valrec_bytes(vf2);

    if (fmt == 1u) {
        uint16_t psc = ot_u16(t, len, sub + 8u);
        if ((uint16_t)cov >= psc) return 0;
        uint32_t pset = sub + ot_u16(t, len, sub + 10u + (uint32_t)cov * 2u);
        if (pset + 2u > len) return 0;
        uint16_t pvc = ot_u16(t, len, pset);
        uint32_t rec_sz = 2u + vs1 + vs2;
        if (pset + 2u + (uint32_t)pvc * rec_sz > len) return 0;
        int lo = 0, hi = (int)pvc - 1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t rec = pset + 2u + (uint32_t)mid * rec_sz;
            uint16_t sg = ot_u16(t, len, rec);
            if (sg == g2) {
                *matched = true;
                return gpos_valrec_xadv(t, len, rec + 2u, vf1);
            }
            if (sg < g2) lo = mid + 1;
            else hi = mid - 1;
        }
    } else if (fmt == 2u) {
        if (sub + 16u > len) return 0;
        uint32_t cd1 = sub + ot_u16(t, len, sub + 8u);
        uint32_t cd2 = sub + ot_u16(t, len, sub + 10u);
        uint16_t c1cnt = ot_u16(t, len, sub + 12u);
        uint16_t c2cnt = ot_u16(t, len, sub + 14u);
        uint16_t c1 = ot_classdef(t, len, cd1, g1);
        uint16_t c2 = ot_classdef(t, len, cd2, g2);
        if (c1 >= c1cnt || c2 >= c2cnt) return 0;
        uint32_t cell_sz = vs1 + vs2;
        uint32_t off = sub + 16u + ((uint32_t)c1 * c2cnt + c2) * cell_sz;
        if (off + cell_sz > len) return 0;
        *matched = true;
        return gpos_valrec_xadv(t, len, off, vf1);
    }
    return 0;
}

static int16_t legacy_kern_pair(const uint8_t *t, uint32_t len, uint16_t g1, uint16_t g2) {
    if (!t || len < 18u) return 0;
    uint16_t ver = ot_u16(t, len, 0);
    if (ver != 0) return 0;
    uint16_t ntables = ot_u16(t, len, 2u);
    uint32_t pos = 4u;
    for (uint16_t st = 0; st < ntables && pos + 6u <= len; st++) {
        uint16_t slen = ot_u16(t, len, pos + 2u);
        uint16_t cov = ot_u16(t, len, pos + 4u);
        if (slen < 6u || pos + slen > len) break;
        if ((cov & 0xFF00u) == 0u && (cov & 0x0001u) != 0u && slen >= 14u) {
            uint16_t npairs = ot_u16(t, len, pos + 6u);
            uint32_t arr = pos + 14u;
            if (arr + (uint32_t)npairs * 6u <= pos + slen) {
                uint32_t key = ((uint32_t)g1 << 16) | g2;
                int lo = 0, hi = (int)npairs - 1;
                while (lo <= hi) {
                    int mid = lo + (hi - lo) / 2;
                    uint32_t rec = arr + (uint32_t)mid * 6u;
                    uint32_t k = ot_u32(t, len, rec);
                    if (k == key) return ot_s16(t, len, rec + 4u);
                    if (k < key) lo = mid + 1;
                    else hi = mid - 1;
                }
            }
        }
        pos += slen;
    }
    return 0;
}

int ttf_ot_shape(const ttf_font_t *f, const ttf_ot_t *ot,
                 const uint32_t *cps, int count,
                 uint16_t *out_gids, int16_t *out_kern_fu, int max_out) {
    if (!f || !cps || !out_gids || count <= 0 || max_out <= 0) return 0;
    int len = count < max_out ? count : max_out;
    for (int i = 0; i < len; i++) {
        out_gids[i] = ttf_glyph_index(f, cps[i]);
    }
    if (out_kern_fu) memset(out_kern_fu, 0, (size_t)len * sizeof(int16_t));

    if (ot && ot->gsub && ot->gsub_len >= 10u) {
        static const uint32_t k_gsub_feats[] = {
            OT_TAG('c', 'c', 'm', 'p'),
            OT_TAG('a', 'b', 'v', 's'),
            OT_TAG('b', 'l', 'w', 's'),
            OT_TAG('c', 'a', 'l', 't'),
            OT_TAG('l', 'i', 'g', 'a'),
        };
        uint8_t active[OT_MAX_LOOKUPS / 8];
        uint16_t lcount = 0;
        if (ot_collect_lookups(ot->gsub, ot->gsub_len, ot->script_tag,
                               k_gsub_feats, (int)(sizeof(k_gsub_feats) / sizeof(k_gsub_feats[0])),
                               active, &lcount) > 0) {
            uint16_t ll_off = ot_u16(ot->gsub, ot->gsub_len, 8u);
            for (uint16_t li = 0; li < lcount; li++) {
                if (!(active[li >> 3] & (1u << (li & 7u)))) continue;
                int p = 0;
                while (p < len) {
                    int adv = gsub_apply_lookup_at(ot->gsub, ot->gsub_len, ll_off,
                                                   li, out_gids, &len, max_out, p, 0);
                    p += (adv > 0) ? adv : 1;
                }
            }
        }
    }

    if (out_kern_fu) {
        memset(out_kern_fu, 0, (size_t)len * sizeof(int16_t));
        bool got_gpos = false;
        if (ot && ot->gpos && ot->gpos_len >= 10u && len > 1) {
            static const uint32_t k_gpos_feats[] = { OT_TAG('k', 'e', 'r', 'n') };
            uint8_t active[OT_MAX_LOOKUPS / 8];
            uint16_t lcount = 0;
            if (ot_collect_lookups(ot->gpos, ot->gpos_len, ot->script_tag,
                                   k_gpos_feats, 1, active, &lcount) > 0) {
                uint16_t ll_off = ot_u16(ot->gpos, ot->gpos_len, 8u);
                for (uint16_t li = 0; li < lcount; li++) {
                    if (!(active[li >> 3] & (1u << (li & 7u)))) continue;
                    uint32_t loff = ll_off + ot_u16(ot->gpos, ot->gpos_len, ll_off + 2u + (uint32_t)li * 2u);
                    if (loff + 6u > ot->gpos_len) continue;
                    uint16_t ltype = ot_u16(ot->gpos, ot->gpos_len, loff);
                    if (ltype != 2u && ltype != 9u) continue;
                    got_gpos = true;
                    uint16_t scnt = ot_u16(ot->gpos, ot->gpos_len, loff + 4u);
                    for (int i = 0; i + 1 < len; i++) {
                        for (uint16_t s = 0; s < scnt; s++) {
                            uint32_t sub = loff + ot_u16(ot->gpos, ot->gpos_len, loff + 6u + (uint32_t)s * 2u);
                            bool matched = false;
                            int16_t kv = gpos_pair_kern(ot->gpos, ot->gpos_len, sub,
                                                        out_gids[i], out_gids[i + 1], &matched);
                            if (matched) {
                                out_kern_fu[i] = (int16_t)(out_kern_fu[i] + kv);
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!got_gpos && ot && ot->kern && ot->kern_len >= 18u && len > 1) {
            for (int i = 0; i + 1 < len; i++) {
                out_kern_fu[i] = legacy_kern_pair(ot->kern, ot->kern_len, out_gids[i], out_gids[i + 1]);
            }
        }
    }

    return len;
}

int ttf_ot_scale_fu(const ttf_font_t *f, int cell_h, int32_t fu) {
    if (!f || cell_h <= 0 || fu == 0) return 0;
    int32_t span = (int32_t)f->ascent - (int32_t)f->descent;
    if (span <= 0) span = (int32_t)f->units_per_em;
    if (span <= 0) return 0;
    int64_t num = (int64_t)fu * cell_h;
    if (num >= 0) return (int)((num + span / 2) / span);
    return -(int)(((-num) + span / 2) / span);
}

#endif /* LUGALOS_HOST_TEST || CONFIG_PSRAM_BYTES || CONFIG_BOARD_VISIONFIVE2 */
