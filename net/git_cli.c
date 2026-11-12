/*
 * Git Smart HTTP(S) / SSH transport and `git` shell command.
 * Zero static writable storage.
 */

#include "net/git.h"

#if !defined(LUGALOS_HOST_TEST) && defined(CONFIG_GIT_CLIENT)

#include "net/http.h"
#include "net/ssh.h"
#include "kernel/crypto.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "fs/vfs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char *git_default_base(void) {
    return vfs_volume_mounted("nvme0") ? "/nvme0" : "/sd0";
}

static int parse_args(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            argv[argc++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

/* --- Dynamic growable byte buffer ------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} gbuf_t;

static int gbuf_append(gbuf_t *b, const uint8_t *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap * 2u : 4096u;
        while (ncap < b->len + n) ncap *= 2u;
        uint8_t *nd = (uint8_t *)git_alloc(ncap);
        if (!nd) return GIT_ERR_NOMEM;
        if (b->data) {
            memcpy(nd, b->data, b->len);
            git_free(b->data, b->cap);
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return GIT_OK;
}

static void gbuf_free(gbuf_t *b) {
    if (b->data) git_free(b->data, b->cap);
    b->data = NULL; b->len = 0; b->cap = 0;
}

static int http_collect_cb(const uint8_t *data, uint32_t len, void *ctx) {
    return gbuf_append((gbuf_t *)ctx, data, len) == GIT_OK ? 0 : -1;
}

/* --- pkt-line helpers ------------------------------------------------------- */

static int hex_d(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int pkt_write(gbuf_t *b, const char *s, size_t slen) {
    char hdr[4];
    size_t total = 4u + slen;
    static const char hx[] = "0123456789abcdef";
    hdr[0] = hx[(total >> 12) & 0xfu];
    hdr[1] = hx[(total >> 8) & 0xfu];
    hdr[2] = hx[(total >> 4) & 0xfu];
    hdr[3] = hx[total & 0xfu];
    if (gbuf_append(b, (const uint8_t *)hdr, 4u) != GIT_OK) return GIT_ERR_NOMEM;
    return gbuf_append(b, (const uint8_t *)s, slen);
}

static bool pkt_next(const uint8_t *buf, size_t len, size_t *pos,
                     const uint8_t **pay_out, size_t *pay_len) {
    if (*pos + 4u > len) return false;
    int d0 = hex_d((char)buf[*pos]);
    int d1 = hex_d((char)buf[*pos + 1u]);
    int d2 = hex_d((char)buf[*pos + 2u]);
    int d3 = hex_d((char)buf[*pos + 3u]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) return false;
    size_t plen = (size_t)((d0 << 12) | (d1 << 8) | (d2 << 4) | d3);
    *pos += 4u;
    if (plen == 0) {
        *pay_out = NULL;
        *pay_len = 0;
        return true;
    }
    if (plen < 4u || *pos + (plen - 4u) > len) return false;
    *pay_out = buf + *pos;
    *pay_len = plen - 4u;
    *pos += plen - 4u;
    return true;
}

static bool parse_ref_advertisement(const uint8_t *buf, size_t len,
                                    uint8_t head_sha1[GIT_SHA1_LEN],
                                    char branch_out[64]) {
    size_t pos = 0;
    bool got_head = false;
    ksnprintf(branch_out, 64u, "main");
    while (pos < len) {
        const uint8_t *pay = NULL;
        size_t plen = 0;
        if (!pkt_next(buf, len, &pos, &pay, &plen)) break;
        if (!pay) continue;
        if (plen > 0 && pay[0] == '#') continue;
        if (plen >= GIT_HEX_LEN + 5u && pay[GIT_HEX_LEN] == ' ') {
            const char *rname = (const char *)(pay + GIT_HEX_LEN + 1u);
            const uint8_t *nul = (const uint8_t *)memchr(rname, '\0', plen - GIT_HEX_LEN - 1u);
            size_t rlen = nul ? (size_t)(nul - (const uint8_t *)rname) : (plen - GIT_HEX_LEN - 1u);
            while (rlen > 0 && (rname[rlen - 1u] == '\n' || rname[rlen - 1u] == '\r')) rlen--;
            if ((rlen == 4u && memcmp(rname, "HEAD", 4u) == 0) ||
                (!got_head && rlen > 11u && memcmp(rname, "refs/heads/", 11u) == 0)) {
                if (git_hex_to_sha1((const char *)pay, head_sha1)) {
                    got_head = true;
                    if (rlen > 11u && memcmp(rname, "refs/heads/", 11u) == 0) {
                        size_t bl = rlen - 11u;
                        if (bl >= 64u) bl = 63u;
                        memcpy(branch_out, rname + 11u, bl);
                        branch_out[bl] = '\0';
                    }
                }
            }
        }
    }
    return got_head;
}

/* --- SSH URL parser: ssh://[user@]host[:port]/path -------------------------- */

static bool parse_ssh_url(const char *url, char *user, size_t user_sz,
                          char *host, size_t host_sz, uint16_t *port,
                          char *path, size_t path_sz) {
    if (strncmp(url, "ssh://", 6u) != 0) return false;
    const char *p = url + 6u;
    const char *slash = strchr(p, '/');
    if (!slash) return false;
    const char *at = strchr(p, '@');
    if (at && at < slash) {
        size_t ul = (size_t)(at - p);
        if (ul >= user_sz) ul = user_sz - 1u;
        memcpy(user, p, ul);
        user[ul] = '\0';
        p = at + 1;
    } else {
        ksnprintf(user, user_sz, "git");
    }
    const char *colon = strchr(p, ':');
    if (colon && colon < slash) {
        size_t hl = (size_t)(colon - p);
        if (hl >= host_sz) return false;
        memcpy(host, p, hl); host[hl] = '\0';
        uint32_t pv = 0;
        for (const char *s = colon + 1; s < slash; s++) {
            if (*s < '0' || *s > '9') return false;
            pv = pv * 10u + (uint32_t)(*s - '0');
        }
        *port = (uint16_t)(pv ? pv : SSH_DEFAULT_PORT);
    } else {
        size_t hl = (size_t)(slash - p);
        if (hl >= host_sz) return false;
        memcpy(host, p, hl); host[hl] = '\0';
        *port = SSH_DEFAULT_PORT;
    }
    ksnprintf(path, path_sz, "%s", slash);
    return true;
}

static bool git_ssh_hk_accept(void *ctx, const char *host, uint16_t port,
                              const char *ktype, const uint8_t *blob, size_t len) {
    (void)ctx; (void)host; (void)port; (void)ktype; (void)blob; (void)len;
    return true;
}

/* --- Smart HTTP / SSH fetch & clone ----------------------------------------- */

static int git_fetch_pack(const char *url, const char *repo_dir,
                          uint8_t head_sha1[GIT_SHA1_LEN], char branch[64]) {
    gbuf_t refs = {0}, resp = {0}, reqb = {0};
    int rc = GIT_ERR_NET;

    if (strncmp(url, "ssh://", 6u) == 0) {
        char user[64], host[128], path[192];
        uint16_t port = SSH_DEFAULT_PORT;
        if (!parse_ssh_url(url, user, sizeof(user), host, sizeof(host), &port, path, sizeof(path))) {
            return GIT_ERR_INVAL;
        }
        uint8_t seed[32];
        const char *kpath = vfs_volume_mounted("nvme0") ? "/nvme0/.ssh/id_ed25519" : "/sd0/.ssh/id_ed25519";
        bool have_key = (vfs_read(kpath, seed, sizeof(seed)) == (int)sizeof(seed));
        ssh_cfg_t cfg = {
            .host = host, .port = port, .user = user,
            .ed25519_seed = have_key ? seed : NULL,
            .timeout_ms = 20000u,
            .hostkey_cb = git_ssh_hk_accept,
        };
        ssh_session_t *s = NULL;
        char err[96];
        int src = ssh_connect_tcp(&s, &cfg, err, sizeof(err));
        crypto_wipe_bytes(seed, sizeof(seed));
        if (src != SSH_OK) { cprintf("git: ssh connect failed: %s\n", err); return GIT_ERR_NET; }
        char cmd[256];
        ksnprintf(cmd, sizeof(cmd), "git-upload-pack '%s'", path);
        if (ssh_open_exec(s, cmd) != SSH_OK) { ssh_close(s); return GIT_ERR_NET; }

        uint8_t tmp[512];
        uint64_t t0 = time_get_ms();
        while (time_get_ms() - t0 < 5000u) {
            bool is_err = false;
            int n = ssh_channel_read(s, tmp, sizeof(tmp), &is_err);
            if (n < 0) break;
            if (n > 0) {
                gbuf_append(&refs, tmp, (size_t)n);
                if (refs.len >= 4u && memcmp(refs.data + refs.len - 4u, "0000", 4u) == 0) break;
            } else sched_yield();
        }
        if (!parse_ref_advertisement(refs.data, refs.len, head_sha1, branch)) {
            ssh_close(s); gbuf_free(&refs); return GIT_ERR_PROTO;
        }
        char hex[GIT_HEX_LEN + 1u], want[96];
        git_sha1_to_hex(head_sha1, hex);
        int wl = ksnprintf(want, sizeof(want), "want %s\n", hex);
        pkt_write(&reqb, want, (size_t)wl);
        gbuf_append(&reqb, (const uint8_t *)"0000", 4u);
        pkt_write(&reqb, "done\n", 5u);
        size_t sent = 0;
        while (sent < reqb.len) {
            int n = ssh_channel_write(s, reqb.data + sent, (uint32_t)(reqb.len - sent));
            if (n < 0) break;
            if (n > 0) sent += (size_t)n; else sched_yield();
        }
        for (;;) {
            bool is_err = false;
            int n = ssh_channel_read(s, tmp, sizeof(tmp), &is_err);
            if (n == SSH_EOF || n < 0) break;
            if (n > 0) gbuf_append(&resp, tmp, (size_t)n);
            else if (console_interrupt_requested()) break;
            else sched_yield();
        }
        ssh_close(s);
    } else {
        char info_url[320], up_url[320], err[128] = {0};
        ksnprintf(info_url, sizeof(info_url), "%s/info/refs?service=git-upload-pack", url);
        ksnprintf(up_url, sizeof(up_url), "%s/git-upload-pack", url);
        http_req_t hget = { .url = info_url, .method = "GET", .timeout_ms = 20000u, .tls_insecure = true };
        int st = http_request(&hget, http_collect_cb, &refs, err, sizeof(err));
        if (st < 200 || st >= 300) {
            cprintf("git: HTTP GET %s failed (%d: %s)\n", info_url, st, err);
            gbuf_free(&refs);
            return GIT_ERR_NET;
        }
        if (!parse_ref_advertisement(refs.data, refs.len, head_sha1, branch)) {
            gbuf_free(&refs);
            return GIT_ERR_PROTO;
        }
        char hex[GIT_HEX_LEN + 1u], want[96];
        git_sha1_to_hex(head_sha1, hex);
        int wl = ksnprintf(want, sizeof(want), "want %s\n", hex);
        pkt_write(&reqb, want, (size_t)wl);
        gbuf_append(&reqb, (const uint8_t *)"0000", 4u);
        pkt_write(&reqb, "done\n", 5u);

        http_req_t hpost = {
            .url = up_url, .method = "POST",
            .content_type = "application/x-git-upload-pack-request",
            .body = (const char *)reqb.data, .body_len = (uint32_t)reqb.len,
            .timeout_ms = 30000u, .tls_insecure = true,
        };
        st = http_request(&hpost, http_collect_cb, &resp, err, sizeof(err));
        if (st < 200 || st >= 300) {
            cprintf("git: HTTP POST %s failed (%d: %s)\n", up_url, st, err);
            gbuf_free(&refs); gbuf_free(&reqb); gbuf_free(&resp);
            return GIT_ERR_NET;
        }
    }

    /* Locate "PACK" header after "0008NAK\n" */
    const uint8_t *pack = NULL;
    size_t pack_len = 0;
    for (size_t i = 0; i + 4u <= resp.len; i++) {
        if (memcmp(resp.data + i, "PACK", 4u) == 0) {
            pack = resp.data + i;
            pack_len = resp.len - i;
            break;
        }
    }
    if (pack && pack_len >= 32u) {
        rc = git_unpack_packfile(repo_dir, pack, pack_len);
    } else {
        rc = GIT_ERR_PROTO;
    }

    gbuf_free(&refs);
    gbuf_free(&reqb);
    gbuf_free(&resp);
    return rc;
}

/* --- `git` Shell Command ---------------------------------------------------- */

static void print_git_usage(void) {
    cprintf("Usage: git [-C dir] <cmd> [args]\n");
    cprintf("  init [dir] | clone <url> [dir] | status | diff | add <file>\n");
    cprintf("  commit -m <msg> | log [-n N] | checkout <branch|sha>\n");
    cprintf("  fetch | pull | push [url]\n");
}

static bool resolve_head_branch_sha1(const char *repo_dir, char branch_out[64],
                                     uint8_t sha1_out[GIT_SHA1_LEN]) {
    ksnprintf(branch_out, 64u, "main");
    char hp[256], htxt[128];
    ksnprintf(hp, sizeof(hp), "%s/.git/HEAD", repo_dir);
    int hn = vfs_read(hp, htxt, sizeof(htxt) - 1u);
    if (hn <= 0) return false;
    htxt[hn] = '\0';
    while (hn > 0 && (htxt[hn - 1] == '\n' || htxt[hn - 1] == '\r')) htxt[--hn] = '\0';
    if (strncmp(htxt, "ref: ", 5u) == 0) {
        const char *ref = htxt + 5u;
        const char *slash = NULL;
        for (const char *p = ref; *p; p++) { if (*p == '/') slash = p; }
        if (slash && slash[1]) ksnprintf(branch_out, 64u, "%s", slash + 1);
        char rp[256], rhex[64];
        ksnprintf(rp, sizeof(rp), "%s/.git/%s", repo_dir, ref);
        int rn = vfs_read(rp, rhex, sizeof(rhex) - 1u);
        return rn >= (int)GIT_HEX_LEN && git_hex_to_sha1(rhex, sha1_out);
    }
    return git_hex_to_sha1(htxt, sha1_out);
}

static void add_unique_obj(uint8_t (*objs)[GIT_SHA1_LEN], size_t *n_objs, size_t max_objs,
                           const uint8_t sha1[GIT_SHA1_LEN]) {
    for (size_t i = 0; i < *n_objs; i++) {
        if (memcmp(objs[i], sha1, GIT_SHA1_LEN) == 0) return;
    }
    if (*n_objs < max_objs) {
        memcpy(objs[(*n_objs)++], sha1, GIT_SHA1_LEN);
    }
}

static void collect_tree_objs(const char *repo_dir, const uint8_t tree_sha1[GIT_SHA1_LEN],
                              uint8_t (*objs)[GIT_SHA1_LEN], size_t *n_objs, size_t max_objs) {
    add_unique_obj(objs, n_objs, max_objs, tree_sha1);
    int type = -1;
    uint8_t *tbuf = NULL;
    size_t tlen = 0;
    if (git_read_loose_obj(repo_dir, tree_sha1, &type, &tbuf, &tlen) != GIT_OK || type != GIT_OBJ_TREE) {
        if (tbuf) git_free(tbuf, tlen + 1u);
        return;
    }
    size_t pos = 0;
    while (pos < tlen) {
        const char *sp = memchr(tbuf + pos, ' ', tlen - pos);
        if (!sp) break;
        bool is_dir = (sp - (const char *)(tbuf + pos) == 5 && memcmp(tbuf + pos, "40000", 5u) == 0);
        size_t name_off = (size_t)(sp - (const char *)tbuf) + 1u;
        const uint8_t *nul = memchr(tbuf + name_off, '\0', tlen - name_off);
        if (!nul || (size_t)(nul - tbuf) + 1u + GIT_SHA1_LEN > tlen) break;
        const uint8_t *esha1 = nul + 1u;
        if (is_dir) {
            collect_tree_objs(repo_dir, esha1, objs, n_objs, max_objs);
        } else {
            add_unique_obj(objs, n_objs, max_objs, esha1);
        }
        pos = (size_t)(esha1 - tbuf) + GIT_SHA1_LEN;
    }
    git_free(tbuf, tlen + 1u);
}

static void git_diff_cmd(const char *repo_dir) {
    git_index_entry_t *ents = NULL;
    size_t count = 0;
    if (git_index_read(repo_dir, &ents, &count) != GIT_OK) {
        cprintf("git diff: not a git repository (%s)\n", repo_dir);
        return;
    }
    size_t changed = 0;
    for (size_t i = 0; i < count; i++) {
        char fp[320];
        ksnprintf(fp, sizeof(fp), "%s/%s", repo_dir, ents[i].path);
        vfs_stat_t st;
        if (vfs_stat(fp, &st) != 0) {
            cprintf("deleted:  %s\n", ents[i].path);
            changed++;
            continue;
        }
        uint8_t *buf = git_alloc(st.size ? st.size : 1u);
        if (!buf) continue;
        int fd = vfs_open(fp, VFS_O_READ);
        int n = (fd >= 0 && st.size > 0) ? vfs_pread(fd, buf, st.size, 0) : 0;
        if (fd >= 0) vfs_close(fd);
        uint8_t wsha1[GIT_SHA1_LEN];
        git_hash_object(GIT_OBJ_BLOB, buf, n > 0 ? (size_t)n : 0u, wsha1);
        git_free(buf, st.size ? st.size : 1u);
        if (memcmp(wsha1, ents[i].sha1, GIT_SHA1_LEN) != 0) {
            cprintf("modified: %s (%uB -> %uB)\n", ents[i].path,
                    (unsigned)ents[i].size, (unsigned)st.size);
            changed++;
        }
    }
    if (changed == 0) cprintf("working tree clean\n");
    if (ents) git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
}

static int git_push_pack(const char *url, const char *repo_dir,
                         const char *branch, const uint8_t new_sha1[GIT_SHA1_LEN]) {
    uint8_t (*objs)[GIT_SHA1_LEN] = git_alloc(256u * GIT_SHA1_LEN);
    if (!objs) return GIT_ERR_NOMEM;
    size_t n_objs = 0;

    uint8_t cur[GIT_SHA1_LEN];
    memcpy(cur, new_sha1, GIT_SHA1_LEN);
    for (int depth = 0; depth < 32; depth++) {
        add_unique_obj(objs, &n_objs, 256u, cur);
        int type = -1;
        uint8_t *cbuf = NULL;
        size_t clen = 0;
        if (git_read_loose_obj(repo_dir, cur, &type, &cbuf, &clen) != GIT_OK || type != GIT_OBJ_COMMIT) {
            if (cbuf) git_free(cbuf, clen + 1u);
            break;
        }
        uint8_t tsha1[GIT_SHA1_LEN];
        if (clen >= 5u + GIT_HEX_LEN && memcmp(cbuf, "tree ", 5u) == 0 &&
            git_hex_to_sha1((const char *)(cbuf + 5u), tsha1)) {
            collect_tree_objs(repo_dir, tsha1, objs, &n_objs, 256u);
        }
        const char *par = strstr((const char *)cbuf, "\nparent ");
        bool has_p = par && git_hex_to_sha1(par + 8, cur);
        git_free(cbuf, clen + 1u);
        if (!has_p) break;
    }

    uint8_t *pack = NULL;
    size_t pack_len = 0;
    int rc = git_build_packfile(repo_dir, (const uint8_t (*)[GIT_SHA1_LEN])objs, n_objs, &pack, &pack_len);
    git_free(objs, 256u * GIT_SHA1_LEN);
    if (rc != GIT_OK) return rc;

    char old_hex[GIT_HEX_LEN + 1u];
    memset(old_hex, '0', GIT_HEX_LEN);
    old_hex[GIT_HEX_LEN] = '\0';
    char new_hex[GIT_HEX_LEN + 1u];
    git_sha1_to_hex(new_sha1, new_hex);

    char target_ref[96];
    ksnprintf(target_ref, sizeof(target_ref), "refs/heads/%s", branch);

    gbuf_t refs = {0}, reqb = {0}, resp = {0};
    char err[128] = {0};

    if (strncmp(url, "ssh://", 6u) == 0) {
        char user[64], host[128], rpath[192];
        uint16_t port = SSH_DEFAULT_PORT;
        if (!parse_ssh_url(url, user, sizeof(user), host, sizeof(host), &port, rpath, sizeof(rpath))) {
            git_free(pack, pack_len);
            return GIT_ERR_INVAL;
        }
        uint8_t seed[32];
        const char *kpath = vfs_volume_mounted("nvme0") ? "/nvme0/.ssh/id_ed25519" : "/sd0/.ssh/id_ed25519";
        bool have_key = (vfs_read(kpath, seed, sizeof(seed)) == (int)sizeof(seed));
        ssh_cfg_t cfg = {
            .host = host, .port = port, .user = user,
            .ed25519_seed = have_key ? seed : NULL,
            .timeout_ms = 20000u,
            .hostkey_cb = git_ssh_hk_accept,
        };
        ssh_session_t *s = NULL;
        int src = ssh_connect_tcp(&s, &cfg, err, sizeof(err));
        crypto_wipe_bytes(seed, sizeof(seed));
        if (src != SSH_OK) { git_free(pack, pack_len); return GIT_ERR_NET; }
        char cmd[256];
        ksnprintf(cmd, sizeof(cmd), "git-receive-pack '%s'", rpath);
        if (ssh_open_exec(s, cmd) != SSH_OK) { ssh_close(s); git_free(pack, pack_len); return GIT_ERR_NET; }
        uint8_t tmp[512];
        uint64_t t0 = time_get_ms();
        while (time_get_ms() - t0 < 5000u) {
            bool is_err = false;
            int n = ssh_channel_read(s, tmp, sizeof(tmp), &is_err);
            if (n < 0) break;
            if (n > 0) {
                gbuf_append(&refs, tmp, (size_t)n);
                if (refs.len >= 4u && memcmp(refs.data + refs.len - 4u, "0000", 4u) == 0) break;
            } else sched_yield();
        }
        if (refs.len > 0) {
            uint8_t rsha1[GIT_SHA1_LEN];
            char rbranch[64];
            if (parse_ref_advertisement(refs.data, refs.len, rsha1, rbranch) &&
                strcmp(rbranch, branch) == 0) {
                git_sha1_to_hex(rsha1, old_hex);
            }
        }
        char upd[192];
        int ulen = ksnprintf(upd, sizeof(upd), "%s %s %s", old_hex, new_hex, target_ref);
        size_t pay_len = (size_t)ulen + 1u + 14u;
        char hdr[5];
        ksnprintf(hdr, sizeof(hdr), "%04x", (unsigned)(pay_len + 4u));
        gbuf_append(&reqb, (const uint8_t *)hdr, 4u);
        gbuf_append(&reqb, (const uint8_t *)upd, (size_t)ulen + 1u);
        gbuf_append(&reqb, (const uint8_t *)"report-status\n", 14u);
        gbuf_append(&reqb, (const uint8_t *)"0000", 4u);
        gbuf_append(&reqb, pack, pack_len);
        (void)ssh_channel_write(s, reqb.data, (uint32_t)reqb.len);
        (void)ssh_channel_send_eof(s);
        t0 = time_get_ms();
        while (time_get_ms() - t0 < 5000u) {
            bool is_err = false;
            int n = ssh_channel_read(s, tmp, sizeof(tmp), &is_err);
            if (n < 0) break;
            if (n > 0) gbuf_append(&resp, tmp, (size_t)n);
            else sched_yield();
        }
        ssh_close(s);
        rc = GIT_OK;
    } else {
        char info_url[320], recv_url[320];
        ksnprintf(info_url, sizeof(info_url), "%s/info/refs?service=git-receive-pack", url);
        ksnprintf(recv_url, sizeof(recv_url), "%s/git-receive-pack", url);
        http_req_t hget = { .url = info_url, .method = "GET", .timeout_ms = 15000u, .tls_insecure = true };
        int st = http_request(&hget, http_collect_cb, &refs, err, sizeof(err));
        if (st < 200 || st >= 300) {
            cprintf("git push: GET %s failed (%d: %s)\n", info_url, st, err);
            git_free(pack, pack_len); gbuf_free(&refs);
            return GIT_ERR_NET;
        }
        uint8_t rsha1[GIT_SHA1_LEN];
        char rbranch[64];
        if (parse_ref_advertisement(refs.data, refs.len, rsha1, rbranch) &&
            strcmp(rbranch, branch) == 0) {
            git_sha1_to_hex(rsha1, old_hex);
        }
        char upd[192];
        int ulen = ksnprintf(upd, sizeof(upd), "%s %s %s", old_hex, new_hex, target_ref);
        size_t pay_len = (size_t)ulen + 1u + 14u;
        char hdr[5];
        ksnprintf(hdr, sizeof(hdr), "%04x", (unsigned)(pay_len + 4u));
        gbuf_append(&reqb, (const uint8_t *)hdr, 4u);
        gbuf_append(&reqb, (const uint8_t *)upd, (size_t)ulen + 1u);
        gbuf_append(&reqb, (const uint8_t *)"report-status\n", 14u);
        gbuf_append(&reqb, (const uint8_t *)"0000", 4u);
        gbuf_append(&reqb, pack, pack_len);

        http_req_t hpost = {
            .url = recv_url, .method = "POST",
            .content_type = "application/x-git-receive-pack-request",
            .body = (const char *)reqb.data, .body_len = (uint32_t)reqb.len,
            .timeout_ms = 30000u, .tls_insecure = true,
        };
        st = http_request(&hpost, http_collect_cb, &resp, err, sizeof(err));
        rc = (st >= 200 && st < 300) ? GIT_OK : GIT_ERR_NET;
    }

    git_free(pack, pack_len);
    gbuf_free(&refs);
    gbuf_free(&reqb);
    gbuf_free(&resp);
    return rc;
}

static void save_origin_url(const char *repo_dir, const char *url) {
    char p[320];
    ksnprintf(p, sizeof(p), "%s/.git/origin_url", repo_dir);
    int fd = vfs_open(p, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd >= 0) {
        vfs_pwrite(fd, url, (uint32_t)strlen(url), 0);
        vfs_close(fd);
    }
}

static bool load_origin_url(const char *repo_dir, char *url_out, size_t cap) {
    char p[320];
    ksnprintf(p, sizeof(p), "%s/.git/origin_url", repo_dir);
    int n = vfs_read(p, url_out, (uint32_t)(cap - 1u));
    if (n <= 0) return false;
    url_out[n] = '\0';
    while (n > 0 && (url_out[n - 1] == '\n' || url_out[n - 1] == '\r')) url_out[--n] = '\0';
    return n > 0;
}

void cmd_git(const char *args) {
    char argbuf[512];
    ksnprintf(argbuf, sizeof(argbuf), "%s", args ? args : "");
    char *argv[24];
    int argc = parse_args(argbuf, argv, 24);
    if (argc < 1) { print_git_usage(); return; }

    char repo_dir[192];
    ksnprintf(repo_dir, sizeof(repo_dir), "%s/repo", git_default_base());

    int idx = 0;
    if (idx + 1 < argc && strcmp(argv[idx], "-C") == 0) {
        ksnprintf(repo_dir, sizeof(repo_dir), "%s", argv[idx + 1]);
        idx += 2;
    }
    if (idx >= argc) { print_git_usage(); return; }
    const char *sub = argv[idx++];

    if (strcmp(sub, "init") == 0) {
        const char *dir = (idx < argc) ? argv[idx] : repo_dir;
        int rc = git_init(dir);
        if (rc == GIT_OK) cprintf("Initialized empty Git repository in %s/.git\n", dir);
        else cprintf("git init failed (%d)\n", rc);
    } else if (strcmp(sub, "add") == 0) {
        if (idx >= argc) { cprintf("Usage: git [-C dir] add <file>\n"); return; }
        int rc = git_add_file(repo_dir, argv[idx]);
        if (rc != GIT_OK) cprintf("git add failed (%d)\n", rc);
    } else if (strcmp(sub, "commit") == 0) {
        const char *msg = "commit";
        for (int i = idx; i < argc; i++) {
            if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) msg = argv[++i];
        }
        uint8_t csha1[GIT_SHA1_LEN];
        int rc = git_commit(repo_dir, "LugalOS <lugalos@local>", msg, csha1);
        if (rc == GIT_OK) {
            char hex[GIT_HEX_LEN + 1u];
            git_sha1_to_hex(csha1, hex);
            cprintf("[commit %.7s] %s\n", hex, msg);
        } else {
            cprintf("git commit failed (%d)\n", rc);
        }
    } else if (strcmp(sub, "status") == 0) {
        git_index_entry_t *ents = NULL;
        size_t count = 0;
        if (git_index_read(repo_dir, &ents, &count) != GIT_OK) {
            cprintf("git status: not a git repository (%s)\n", repo_dir);
            return;
        }
        cprintf("Tracked files (%u):\n", (unsigned)count);
        for (size_t i = 0; i < count; i++) {
            cprintf("  %s (%u bytes)\n", ents[i].path, (unsigned)ents[i].size);
        }
        if (ents) git_free(ents, (count ? count : 1u) * sizeof(git_index_entry_t));
    } else if (strcmp(sub, "log") == 0) {
        char hp[256], htxt[128];
        ksnprintf(hp, sizeof(hp), "%s/.git/HEAD", repo_dir);
        int hn = vfs_read(hp, htxt, sizeof(htxt) - 1u);
        if (hn <= 0) { cprintf("git log: no HEAD\n"); return; }
        htxt[hn] = '\0';
        while (hn > 0 && (htxt[hn - 1] == '\n' || htxt[hn - 1] == '\r')) htxt[--hn] = '\0';
        uint8_t cur[GIT_SHA1_LEN];
        if (strncmp(htxt, "ref: ", 5u) == 0) {
            char rp[256], rhex[64];
            ksnprintf(rp, sizeof(rp), "%s/.git/%s", repo_dir, htxt + 5u);
            int rn = vfs_read(rp, rhex, sizeof(rhex) - 1u);
            if (rn < (int)GIT_HEX_LEN || !git_hex_to_sha1(rhex, cur)) {
                cprintf("git log: no commits yet\n");
                return;
            }
        } else if (!git_hex_to_sha1(htxt, cur)) {
            cprintf("git log: invalid HEAD\n");
            return;
        }
        for (int n = 0; n < 10; n++) {
            int type = -1;
            uint8_t *cbuf = NULL;
            size_t clen = 0;
            if (git_read_loose_obj(repo_dir, cur, &type, &cbuf, &clen) != GIT_OK || type != GIT_OBJ_COMMIT) break;
            char hex[GIT_HEX_LEN + 1u];
            git_sha1_to_hex(cur, hex);
            const char *msg = strstr((const char *)cbuf, "\n\n");
            cprintf("commit %s\n  %s\n", hex, msg ? msg + 2 : "");
            const char *par = strstr((const char *)cbuf, "\nparent ");
            bool has_p = par && git_hex_to_sha1(par + 8, cur);
            git_free(cbuf, clen + 1u);
            if (!has_p) break;
        }
    } else if (strcmp(sub, "checkout") == 0) {
        if (idx >= argc) { cprintf("Usage: git [-C dir] checkout <branch|sha>\n"); return; }
        int rc = git_checkout_ref(repo_dir, argv[idx]);
        if (rc != GIT_OK) cprintf("git checkout failed (%d)\n", rc);
    } else if (strcmp(sub, "clone") == 0) {
        if (idx >= argc) { cprintf("Usage: git clone <url> [dir]\n"); return; }
        const char *url = argv[idx++];
        const char *dir = (idx < argc) ? argv[idx] : repo_dir;
        if (git_init(dir) != GIT_OK) { cprintf("git clone: init failed\n"); return; }
        save_origin_url(dir, url);
        uint8_t head_sha1[GIT_SHA1_LEN];
        char branch[64];
        int rc = git_fetch_pack(url, dir, head_sha1, branch);
        if (rc != GIT_OK) { cprintf("git clone failed (%d)\n", rc); return; }
        char hex[GIT_HEX_LEN + 2u], rp[256], hp[256], htxt[128];
        git_sha1_to_hex(head_sha1, hex);
        hex[GIT_HEX_LEN] = '\n'; hex[GIT_HEX_LEN + 1u] = '\0';
        ksnprintf(rp, sizeof(rp), "%s/.git/refs/heads/%s", dir, branch);
        int fd = vfs_open(rp, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (fd >= 0) { vfs_pwrite(fd, hex, GIT_HEX_LEN + 1u, 0); vfs_close(fd); }
        ksnprintf(hp, sizeof(hp), "%s/.git/HEAD", dir);
        int hl = ksnprintf(htxt, sizeof(htxt), "ref: refs/heads/%s\n", branch);
        fd = vfs_open(hp, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (fd >= 0 && hl > 0) { vfs_pwrite(fd, htxt, (uint32_t)hl, 0); vfs_close(fd); }
        rc = git_checkout_ref(dir, branch);
        if (rc == GIT_OK) cprintf("Cloned into %s (branch %s)\n", dir, branch);
        else cprintf("git clone checkout failed (%d)\n", rc);
    } else if (strcmp(sub, "fetch") == 0 || strcmp(sub, "pull") == 0) {
        char url[256];
        if (!load_origin_url(repo_dir, url, sizeof(url))) {
            cprintf("git %s: no origin URL configured\n", sub);
            return;
        }
        uint8_t head_sha1[GIT_SHA1_LEN];
        char branch[64];
        int rc = git_fetch_pack(url, repo_dir, head_sha1, branch);
        if (rc != GIT_OK) { cprintf("git %s failed (%d)\n", sub, rc); return; }
        char hex[GIT_HEX_LEN + 2u], rp[256];
        git_sha1_to_hex(head_sha1, hex);
        hex[GIT_HEX_LEN] = '\n'; hex[GIT_HEX_LEN + 1u] = '\0';
        ksnprintf(rp, sizeof(rp), "%s/.git/refs/remotes/origin/%s", repo_dir, branch);
        int fd = vfs_open(rp, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (fd >= 0) { vfs_pwrite(fd, hex, GIT_HEX_LEN + 1u, 0); vfs_close(fd); }
        if (strcmp(sub, "pull") == 0) {
            ksnprintf(rp, sizeof(rp), "%s/.git/refs/heads/%s", repo_dir, branch);
            fd = vfs_open(rp, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
            if (fd >= 0) { vfs_pwrite(fd, hex, GIT_HEX_LEN + 1u, 0); vfs_close(fd); }
            rc = git_checkout_ref(repo_dir, branch);
        }
        if (rc == GIT_OK) cprintf("git %s: updated %s (%.7s)\n", sub, branch, hex);
        else cprintf("git %s failed (%d)\n", sub, rc);
    } else if (strcmp(sub, "diff") == 0) {
        git_diff_cmd(repo_dir);
    } else if (strcmp(sub, "push") == 0) {
        char url[256];
        if (idx < argc) {
            ksnprintf(url, sizeof(url), "%s", argv[idx]);
        } else if (!load_origin_url(repo_dir, url, sizeof(url))) {
            cprintf("git push: no origin URL configured\n");
            return;
        }
        char branch[64];
        uint8_t head_sha1[GIT_SHA1_LEN];
        if (!resolve_head_branch_sha1(repo_dir, branch, head_sha1)) {
            cprintf("git push: no commits to push\n");
            return;
        }
        int rc = git_push_pack(url, repo_dir, branch, head_sha1);
        if (rc == GIT_OK) cprintf("Pushed %s to %s\n", branch, url);
        else cprintf("git push failed (%d)\n", rc);
    } else {
        print_git_usage();
    }
}

#else
typedef int git_cli_unused_t;
#endif
