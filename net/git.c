/*
 * Freestanding Git object store, index v2, tree/commit builder, and checkout.
 * Zero static writable storage.
 */

#include "net/git.h"

#if defined(LUGALOS_HOST_TEST) || defined(CONFIG_GIT_CLIENT)

#include "kernel/crypto.h"
#include "kernel/zlib.h"

#include <string.h>

#if defined(LUGALOS_HOST_TEST)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

void *git_alloc(size_t bytes) {
    return calloc(1u, bytes ? bytes : 1u);
}

void git_free(void *ptr, size_t bytes) {
    (void)bytes;
    free(ptr);
}

static int git_mkdir_one(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return GIT_OK;
    return GIT_ERR_IO;
}

static int git_stat_file(const char *path, uint32_t *size_out, bool *is_dir_out) {
    struct stat st;
    if (stat(path, &st) != 0) return GIT_ERR_NOTFOUND;
    if (size_out) *size_out = (uint32_t)st.st_size;
    if (is_dir_out) *is_dir_out = S_ISDIR(st.st_mode);
    return GIT_OK;
}

static int git_read_file(const char *path, uint8_t **buf_out, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return GIT_ERR_NOTFOUND;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return GIT_ERR_IO; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return GIT_ERR_IO; }
    rewind(f);
    uint8_t *buf = (uint8_t *)git_alloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return GIT_ERR_NOMEM; }
    if (sz > 0 && fread(buf, 1u, (size_t)sz, f) != (size_t)sz) {
        git_free(buf, (size_t)sz + 1u);
        fclose(f);
        return GIT_ERR_IO;
    }
    buf[sz] = '\0';
    fclose(f);
    *buf_out = buf;
    *len_out = (size_t)sz;
    return GIT_OK;
}

static int git_write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return GIT_ERR_IO;
    if (len > 0 && fwrite(buf, 1u, len, f) != len) {
        fclose(f);
        return GIT_ERR_IO;
    }
    fclose(f);
    return GIT_OK;
}

#else

#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "fs/vfs.h"

void *git_alloc(size_t bytes) {
    uint32_t pages = (uint32_t)(((bytes ? bytes : 1u) + 4095u) / 4096u);
    void *p = palloc_pages_bulk(pages);
    if (p) memset(p, 0, (size_t)pages * 4096u);
    return p;
}

void git_free(void *ptr, size_t bytes) {
    if (!ptr) return;
    uint32_t pages = (uint32_t)(((bytes ? bytes : 1u) + 4095u) / 4096u);
    palloc_free(ptr, pages);
}

static int git_mkdir_one(const char *path) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) == 0 && st.is_dir) return GIT_OK;
    return vfs_mkdir(path) == 0 ? GIT_OK : GIT_ERR_IO;
}

static int git_stat_file(const char *path, uint32_t *size_out, bool *is_dir_out) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0) return GIT_ERR_NOTFOUND;
    if (size_out) *size_out = st.size;
    if (is_dir_out) *is_dir_out = st.is_dir != 0;
    return GIT_OK;
}

static int git_read_file(const char *path, uint8_t **buf_out, size_t *len_out) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) < 0 || st.is_dir) return GIT_ERR_NOTFOUND;
    uint8_t *buf = (uint8_t *)git_alloc((size_t)st.size + 1u);
    if (!buf) return GIT_ERR_NOMEM;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) { git_free(buf, (size_t)st.size + 1u); return GIT_ERR_IO; }
    uint32_t off = 0;
    while (off < st.size) {
        int n = vfs_pread(fd, buf + off, st.size - off, off);
        if (n <= 0) { vfs_close(fd); git_free(buf, (size_t)st.size + 1u); return GIT_ERR_IO; }
        off += (uint32_t)n;
    }
    vfs_close(fd);
    buf[st.size] = '\0';
    *buf_out = buf;
    *len_out = st.size;
    return GIT_OK;
}

static int git_write_file(const char *path, const uint8_t *buf, size_t len) {
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return GIT_ERR_IO;
    uint32_t off = 0;
    while (off < (uint32_t)len) {
        int n = vfs_pwrite(fd, buf + off, (uint32_t)len - off, off);
        if (n <= 0) { vfs_close(fd); return GIT_ERR_IO; }
        off += (uint32_t)n;
    }
    vfs_close(fd);
    return GIT_OK;
}

#endif

static int git_mkdir_p(const char *path) {
    char tmp[320];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return GIT_ERR_INVAL;
    memcpy(tmp, path, len + 1u);
    for (size_t i = 1u; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\0') {
            char sav = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && git_mkdir_one(tmp) != GIT_OK) {
                /* On LugalOS, volume root like "/sd0" may fail mkdir if already mounted; ignore if stat succeeds */
                bool is_dir = false;
                if (git_stat_file(tmp, NULL, &is_dir) != GIT_OK || !is_dir) {
                    return GIT_ERR_IO;
                }
            }
            tmp[i] = sav;
        }
    }
    return GIT_OK;
}

static void git_fmt_path(char *out, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + 1u + lb + 1u > cap) { out[0] = '\0'; return; }
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1u, b, lb + 1u);
}

/* --- Hex & SHA-1 helpers ---------------------------------------------------- */

void git_sha1_to_hex(const uint8_t sha1[GIT_SHA1_LEN], char hex_out[GIT_HEX_LEN + 1u]) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < GIT_SHA1_LEN; i++) {
        hex_out[i * 2u]      = hx[sha1[i] >> 4];
        hex_out[i * 2u + 1u] = hx[sha1[i] & 0x0fu];
    }
    hex_out[GIT_HEX_LEN] = '\0';
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool git_hex_to_sha1(const char *hex, uint8_t sha1_out[GIT_SHA1_LEN]) {
    if (!hex) return false;
    for (size_t i = 0; i < GIT_SHA1_LEN; i++) {
        int hi = hex_val(hex[i * 2u]);
        int lo = hex_val(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return false;
        sha1_out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static const char *obj_type_name(int type) {
    switch (type) {
    case GIT_OBJ_COMMIT: return "commit";
    case GIT_OBJ_TREE:   return "tree";
    case GIT_OBJ_BLOB:   return "blob";
    case GIT_OBJ_TAG:    return "tag";
    default:             return NULL;
    }
}

static int parse_obj_type(const char *name) {
    if (strcmp(name, "commit") == 0) return GIT_OBJ_COMMIT;
    if (strcmp(name, "tree") == 0)   return GIT_OBJ_TREE;
    if (strcmp(name, "blob") == 0)   return GIT_OBJ_BLOB;
    if (strcmp(name, "tag") == 0)    return GIT_OBJ_TAG;
    return -1;
}

static size_t format_obj_header(char *hdr, size_t cap, int type, size_t len) {
    const char *tn = obj_type_name(type);
    if (!tn) return 0;
    size_t tl = strlen(tn);
    if (tl + 24u > cap) return 0;
    memcpy(hdr, tn, tl);
    hdr[tl++] = ' ';
    char tmp[20];
    size_t n = 0;
    size_t v = len;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    for (size_t i = 0; i < n; i++) hdr[tl++] = tmp[n - 1u - i];
    hdr[tl++] = '\0';
    return tl;
}

void git_hash_object(int type, const void *data, size_t len,
                     uint8_t sha1_out[GIT_SHA1_LEN]) {
    char hdr[40];
    size_t hlen = format_obj_header(hdr, sizeof(hdr), type, len);
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, hdr, (uint32_t)hlen);
    if (len > 0) sha1_update(&ctx, data, (uint32_t)len);
    sha1_final(&ctx, sha1_out);
}

/* --- Repository init & loose objects ---------------------------------------- */

int git_init(const char *repo_dir) {
    char p[320];
    if (git_mkdir_p(repo_dir) != GIT_OK) return GIT_ERR_IO;
    git_fmt_path(p, sizeof(p), repo_dir, ".git");
    if (git_mkdir_p(p) != GIT_OK) return GIT_ERR_IO;
    git_fmt_path(p, sizeof(p), repo_dir, ".git/objects");
    if (git_mkdir_p(p) != GIT_OK) return GIT_ERR_IO;
    git_fmt_path(p, sizeof(p), repo_dir, ".git/refs/heads");
    if (git_mkdir_p(p) != GIT_OK) return GIT_ERR_IO;
    git_fmt_path(p, sizeof(p), repo_dir, ".git/refs/remotes/origin");
    if (git_mkdir_p(p) != GIT_OK) return GIT_ERR_IO;

    git_fmt_path(p, sizeof(p), repo_dir, ".git/HEAD");
    const char *head = "ref: refs/heads/main\n";
    return git_write_file(p, (const uint8_t *)head, strlen(head));
}

static void obj_path(char *out, size_t cap, const char *repo_dir, const char *hex) {
    char dir[320];
    size_t rlen = strlen(repo_dir);
    if (rlen + 16u + GIT_HEX_LEN >= cap) { out[0] = '\0'; return; }
    memcpy(dir, repo_dir, rlen);
    memcpy(dir + rlen, "/.git/objects/", 15u);
    dir[rlen + 14u] = hex[0];
    dir[rlen + 15u] = hex[1];
    dir[rlen + 16u] = '/';
    memcpy(dir + rlen + 17u, hex + 2u, GIT_HEX_LEN - 2u + 1u);
    memcpy(out, dir, strlen(dir) + 1u);
}

int git_write_loose_obj(const char *repo_dir, int type,
                        const void *data, size_t len,
                        uint8_t sha1_out[GIT_SHA1_LEN]) {
    uint8_t sha1[GIT_SHA1_LEN];
    git_hash_object(type, data, len, sha1);
    if (sha1_out) memcpy(sha1_out, sha1, GIT_SHA1_LEN);

    char hex[GIT_HEX_LEN + 1u];
    git_sha1_to_hex(sha1, hex);

    char p[320];
    obj_path(p, sizeof(p), repo_dir, hex);
    if (git_stat_file(p, NULL, NULL) == GIT_OK) return GIT_OK;

    char dir[320];
    size_t rlen = strlen(repo_dir);
    memcpy(dir, repo_dir, rlen);
    memcpy(dir + rlen, "/.git/objects/", 14u);
    dir[rlen + 14u] = hex[0];
    dir[rlen + 15u] = hex[1];
    dir[rlen + 16u] = '\0';
    if (git_mkdir_p(dir) != GIT_OK) return GIT_ERR_IO;

    char hdr[40];
    size_t hlen = format_obj_header(hdr, sizeof(hdr), type, len);
    size_t raw_len = hlen + len;
    uint8_t *raw = (uint8_t *)git_alloc(raw_len);
    if (!raw) return GIT_ERR_NOMEM;
    memcpy(raw, hdr, hlen);
    if (len > 0) memcpy(raw + hlen, data, len);

    size_t zcap = raw_len + raw_len / 8u + 64u;
    uint8_t *zbuf = (uint8_t *)git_alloc(zcap);
    if (!zbuf) { git_free(raw, raw_len); return GIT_ERR_NOMEM; }

    size_t zlen = 0;
    int zrc = zlib_deflate(raw, raw_len, zbuf, zcap, &zlen);
    git_free(raw, raw_len);
    if (zrc != ZLIB_OK) { git_free(zbuf, zcap); return GIT_ERR_IO; }

    int wrc = git_write_file(p, zbuf, zlen);
    git_free(zbuf, zcap);
    return wrc;
}

int git_read_loose_obj(const char *repo_dir, const uint8_t sha1[GIT_SHA1_LEN],
                       int *type_out, uint8_t **data_out, size_t *len_out) {
    char hex[GIT_HEX_LEN + 1u];
    git_sha1_to_hex(sha1, hex);
    char p[320];
    obj_path(p, sizeof(p), repo_dir, hex);

    uint8_t *zbuf = NULL;
    size_t zlen = 0;
    int rc = git_read_file(p, &zbuf, &zlen);
    if (rc != GIT_OK) return rc;

    /* Try progressively larger output buffers if needed. */
    size_t cap = zlen * 4u + 1024u;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        raw = (uint8_t *)git_alloc(cap);
        if (!raw) { git_free(zbuf, zlen + 1u); return GIT_ERR_NOMEM; }
        int zrc = zlib_inflate(zbuf, zlen, raw, cap, &raw_len, NULL);
        if (zrc == ZLIB_OK) break;
        git_free(raw, cap);
        raw = NULL;
        if (zrc != ZLIB_ERR_SPACE) { git_free(zbuf, zlen + 1u); return GIT_ERR_CORRUPT; }
        cap *= 2u;
    }
    git_free(zbuf, zlen + 1u);
    if (!raw) return GIT_ERR_NOMEM;

    const char *sp = (const char *)memchr(raw, ' ', raw_len);
    const char *nul = (const char *)memchr(raw, '\0', raw_len);
    if (!sp || !nul || sp > nul) { git_free(raw, cap); return GIT_ERR_CORRUPT; }

    char tname[16];
    size_t tnlen = (size_t)(sp - (const char *)raw);
    if (tnlen >= sizeof(tname)) { git_free(raw, cap); return GIT_ERR_CORRUPT; }
    memcpy(tname, raw, tnlen);
    tname[tnlen] = '\0';
    int type = parse_obj_type(tname);
    if (type < 0) { git_free(raw, cap); return GIT_ERR_CORRUPT; }

    size_t hdr_len = (size_t)(nul - (const char *)raw) + 1u;
    size_t pay_len = raw_len - hdr_len;
    uint8_t *pay = (uint8_t *)git_alloc(pay_len + 1u);
    if (!pay) { git_free(raw, cap); return GIT_ERR_NOMEM; }
    if (pay_len > 0) memcpy(pay, raw + hdr_len, pay_len);
    pay[pay_len] = '\0';
    git_free(raw, cap);

    if (type_out) *type_out = type;
    *data_out = pay;
    *len_out = pay_len;
    return GIT_OK;
}

/* --- Index v2 (.git/index) -------------------------------------------------- */

static inline uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

int git_index_read(const char *repo_dir, git_index_entry_t **entries_out, size_t *count_out) {
    *entries_out = NULL;
    *count_out = 0;
    char p[320];
    git_fmt_path(p, sizeof(p), repo_dir, ".git/index");
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = git_read_file(p, &buf, &len);
    if (rc == GIT_ERR_NOTFOUND) return GIT_OK;
    if (rc != GIT_OK) return rc;
    if (len < 32u || memcmp(buf, "DIRC", 4u) != 0 || rd_be32(buf + 4u) != 2u) {
        git_free(buf, len + 1u);
        return GIT_ERR_CORRUPT;
    }

    uint32_t count = rd_be32(buf + 8u);
    git_index_entry_t *ents = (git_index_entry_t *)git_alloc((count ? count : 1u) * sizeof(git_index_entry_t));
    if (!ents) { git_free(buf, len + 1u); return GIT_ERR_NOMEM; }

    size_t pos = 12u;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 62u > len - 20u) {
            git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
            git_free(buf, len + 1u);
            return GIT_ERR_CORRUPT;
        }
        ents[i].mode = rd_be32(buf + pos + 24u);
        ents[i].size = rd_be32(buf + pos + 36u);
        memcpy(ents[i].sha1, buf + pos + 40u, GIT_SHA1_LEN);
        const char *name = (const char *)(buf + pos + 62u);
        size_t max_n = (len - 20u) - (pos + 62u);
        const char *nul = (const char *)memchr(name, '\0', max_n);
        if (!nul) {
            git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
            git_free(buf, len + 1u);
            return GIT_ERR_CORRUPT;
        }
        size_t nlen = (size_t)(nul - name);
        if (nlen >= sizeof(ents[i].path)) nlen = sizeof(ents[i].path) - 1u;
        memcpy(ents[i].path, name, nlen);
        ents[i].path[nlen] = '\0';
        size_t rec = 62u + (size_t)(nul - name) + 1u;
        pos += (rec + 7u) & ~7u;
    }
    git_free(buf, len + 1u);
    *entries_out = ents;
    *count_out = count;
    return GIT_OK;
}

int git_index_write(const char *repo_dir, const git_index_entry_t *entries, size_t count) {
    size_t cap = 12u + count * 320u + 20u;
    uint8_t *buf = (uint8_t *)git_alloc(cap);
    if (!buf) return GIT_ERR_NOMEM;
    memcpy(buf, "DIRC", 4u);
    wr_be32(buf + 4u, 2u);
    wr_be32(buf + 8u, (uint32_t)count);
    size_t pos = 12u;
    for (size_t i = 0; i < count; i++) {
        size_t nlen = strlen(entries[i].path);
        size_t rec = (62u + nlen + 1u + 7u) & ~7u;
        if (pos + rec + 20u > cap) { git_free(buf, cap); return GIT_ERR_NOMEM; }
        memset(buf + pos, 0, rec);
        wr_be32(buf + pos + 24u, entries[i].mode ? entries[i].mode : 0100644u);
        wr_be32(buf + pos + 36u, entries[i].size);
        memcpy(buf + pos + 40u, entries[i].sha1, GIT_SHA1_LEN);
        uint16_t flags = (uint16_t)(nlen < 0x0fffu ? nlen : 0x0fffu);
        buf[pos + 60u] = (uint8_t)(flags >> 8);
        buf[pos + 61u] = (uint8_t)flags;
        memcpy(buf + pos + 62u, entries[i].path, nlen + 1u);
        pos += rec;
    }
    uint8_t trailer[GIT_SHA1_LEN];
    sha1(buf, (uint32_t)pos, trailer);
    memcpy(buf + pos, trailer, GIT_SHA1_LEN);
    pos += GIT_SHA1_LEN;

    char p[320];
    git_fmt_path(p, sizeof(p), repo_dir, ".git/index");
    int rc = git_write_file(p, buf, pos);
    git_free(buf, cap);
    return rc;
}

/* --- High-level porcelain: add, commit, checkout ---------------------------- */

int git_add_file(const char *repo_dir, const char *rel_path) {
    char full[320];
    git_fmt_path(full, sizeof(full), repo_dir, rel_path);
    uint8_t *data = NULL;
    size_t len = 0;
    int rc = git_read_file(full, &data, &len);
    if (rc != GIT_OK) return rc;

    uint8_t sha1[GIT_SHA1_LEN];
    rc = git_write_loose_obj(repo_dir, GIT_OBJ_BLOB, data, len, sha1);
    git_free(data, len + 1u);
    if (rc != GIT_OK) return rc;

    git_index_entry_t *ents = NULL;
    size_t count = 0;
    rc = git_index_read(repo_dir, &ents, &count);
    if (rc != GIT_OK) return rc;

    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(ents[i].path, rel_path) == 0) {
            ents[i].mode = 0100644u;
            ents[i].size = (uint32_t)len;
            memcpy(ents[i].sha1, sha1, GIT_SHA1_LEN);
            found = true;
            break;
        }
    }
    if (!found) {
        git_index_entry_t *nents = (git_index_entry_t *)git_alloc((count + 1u) * sizeof(git_index_entry_t));
        if (!nents) {
            git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
            return GIT_ERR_NOMEM;
        }
        if (count > 0) memcpy(nents, ents, count * sizeof(git_index_entry_t));
        git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
        ents = nents;
        ents[count].mode = 0100644u;
        ents[count].size = (uint32_t)len;
        memcpy(ents[count].sha1, sha1, GIT_SHA1_LEN);
        size_t pl = strlen(rel_path);
        if (pl >= sizeof(ents[count].path)) pl = sizeof(ents[count].path) - 1u;
        memcpy(ents[count].path, rel_path, pl);
        ents[count].path[pl] = '\0';
        count++;
        /* Sort entries lexicographically by path (Git tree requirement). */
        for (size_t i = 0; i < count; i++) {
            for (size_t j = i + 1u; j < count; j++) {
                if (strcmp(ents[i].path, ents[j].path) > 0) {
                    git_index_entry_t tmp = ents[i];
                    ents[i] = ents[j];
                    ents[j] = tmp;
                }
            }
        }
    }

    rc = git_index_write(repo_dir, ents, count);
    git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
    return rc;
}

static int build_tree_recursive(const char *repo_dir, const git_index_entry_t *ents,
                                size_t count, const char *prefix,
                                uint8_t tree_sha1_out[GIT_SHA1_LEN]) {
    size_t plen = strlen(prefix);
    size_t cap = count * 300u + 64u;
    uint8_t *tbuf = (uint8_t *)git_alloc(cap);
    if (!tbuf) return GIT_ERR_NOMEM;
    size_t tpos = 0;

    size_t i = 0;
    while (i < count) {
        if (plen > 0 && strncmp(ents[i].path, prefix, plen) != 0) { i++; continue; }
        const char *sub = ents[i].path + plen;
        const char *slash = strchr(sub, '/');
        if (!slash) {
            /* File entry */
            char hdr[300];
            int hlen = 0;
            const char *mstr = (ents[i].mode == 0100755u) ? "100755 " : "100644 ";
            memcpy(hdr, mstr, 7u);
            size_t sl = strlen(sub);
            memcpy(hdr + 7u, sub, sl);
            hdr[7u + sl] = '\0';
            hlen = (int)(7u + sl + 1u);
            if (tpos + (size_t)hlen + GIT_SHA1_LEN > cap) { git_free(tbuf, cap); return GIT_ERR_NOMEM; }
            memcpy(tbuf + tpos, hdr, (size_t)hlen); tpos += (size_t)hlen;
            memcpy(tbuf + tpos, ents[i].sha1, GIT_SHA1_LEN); tpos += GIT_SHA1_LEN;
            i++;
        } else {
            /* Subtree: group consecutive entries sharing this directory component */
            size_t dlen = (size_t)(slash - sub);
            char new_pref[260];
            if (plen + dlen + 2u >= sizeof(new_pref)) { git_free(tbuf, cap); return GIT_ERR_INVAL; }
            if (plen > 0) memcpy(new_pref, prefix, plen);
            memcpy(new_pref + plen, sub, dlen);
            new_pref[plen + dlen] = '/';
            new_pref[plen + dlen + 1u] = '\0';

            uint8_t sub_sha1[GIT_SHA1_LEN];
            int rc = build_tree_recursive(repo_dir, ents, count, new_pref, sub_sha1);
            if (rc != GIT_OK) { git_free(tbuf, cap); return rc; }

            if (tpos + 4u + dlen + 1u + GIT_SHA1_LEN > cap) { git_free(tbuf, cap); return GIT_ERR_NOMEM; }
            memcpy(tbuf + tpos, "40000 ", 6u); tpos += 6u;
            memcpy(tbuf + tpos, sub, dlen);    tpos += dlen;
            tbuf[tpos++] = '\0';
            memcpy(tbuf + tpos, sub_sha1, GIT_SHA1_LEN); tpos += GIT_SHA1_LEN;

            while (i < count && strncmp(ents[i].path, new_pref, strlen(new_pref)) == 0) i++;
        }
    }

    int rc = git_write_loose_obj(repo_dir, GIT_OBJ_TREE, tbuf, tpos, tree_sha1_out);
    git_free(tbuf, cap);
    return rc;
}

static bool read_head_ref(const char *repo_dir, char *ref_path_out, size_t ref_cap,
                          uint8_t sha1_out[GIT_SHA1_LEN], bool *has_commit) {
    *has_commit = false;
    ref_path_out[0] = '\0';
    char p[320];
    git_fmt_path(p, sizeof(p), repo_dir, ".git/HEAD");
    uint8_t *hbuf = NULL;
    size_t hlen = 0;
    if (git_read_file(p, &hbuf, &hlen) != GIT_OK) return false;
    char *s = (char *)hbuf;
    while (hlen > 0 && (s[hlen - 1u] == '\n' || s[hlen - 1u] == '\r')) s[--hlen] = '\0';
    if (strncmp(s, "ref: ", 5u) == 0) {
        size_t rl = strlen(s + 5u);
        if (rl >= ref_cap) { git_free(hbuf, hlen + 1u); return false; }
        memcpy(ref_path_out, s + 5u, rl + 1u);
        git_free(hbuf, hlen + 1u);
        char rp[320];
        char rsub[280];
        git_fmt_path(rsub, sizeof(rsub), ".git", ref_path_out);
        git_fmt_path(rp, sizeof(rp), repo_dir, rsub);
        uint8_t *rbuf = NULL;
        size_t rlen = 0;
        if (git_read_file(rp, &rbuf, &rlen) == GIT_OK) {
            if (rlen >= GIT_HEX_LEN && git_hex_to_sha1((const char *)rbuf, sha1_out)) {
                *has_commit = true;
            }
            git_free(rbuf, rlen + 1u);
        }
        return true;
    }
    if (hlen >= GIT_HEX_LEN && git_hex_to_sha1(s, sha1_out)) {
        *has_commit = true;
    }
    git_free(hbuf, hlen + 1u);
    return true;
}

int git_commit(const char *repo_dir, const char *author, const char *message,
               uint8_t commit_sha1_out[GIT_SHA1_LEN]) {
    git_index_entry_t *ents = NULL;
    size_t count = 0;
    int rc = git_index_read(repo_dir, &ents, &count);
    if (rc != GIT_OK) return rc;

    uint8_t tree_sha1[GIT_SHA1_LEN];
    rc = build_tree_recursive(repo_dir, ents, count, "", tree_sha1);
    git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
    if (rc != GIT_OK) return rc;

    char tree_hex[GIT_HEX_LEN + 1u];
    git_sha1_to_hex(tree_sha1, tree_hex);

    char ref_rel[192];
    uint8_t parent_sha1[GIT_SHA1_LEN];
    bool has_parent = false;
    (void)read_head_ref(repo_dir, ref_rel, sizeof(ref_rel), parent_sha1, &has_parent);

    char cbuf[1024];
    size_t pos = 0;
    const char *au = (author && author[0]) ? author : "LugalOS <lugalos@local>";
    const char *ms = (message && message[0]) ? message : "commit";
    memcpy(cbuf + pos, "tree ", 5u); pos += 5u;
    memcpy(cbuf + pos, tree_hex, GIT_HEX_LEN); pos += GIT_HEX_LEN;
    cbuf[pos++] = '\n';
    if (has_parent) {
        char phex[GIT_HEX_LEN + 1u];
        git_sha1_to_hex(parent_sha1, phex);
        memcpy(cbuf + pos, "parent ", 7u); pos += 7u;
        memcpy(cbuf + pos, phex, GIT_HEX_LEN); pos += GIT_HEX_LEN;
        cbuf[pos++] = '\n';
    }
    const char *ts = " 0 +0000\n";
    size_t aul = strlen(au);
    if (pos + 24u + aul * 2u + strlen(ms) + 8u > sizeof(cbuf)) return GIT_ERR_NOMEM;
    memcpy(cbuf + pos, "author ", 7u); pos += 7u;
    memcpy(cbuf + pos, au, aul); pos += aul;
    memcpy(cbuf + pos, ts, 9u); pos += 9u;
    memcpy(cbuf + pos, "committer ", 10u); pos += 10u;
    memcpy(cbuf + pos, au, aul); pos += aul;
    memcpy(cbuf + pos, ts, 9u); pos += 9u;
    cbuf[pos++] = '\n';
    size_t ml = strlen(ms);
    memcpy(cbuf + pos, ms, ml); pos += ml;
    if (ml == 0 || ms[ml - 1u] != '\n') cbuf[pos++] = '\n';

    uint8_t csha1[GIT_SHA1_LEN];
    rc = git_write_loose_obj(repo_dir, GIT_OBJ_COMMIT, cbuf, pos, csha1);
    if (rc != GIT_OK) return rc;
    if (commit_sha1_out) memcpy(commit_sha1_out, csha1, GIT_SHA1_LEN);

    char chex[GIT_HEX_LEN + 2u];
    git_sha1_to_hex(csha1, chex);
    chex[GIT_HEX_LEN] = '\n';
    chex[GIT_HEX_LEN + 1u] = '\0';

    char rpath[320];
    if (ref_rel[0]) {
        char rsub[220];
        git_fmt_path(rsub, sizeof(rsub), ".git", ref_rel);
        git_fmt_path(rpath, sizeof(rpath), repo_dir, rsub);
    } else {
        git_fmt_path(rpath, sizeof(rpath), repo_dir, ".git/HEAD");
    }
    return git_write_file(rpath, (const uint8_t *)chex, GIT_HEX_LEN + 1u);
}

static int checkout_tree_rec(const char *repo_dir, const uint8_t tree_sha1[GIT_SHA1_LEN],
                             const char *sub_dir, git_index_entry_t **ents,
                             size_t *count, size_t *cap) {
    int type = -1;
    uint8_t *tbuf = NULL;
    size_t tlen = 0;
    int rc = git_read_loose_obj(repo_dir, tree_sha1, &type, &tbuf, &tlen);
    if (rc != GIT_OK) return rc;
    if (type != GIT_OBJ_TREE) { git_free(tbuf, tlen + 1u); return GIT_ERR_CORRUPT; }

    size_t pos = 0;
    while (pos < tlen) {
        const uint8_t *sp = (const uint8_t *)memchr(tbuf + pos, ' ', tlen - pos);
        if (!sp) { git_free(tbuf, tlen + 1u); return GIT_ERR_CORRUPT; }
        bool is_dir = (sp - (tbuf + pos) == 5 && memcmp(tbuf + pos, "40000", 5u) == 0);
        const char *name = (const char *)(sp + 1u);
        const uint8_t *nul = (const uint8_t *)memchr(name, '\0', tlen - (size_t)(name - (const char *)tbuf));
        if (!nul || (size_t)(nul - tbuf) + 1u + GIT_SHA1_LEN > tlen) {
            git_free(tbuf, tlen + 1u);
            return GIT_ERR_CORRUPT;
        }
        const uint8_t *esha1 = nul + 1u;
        char rel[256];
        if (sub_dir[0]) git_fmt_path(rel, sizeof(rel), sub_dir, name);
        else {
            size_t nl = strlen(name);
            if (nl >= sizeof(rel)) nl = sizeof(rel) - 1u;
            memcpy(rel, name, nl);
            rel[nl] = '\0';
        }
        char full[320];
        git_fmt_path(full, sizeof(full), repo_dir, rel);

        if (is_dir) {
            if (git_mkdir_p(full) != GIT_OK) { git_free(tbuf, tlen + 1u); return GIT_ERR_IO; }
            rc = checkout_tree_rec(repo_dir, esha1, rel, ents, count, cap);
            if (rc != GIT_OK) { git_free(tbuf, tlen + 1u); return rc; }
        } else {
            int btype = -1;
            uint8_t *bdata = NULL;
            size_t blen = 0;
            rc = git_read_loose_obj(repo_dir, esha1, &btype, &bdata, &blen);
            if (rc != GIT_OK || btype != GIT_OBJ_BLOB) {
                if (bdata) git_free(bdata, blen + 1u);
                git_free(tbuf, tlen + 1u);
                return rc != GIT_OK ? rc : GIT_ERR_CORRUPT;
            }
            rc = git_write_file(full, bdata, blen);
            git_free(bdata, blen + 1u);
            if (rc != GIT_OK) { git_free(tbuf, tlen + 1u); return rc; }

            if (*count == *cap) {
                size_t ncap = (*cap == 0) ? 16u : (*cap * 2u);
                git_index_entry_t *ne = (git_index_entry_t *)git_alloc(ncap * sizeof(git_index_entry_t));
                if (!ne) { git_free(tbuf, tlen + 1u); return GIT_ERR_NOMEM; }
                if (*count > 0) memcpy(ne, *ents, *count * sizeof(git_index_entry_t));
                if (*ents) git_free(*ents, *cap * sizeof(git_index_entry_t));
                *ents = ne;
                *cap = ncap;
            }
            git_index_entry_t *e = &(*ents)[(*count)++];
            e->mode = 0100644u;
            e->size = (uint32_t)blen;
            memcpy(e->sha1, esha1, GIT_SHA1_LEN);
            memcpy(e->path, rel, strlen(rel) + 1u);
        }
        pos = (size_t)(esha1 - tbuf) + GIT_SHA1_LEN;
    }

    git_free(tbuf, tlen + 1u);
    return GIT_OK;
}

int git_checkout_tree(const char *repo_dir, const uint8_t tree_sha1[GIT_SHA1_LEN]) {
    git_index_entry_t *ents = NULL;
    size_t count = 0, cap = 0;
    int rc = checkout_tree_rec(repo_dir, tree_sha1, "", &ents, &count, &cap);
    if (rc == GIT_OK) rc = git_index_write(repo_dir, ents, count);
    if (ents) git_free(ents, cap * sizeof(git_index_entry_t));
    return rc;
}

int git_checkout_ref(const char *repo_dir, const char *ref_or_hex) {
    uint8_t csha1[GIT_SHA1_LEN];
    if (!git_hex_to_sha1(ref_or_hex, csha1)) {
        char rp[320], sub[256];
        git_fmt_path(sub, sizeof(sub), ".git/refs/heads", ref_or_hex);
        git_fmt_path(rp, sizeof(rp), repo_dir, sub);
        uint8_t *rbuf = NULL;
        size_t rlen = 0;
        if (git_read_file(rp, &rbuf, &rlen) != GIT_OK) return GIT_ERR_NOTFOUND;
        bool ok = (rlen >= GIT_HEX_LEN && git_hex_to_sha1((const char *)rbuf, csha1));
        git_free(rbuf, rlen + 1u);
        if (!ok) return GIT_ERR_CORRUPT;
        char hp[320], htxt[256];
        git_fmt_path(hp, sizeof(hp), repo_dir, ".git/HEAD");
        size_t rl = strlen(ref_or_hex);
        if (22u + rl >= sizeof(htxt)) return GIT_ERR_INVAL;
        memcpy(htxt, "ref: refs/heads/", 16u);
        memcpy(htxt + 16u, ref_or_hex, rl);
        htxt[16u + rl] = '\n';
        (void)git_write_file(hp, (const uint8_t *)htxt, 17u + rl);
    }

    int type = -1;
    uint8_t *cbuf = NULL;
    size_t clen = 0;
    int rc = git_read_loose_obj(repo_dir, csha1, &type, &cbuf, &clen);
    if (rc != GIT_OK) return rc;
    if (type != GIT_OBJ_COMMIT || clen < 5u + GIT_HEX_LEN || memcmp(cbuf, "tree ", 5u) != 0) {
        git_free(cbuf, clen + 1u);
        return GIT_ERR_CORRUPT;
    }
    uint8_t tsha1[GIT_SHA1_LEN];
    bool tok = git_hex_to_sha1((const char *)(cbuf + 5u), tsha1);
    git_free(cbuf, clen + 1u);
    if (!tok) return GIT_ERR_CORRUPT;
    return git_checkout_tree(repo_dir, tsha1);
}

#else
typedef int git_unused_t;
#endif
