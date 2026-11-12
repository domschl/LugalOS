/*
 * Freestanding RFC 1950 (zlib) and RFC 1951 (DEFLATE) compression/decompression,
 * plus Adler-32 and CRC-32. Zero static writable storage.
 */

#include "kernel/zlib.h"
#include <stdbool.h>
#include <string.h>

/* --- Checksums -------------------------------------------------------------- */

uint32_t zlib_adler32(uint32_t adler, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t s1 = adler & 0xffffu;
    uint32_t s2 = (adler >> 16) & 0xffffu;
    while (len > 0) {
        size_t chunk = len > 5552u ? 5552u : len;
        len -= chunk;
        for (size_t i = 0; i < chunk; i++) {
            s1 += p[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
        p += chunk;
    }
    return (s2 << 16) | s1;
}

uint32_t zlib_crc32(uint32_t crc, const void *buf, size_t len) {
    static const uint32_t tbl[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
    };
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ tbl[crc & 0x0fu];
        crc = (crc >> 4) ^ tbl[crc & 0x0fu];
    }
    return ~crc;
}

/* --- RFC 1951 Tables (.rodata) ---------------------------------------------- */

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
    15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
    33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3,
    4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const uint8_t k_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* --- Inflate ---------------------------------------------------------------- */

#define HUFF_MAX_BITS 15u
#define HUFF_MAX_SYMS 288u

typedef struct {
    const uint8_t *src;
    size_t         len;
    size_t         pos;
    uint32_t       bitbuf;
    unsigned       bitcnt;
} bs_t;

typedef struct {
    uint16_t count[HUFF_MAX_BITS + 1u];
    uint16_t sym[HUFF_MAX_SYMS];
} huff_t;

static bool bs_ensure(bs_t *bs, unsigned n) {
    while (bs->bitcnt < n) {
        if (bs->pos >= bs->len) return false;
        bs->bitbuf |= (uint32_t)bs->src[bs->pos++] << bs->bitcnt;
        bs->bitcnt += 8u;
    }
    return true;
}

static bool bs_read_bits(bs_t *bs, unsigned n, uint32_t *out) {
    if (n == 0) {
        *out = 0;
        return true;
    }
    if (!bs_ensure(bs, n)) return false;
    *out = bs->bitbuf & ((1u << n) - 1u);
    bs->bitbuf >>= n;
    bs->bitcnt -= n;
    return true;
}

static void bs_align(bs_t *bs) {
    unsigned drop = bs->bitcnt & 7u;
    bs->bitbuf >>= drop;
    bs->bitcnt -= drop;
}

static size_t bs_bytes_consumed(const bs_t *bs) {
    return bs->pos - (bs->bitcnt / 8u);
}

static bool huff_build(huff_t *h, const uint8_t *lens, unsigned nsyms) {
    uint16_t offs[HUFF_MAX_BITS + 1u];
    for (unsigned i = 0; i <= HUFF_MAX_BITS; i++) h->count[i] = 0;
    for (unsigned i = 0; i < nsyms; i++) {
        if (lens[i] > HUFF_MAX_BITS) return false;
        h->count[lens[i]]++;
    }
    if (h->count[0] == nsyms) return true;

    int left = 1;
    for (unsigned b = 1; b <= HUFF_MAX_BITS; b++) {
        left <<= 1;
        left -= h->count[b];
        if (left < 0) return false;
    }

    offs[1] = 0;
    for (unsigned b = 1; b < HUFF_MAX_BITS; b++) {
        offs[b + 1u] = (uint16_t)(offs[b] + h->count[b]);
    }
    for (unsigned i = 0; i < nsyms; i++) {
        if (lens[i] != 0) h->sym[offs[lens[i]]++] = (uint16_t)i;
    }
    return true;
}

static bool huff_decode(bs_t *bs, const huff_t *h, unsigned *sym_out) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (unsigned len = 1; len <= HUFF_MAX_BITS; len++) {
        uint32_t bit;
        if (!bs_read_bits(bs, 1u, &bit)) return false;
        code |= (int)bit;
        int count = h->count[len];
        if (code - count < first) {
            *sym_out = h->sym[index + (code - first)];
            return true;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return false;
}

static void build_fixed_trees(huff_t *lit, huff_t *dist) {
    uint8_t lens[288];
    for (unsigned i = 0; i < 144; i++) lens[i] = 8;
    for (unsigned i = 144; i < 256; i++) lens[i] = 9;
    for (unsigned i = 256; i < 280; i++) lens[i] = 7;
    for (unsigned i = 280; i < 288; i++) lens[i] = 8;
    (void)huff_build(lit, lens, 288);
    for (unsigned i = 0; i < 32; i++) lens[i] = 5;
    (void)huff_build(dist, lens, 32);
}

static int decode_dynamic_trees(bs_t *bs, huff_t *lit, huff_t *dist) {
    uint32_t hlit_r, hdist_r, hclen_r;
    if (!bs_read_bits(bs, 5u, &hlit_r) ||
        !bs_read_bits(bs, 5u, &hdist_r) ||
        !bs_read_bits(bs, 4u, &hclen_r)) {
        return ZLIB_ERR_DATA;
    }
    unsigned hlit = hlit_r + 257u;
    unsigned hdist = hdist_r + 1u;
    unsigned hclen = hclen_r + 4u;
    if (hlit > 286u || hdist > 30u) return ZLIB_ERR_DATA;

    uint8_t cl_lens[19];
    memset(cl_lens, 0, sizeof(cl_lens));
    for (unsigned i = 0; i < hclen; i++) {
        uint32_t v;
        if (!bs_read_bits(bs, 3u, &v)) return ZLIB_ERR_DATA;
        cl_lens[k_cl_order[i]] = (uint8_t)v;
    }
    huff_t cl_tree;
    if (!huff_build(&cl_tree, cl_lens, 19u)) return ZLIB_ERR_DATA;

    uint8_t lens[286u + 30u];
    unsigned total = hlit + hdist;
    unsigned idx = 0;
    while (idx < total) {
        unsigned sym;
        if (!huff_decode(bs, &cl_tree, &sym)) return ZLIB_ERR_DATA;
        if (sym < 16u) {
            lens[idx++] = (uint8_t)sym;
        } else {
            uint8_t rep_val = 0;
            unsigned rep_cnt = 0;
            uint32_t extra = 0;
            if (sym == 16u) {
                if (idx == 0 || !bs_read_bits(bs, 2u, &extra)) return ZLIB_ERR_DATA;
                rep_val = lens[idx - 1u];
                rep_cnt = 3u + extra;
            } else if (sym == 17u) {
                if (!bs_read_bits(bs, 3u, &extra)) return ZLIB_ERR_DATA;
                rep_cnt = 3u + extra;
            } else if (sym == 18u) {
                if (!bs_read_bits(bs, 7u, &extra)) return ZLIB_ERR_DATA;
                rep_cnt = 11u + extra;
            } else {
                return ZLIB_ERR_DATA;
            }
            if (idx + rep_cnt > total) return ZLIB_ERR_DATA;
            for (unsigned k = 0; k < rep_cnt; k++) lens[idx++] = rep_val;
        }
    }
    if (lens[256] == 0) return ZLIB_ERR_DATA;
    if (!huff_build(lit, lens, hlit)) return ZLIB_ERR_DATA;
    if (!huff_build(dist, lens + hlit, hdist)) return ZLIB_ERR_DATA;
    return ZLIB_OK;
}

int zlib_inflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap,
                     size_t *out_len, size_t *in_consumed) {
    if (!src || (!dst && dst_cap > 0)) return ZLIB_ERR_INVAL;
    bs_t bs = { src, src_len, 0, 0, 0 };
    size_t out_pos = 0;
    uint32_t bfinal = 0;

    do {
        uint32_t btype = 0;
        if (!bs_read_bits(&bs, 1u, &bfinal) || !bs_read_bits(&bs, 2u, &btype)) {
            return ZLIB_ERR_DATA;
        }
        if (btype == 0u) {
            bs_align(&bs);
            uint32_t lo, hi;
            if (!bs_read_bits(&bs, 16u, &lo) || !bs_read_bits(&bs, 16u, &hi)) {
                return ZLIB_ERR_DATA;
            }
            if ((lo ^ 0xffffu) != hi) return ZLIB_ERR_DATA;
            if (out_pos + lo > dst_cap) return ZLIB_ERR_SPACE;
            /* Consume bytes directly from bitbuf-aligned stream position. */
            size_t cur = bs_bytes_consumed(&bs);
            if (cur + lo > bs.len) return ZLIB_ERR_DATA;
            if (lo > 0) memcpy(dst + out_pos, bs.src + cur, lo);
            out_pos += lo;
            bs.pos = cur + lo;
            bs.bitbuf = 0;
            bs.bitcnt = 0;
        } else if (btype == 1u || btype == 2u) {
            huff_t lit, dist;
            if (btype == 1u) {
                build_fixed_trees(&lit, &dist);
            } else {
                int rc = decode_dynamic_trees(&bs, &lit, &dist);
                if (rc != ZLIB_OK) return rc;
            }
            for (;;) {
                unsigned sym;
                if (!huff_decode(&bs, &lit, &sym)) return ZLIB_ERR_DATA;
                if (sym < 256u) {
                    if (out_pos >= dst_cap) return ZLIB_ERR_SPACE;
                    dst[out_pos++] = (uint8_t)sym;
                } else if (sym == 256u) {
                    break;
                } else {
                    if (sym > 285u) return ZLIB_ERR_DATA;
                    unsigned lidx = sym - 257u;
                    uint32_t lextra = 0;
                    if (!bs_read_bits(&bs, k_len_extra[lidx], &lextra)) return ZLIB_ERR_DATA;
                    size_t mlen = k_len_base[lidx] + lextra;

                    unsigned dsym;
                    if (!huff_decode(&bs, &dist, &dsym) || dsym >= 30u) return ZLIB_ERR_DATA;
                    uint32_t dextra = 0;
                    if (!bs_read_bits(&bs, k_dist_extra[dsym], &dextra)) return ZLIB_ERR_DATA;
                    size_t mdist = k_dist_base[dsym] + dextra;

                    if (mdist == 0 || mdist > out_pos) return ZLIB_ERR_DATA;
                    if (out_pos + mlen > dst_cap) return ZLIB_ERR_SPACE;
                    for (size_t k = 0; k < mlen; k++) {
                        dst[out_pos] = dst[out_pos - mdist];
                        out_pos++;
                    }
                }
            }
        } else {
            return ZLIB_ERR_DATA;
        }
    } while (!bfinal);

    if (out_len) *out_len = out_pos;
    if (in_consumed) *in_consumed = bs_bytes_consumed(&bs);
    return ZLIB_OK;
}

int zlib_inflate(const uint8_t *src, size_t src_len,
                 uint8_t *dst, size_t dst_cap,
                 size_t *out_len, size_t *in_consumed) {
    if (!src || src_len < 6u) return ZLIB_ERR_DATA;
    uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0fu) != 8u || (cmf >> 4) > 7u) return ZLIB_ERR_DATA;
    if ((((uint16_t)cmf << 8) | flg) % 31u != 0) return ZLIB_ERR_DATA;
    if (flg & 0x20u) return ZLIB_ERR_DATA; /* FDICT not supported */

    size_t raw_out = 0, raw_in = 0;
    int rc = zlib_inflate_raw(src + 2u, src_len - 2u, dst, dst_cap, &raw_out, &raw_in);
    if (rc != ZLIB_OK) return rc;
    if (2u + raw_in + 4u > src_len) return ZLIB_ERR_DATA;

    const uint8_t *adp = src + 2u + raw_in;
    uint32_t want = ((uint32_t)adp[0] << 24) | ((uint32_t)adp[1] << 16) |
                    ((uint32_t)adp[2] << 8) | (uint32_t)adp[3];
    uint32_t got = zlib_adler32(1u, dst, raw_out);
    if (want != got) return ZLIB_ERR_DATA;

    if (out_len) *out_len = raw_out;
    if (in_consumed) *in_consumed = 2u + raw_in + 4u;
    return ZLIB_OK;
}

/* --- Deflate (LZ77 + Fixed Huffman) ----------------------------------------- */

typedef struct {
    uint8_t *dst;
    size_t   cap;
    size_t   pos;
    uint32_t bitbuf;
    unsigned bitcnt;
} bbw_t;

static bool bbw_write_bits(bbw_t *w, uint32_t val, unsigned n) {
    w->bitbuf |= val << w->bitcnt;
    w->bitcnt += n;
    while (w->bitcnt >= 8u) {
        if (w->pos >= w->cap) return false;
        w->dst[w->pos++] = (uint8_t)(w->bitbuf & 0xffu);
        w->bitbuf >>= 8;
        w->bitcnt -= 8u;
    }
    return true;
}

static bool bbw_write_huff(bbw_t *w, uint32_t code_msb, unsigned n) {
    uint32_t rev = 0;
    for (unsigned i = 0; i < n; i++) {
        rev = (rev << 1) | (code_msb & 1u);
        code_msb >>= 1;
    }
    return bbw_write_bits(w, rev, n);
}

static bool bbw_flush(bbw_t *w) {
    if (w->bitcnt > 0) {
        if (w->pos >= w->cap) return false;
        w->dst[w->pos++] = (uint8_t)(w->bitbuf & 0xffu);
        w->bitbuf = 0;
        w->bitcnt = 0;
    }
    return true;
}

static bool emit_fixed_lit(bbw_t *w, unsigned sym) {
    if (sym <= 143u) return bbw_write_huff(w, 0x30u + sym, 8u);
    if (sym <= 255u) return bbw_write_huff(w, 0x190u + (sym - 144u), 9u);
    if (sym <= 279u) return bbw_write_huff(w, sym - 256u, 7u);
    return bbw_write_huff(w, 0xc0u + (sym - 280u), 8u);
}

static bool emit_match(bbw_t *w, size_t len, size_t dist) {
    unsigned li = 0;
    while (li + 1u < 29u && k_len_base[li + 1u] <= len) li++;
    if (!emit_fixed_lit(w, 257u + li)) return false;
    if (k_len_extra[li] > 0) {
        if (!bbw_write_bits(w, (uint32_t)(len - k_len_base[li]), k_len_extra[li])) return false;
    }
    unsigned di = 0;
    while (di + 1u < 30u && k_dist_base[di + 1u] <= dist) di++;
    if (!bbw_write_huff(w, di, 5u)) return false;
    if (k_dist_extra[di] > 0) {
        if (!bbw_write_bits(w, (uint32_t)(dist - k_dist_base[di]), k_dist_extra[di])) return false;
    }
    return true;
}

#define DEFLATE_HASH_SZ 512u

static uint32_t hash3(const uint8_t *p) {
    uint32_t v = ((uint32_t)p[0] << 10) ^ ((uint32_t)p[1] << 5) ^ (uint32_t)p[2];
    return v & (DEFLATE_HASH_SZ - 1u);
}

int zlib_deflate_raw(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap,
                     size_t *out_len) {
    if ((!src && src_len > 0) || (!dst && dst_cap > 0)) return ZLIB_ERR_INVAL;

    bbw_t w = { dst, dst_cap, 0, 0, 0 };
    if (!bbw_write_bits(&w, 1u, 1u) || !bbw_write_bits(&w, 1u, 2u)) return ZLIB_ERR_SPACE;

    /* 1 KB stack-only 3-byte hash table; stores (pos + 1) mod 65536 (0 = empty). */
    uint16_t head[DEFLATE_HASH_SZ];
    memset(head, 0, sizeof(head));

    size_t pos = 0;
    while (pos < src_len) {
        size_t best_len = 0;
        size_t best_dist = 0;
        if (pos + 3u <= src_len) {
            uint32_t h = hash3(src + pos);
            uint16_t slot = head[h];
            head[h] = (uint16_t)((pos + 1u) & 0xffffu);
            if (slot != 0) {
                /* Reconstruct candidate offset in the current 64 KB window. */
                size_t cur_lo = (pos + 1u) & 0xffffu;
                size_t dist = (cur_lo >= slot) ? (cur_lo - slot) : (0x10000u + cur_lo - slot);
                if (dist > 0 && dist <= 32768u && dist <= pos) {
                    size_t mpos = pos - dist;
                    size_t maxl = src_len - pos;
                    if (maxl > 258u) maxl = 258u;
                    size_t l = 0;
                    while (l < maxl && src[mpos + l] == src[pos + l]) l++;
                    if (l >= 3u) {
                        best_len = l;
                        best_dist = dist;
                    }
                }
            }
        }
        if (best_len >= 3u) {
            if (!emit_match(&w, best_len, best_dist)) return ZLIB_ERR_SPACE;
            pos += best_len;
        } else {
            if (!emit_fixed_lit(&w, src[pos])) return ZLIB_ERR_SPACE;
            pos++;
        }
    }
    if (!emit_fixed_lit(&w, 256u) || !bbw_flush(&w)) return ZLIB_ERR_SPACE;
    if (out_len) *out_len = w.pos;
    return ZLIB_OK;
}

int zlib_deflate(const uint8_t *src, size_t src_len,
                 uint8_t *dst, size_t dst_cap,
                 size_t *out_len) {
    if ((!src && src_len > 0) || !dst) return ZLIB_ERR_INVAL;
    if (dst_cap < 6u) return ZLIB_ERR_SPACE;

    /* Standard zlib header: CMF=0x78 (32K window, deflate), FLG=0x9c (default). */
    dst[0] = 0x78u;
    dst[1] = 0x9cu;
    size_t raw_len = 0;
    int rc = zlib_deflate_raw(src, src_len, dst + 2u, dst_cap - 6u, &raw_len);
    if (rc != ZLIB_OK) return rc;

    uint32_t ad = zlib_adler32(1u, src, src_len);
    uint8_t *tail = dst + 2u + raw_len;
    tail[0] = (uint8_t)(ad >> 24);
    tail[1] = (uint8_t)(ad >> 16);
    tail[2] = (uint8_t)(ad >> 8);
    tail[3] = (uint8_t)ad;
    if (out_len) *out_len = 2u + raw_len + 4u;
    return ZLIB_OK;
}
