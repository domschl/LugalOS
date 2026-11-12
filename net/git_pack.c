/*
 * Freestanding Git Packfile v2 unpacker (including OFS_DELTA and REF_DELTA),
 * binary delta applier, and Packfile v2 builder. Zero static writable storage.
 */

#include "net/git.h"

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_GIT_CLIENT)

#include "kernel/crypto.h"
#include "kernel/zlib.h"

#include <string.h>

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static bool read_delta_varint(const uint8_t *buf, size_t len, size_t *pos, size_t *out) {
    size_t v = 0, shift = 0;
    while (*pos < len) {
        uint8_t c = buf[(*pos)++];
        v |= (size_t)(c & 0x7fu) << shift;
        if (!(c & 0x80u)) {
            *out = v;
            return true;
        }
        shift += 7u;
        if (shift >= 32u) return false;
    }
    return false;
}

int git_apply_delta(const uint8_t *base, size_t base_len,
                    const uint8_t *delta, size_t delta_len,
                    uint8_t **out_buf, size_t *out_len) {
    if (!base || !delta || !out_buf || !out_len) return GIT_ERR_INVAL;
    size_t pos = 0, src_sz = 0, dst_sz = 0;
    if (!read_delta_varint(delta, delta_len, &pos, &src_sz) ||
        !read_delta_varint(delta, delta_len, &pos, &dst_sz)) {
        return GIT_ERR_CORRUPT;
    }
    if (src_sz != base_len) return GIT_ERR_CORRUPT;

    uint8_t *out = (uint8_t *)git_alloc(dst_sz + 1u);
    if (!out) return GIT_ERR_NOMEM;
    size_t opos = 0;

    while (pos < delta_len) {
        uint8_t cmd = delta[pos++];
        if (cmd & 0x80u) {
            uint32_t cp_off = 0, cp_len = 0;
            if (cmd & 0x01u) { if (pos >= delta_len) goto bad; cp_off |= (uint32_t)delta[pos++]; }
            if (cmd & 0x02u) { if (pos >= delta_len) goto bad; cp_off |= (uint32_t)delta[pos++] << 8; }
            if (cmd & 0x04u) { if (pos >= delta_len) goto bad; cp_off |= (uint32_t)delta[pos++] << 16; }
            if (cmd & 0x08u) { if (pos >= delta_len) goto bad; cp_off |= (uint32_t)delta[pos++] << 24; }
            if (cmd & 0x10u) { if (pos >= delta_len) goto bad; cp_len |= (uint32_t)delta[pos++]; }
            if (cmd & 0x20u) { if (pos >= delta_len) goto bad; cp_len |= (uint32_t)delta[pos++] << 8; }
            if (cmd & 0x40u) { if (pos >= delta_len) goto bad; cp_len |= (uint32_t)delta[pos++] << 16; }
            if (cp_len == 0) cp_len = 0x10000u;
            if ((size_t)cp_off + cp_len > base_len || opos + cp_len > dst_sz) goto bad;
            memcpy(out + opos, base + cp_off, cp_len);
            opos += cp_len;
        } else if (cmd > 0) {
            if (pos + cmd > delta_len || opos + cmd > dst_sz) goto bad;
            memcpy(out + opos, delta + pos, cmd);
            pos += cmd;
            opos += cmd;
        } else {
            goto bad;
        }
    }

    if (opos != dst_sz) goto bad;
    out[dst_sz] = '\0';
    *out_buf = out;
    *out_len = dst_sz;
    return GIT_OK;

bad:
    git_free(out, dst_sz + 1u);
    return GIT_ERR_CORRUPT;
}

typedef struct {
    size_t  offset;
    uint8_t sha1[GIT_SHA1_LEN];
} pack_idx_t;

int git_unpack_packfile(const char *repo_dir, const uint8_t *pack, size_t pack_len) {
    if (!repo_dir || !pack || pack_len < 32u) return GIT_ERR_INVAL;
    if (memcmp(pack, "PACK", 4u) != 0 || rd_be32(pack + 4u) != 2u) return GIT_ERR_CORRUPT;

    /* Verify trailing SHA-1 checksum over the packfile body. */
    uint8_t expect_sha1[GIT_SHA1_LEN];
    sha1(pack, (uint32_t)(pack_len - GIT_SHA1_LEN), expect_sha1);
    if (memcmp(expect_sha1, pack + pack_len - GIT_SHA1_LEN, GIT_SHA1_LEN) != 0) {
        return GIT_ERR_CORRUPT;
    }

    uint32_t num_objs = rd_be32(pack + 8u);
    size_t body_end = pack_len - GIT_SHA1_LEN;
    size_t pos = 12u;

    pack_idx_t *idx = (pack_idx_t *)git_alloc((num_objs ? num_objs : 1u) * sizeof(pack_idx_t));
    if (!idx) return GIT_ERR_NOMEM;

    int rc = GIT_OK;
    for (uint32_t i = 0; i < num_objs; i++) {
        if (pos >= body_end) { rc = GIT_ERR_CORRUPT; break; }
        size_t obj_off = pos;

        uint8_t c = pack[pos++];
        int type = (c >> 4) & 7;
        size_t uncomp_sz = c & 0x0fu;
        size_t shift = 4u;
        while (c & 0x80u) {
            if (pos >= body_end || shift >= 31u) { rc = GIT_ERR_CORRUPT; break; }
            c = pack[pos++];
            uncomp_sz |= (size_t)(c & 0x7fu) << shift;
            shift += 7u;
        }
        if (rc != GIT_OK) break;

        uint8_t base_sha1[GIT_SHA1_LEN];
        bool is_delta = false;
        if (type == GIT_OBJ_OFS_DELTA) {
            is_delta = true;
            if (pos >= body_end) { rc = GIT_ERR_CORRUPT; break; }
            uint8_t oc = pack[pos++];
            size_t rel_off = oc & 0x7fu;
            while (oc & 0x80u) {
                if (pos >= body_end) { rc = GIT_ERR_CORRUPT; break; }
                oc = pack[pos++];
                rel_off = ((rel_off + 1u) << 7) | (oc & 0x7fu);
            }
            if (rc != GIT_OK || rel_off > obj_off) { rc = GIT_ERR_CORRUPT; break; }
            size_t target_off = obj_off - rel_off;
            bool found = false;
            for (uint32_t k = 0; k < i; k++) {
                if (idx[k].offset == target_off) {
                    memcpy(base_sha1, idx[k].sha1, GIT_SHA1_LEN);
                    found = true;
                    break;
                }
            }
            if (!found) { rc = GIT_ERR_CORRUPT; break; }
        } else if (type == GIT_OBJ_REF_DELTA) {
            is_delta = true;
            if (pos + GIT_SHA1_LEN > body_end) { rc = GIT_ERR_CORRUPT; break; }
            memcpy(base_sha1, pack + pos, GIT_SHA1_LEN);
            pos += GIT_SHA1_LEN;
        }

        uint8_t *obj_buf = (uint8_t *)git_alloc(uncomp_sz + 1u);
        if (!obj_buf) { rc = GIT_ERR_NOMEM; break; }
        size_t out_len = 0, consumed = 0;
        int zrc = zlib_inflate(pack + pos, body_end - pos, obj_buf, uncomp_sz, &out_len, &consumed);
        if (zrc != ZLIB_OK || out_len != uncomp_sz) {
            git_free(obj_buf, uncomp_sz + 1u);
            rc = GIT_ERR_CORRUPT;
            break;
        }
        pos += consumed;

        idx[i].offset = obj_off;
        if (!is_delta) {
            rc = git_write_loose_obj(repo_dir, type, obj_buf, out_len, idx[i].sha1);
            git_free(obj_buf, uncomp_sz + 1u);
            if (rc != GIT_OK) break;
        } else {
            int base_type = -1;
            uint8_t *base_buf = NULL;
            size_t base_len = 0;
            rc = git_read_loose_obj(repo_dir, base_sha1, &base_type, &base_buf, &base_len);
            if (rc != GIT_OK) {
                git_free(obj_buf, uncomp_sz + 1u);
                break;
            }
            uint8_t *res_buf = NULL;
            size_t res_len = 0;
            rc = git_apply_delta(base_buf, base_len, obj_buf, out_len, &res_buf, &res_len);
            git_free(base_buf, base_len + 1u);
            git_free(obj_buf, uncomp_sz + 1u);
            if (rc != GIT_OK) break;
            rc = git_write_loose_obj(repo_dir, base_type, res_buf, res_len, idx[i].sha1);
            git_free(res_buf, res_len + 1u);
            if (rc != GIT_OK) break;
        }
    }

    git_free(idx, (num_objs ? num_objs : 1u) * sizeof(pack_idx_t));
    return rc;
}

int git_build_packfile(const char *repo_dir, const uint8_t (*sha1s)[GIT_SHA1_LEN],
                       size_t n_objs, uint8_t **pack_out, size_t *pack_len) {
    if (!repo_dir || !pack_out || !pack_len) return GIT_ERR_INVAL;
    size_t cap = 64u + n_objs * 4096u;
    uint8_t *buf = (uint8_t *)git_alloc(cap);
    if (!buf) return GIT_ERR_NOMEM;

    memcpy(buf, "PACK", 4u);
    wr_be32(buf + 4u, 2u);
    wr_be32(buf + 8u, (uint32_t)n_objs);
    size_t pos = 12u;

    for (size_t i = 0; i < n_objs; i++) {
        int type = -1;
        uint8_t *data = NULL;
        size_t len = 0;
        int rc = git_read_loose_obj(repo_dir, sha1s[i], &type, &data, &len);
        if (rc != GIT_OK) { git_free(buf, cap); return rc; }

        size_t zcap = len + len / 8u + 64u;
        uint8_t *zbuf = (uint8_t *)git_alloc(zcap);
        if (!zbuf) { git_free(data, len + 1u); git_free(buf, cap); return GIT_ERR_NOMEM; }
        size_t zlen = 0;
        int zrc = zlib_deflate(data, len, zbuf, zcap, &zlen);
        git_free(data, len + 1u);
        if (zrc != ZLIB_OK) { git_free(zbuf, zcap); git_free(buf, cap); return GIT_ERR_CORRUPT; }

        while (pos + 16u + zlen + GIT_SHA1_LEN > cap) {
            size_t ncap = cap * 2u;
            uint8_t *nbuf = (uint8_t *)git_alloc(ncap);
            if (!nbuf) { git_free(zbuf, zcap); git_free(buf, cap); return GIT_ERR_NOMEM; }
            memcpy(nbuf, buf, pos);
            git_free(buf, cap);
            buf = nbuf;
            cap = ncap;
        }

        size_t sz = len;
        uint8_t first = (uint8_t)(((type & 7) << 4) | (sz & 0x0fu));
        sz >>= 4;
        if (sz > 0) first |= 0x80u;
        buf[pos++] = first;
        while (sz > 0) {
            uint8_t b = (uint8_t)(sz & 0x7fu);
            sz >>= 7;
            if (sz > 0) b |= 0x80u;
            buf[pos++] = b;
        }
        memcpy(buf + pos, zbuf, zlen);
        pos += zlen;
        git_free(zbuf, zcap);
    }

    sha1(buf, (uint32_t)pos, buf + pos);
    pos += GIT_SHA1_LEN;
    *pack_out = buf;
    *pack_len = pos;
    return GIT_OK;
}

#else
typedef int git_pack_unused_t;
#endif
