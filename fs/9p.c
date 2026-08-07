#include "fs/9p.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include <string.h>

/* --- Bounds-checked wire cursors (closes B11, plan/completed/2026-08-07_review_
 * and_remediation.md) ---
 *
 * Every write goes through wcur_*(), every read through rcur_*(); both track
 * an explicit end pointer and set `overflow`/return false the instant a
 * field would cross it, instead of trusting a length that came off the
 * wire. Once `overflow` is set, every subsequent call on that cursor is a
 * silent no-op, so a caller only has to check it once at the end rather
 * than after every single field. */

typedef struct { uint8_t *p; uint8_t *end; bool overflow; } wcur_t;
typedef struct { const uint8_t *p; const uint8_t *end; bool overflow; } rcur_t;

static void wcur_u8(wcur_t *c, uint8_t v) {
    if (c->overflow || c->p + 1 > c->end) { c->overflow = true; return; }
    *c->p++ = v;
}

static void wcur_u16(wcur_t *c, uint16_t v) {
    if (c->overflow || c->p + 2 > c->end) { c->overflow = true; return; }
    c->p[0] = (uint8_t)(v & 0xFF);
    c->p[1] = (uint8_t)((v >> 8) & 0xFF);
    c->p += 2;
}

static void wcur_u32(wcur_t *c, uint32_t v) {
    if (c->overflow || c->p + 4 > c->end) { c->overflow = true; return; }
    c->p[0] = (uint8_t)(v & 0xFF);
    c->p[1] = (uint8_t)((v >> 8) & 0xFF);
    c->p[2] = (uint8_t)((v >> 16) & 0xFF);
    c->p[3] = (uint8_t)((v >> 24) & 0xFF);
    c->p += 4;
}

static void wcur_u64(wcur_t *c, uint64_t v) {
    wcur_u32(c, (uint32_t)(v & 0xFFFFFFFFu));
    wcur_u32(c, (uint32_t)(v >> 32));
}

static void wcur_bytes(wcur_t *c, const uint8_t *src, uint32_t n) {
    if (n == 0) return;
    if (c->overflow || !src || c->p + n > c->end) { c->overflow = true; return; }
    memcpy(c->p, src, n);
    c->p += n;
}

static void wcur_str(wcur_t *c, const char *s) {
    uint16_t len = s ? (uint16_t)strlen(s) : 0;
    wcur_u16(c, len);
    if (len > 0) wcur_bytes(c, (const uint8_t *)s, len);
}

static void wcur_qid(wcur_t *c, const p9_qid_t *q) {
    wcur_u8(c, q->type);
    wcur_u32(c, q->vers);
    wcur_u64(c, q->path);
}

static bool rcur_u8(rcur_t *c, uint8_t *out) {
    if (c->overflow || c->p + 1 > c->end) { c->overflow = true; return false; }
    *out = *c->p++;
    return true;
}

static bool rcur_u16(rcur_t *c, uint16_t *out) {
    if (c->overflow || c->p + 2 > c->end) { c->overflow = true; return false; }
    *out = (uint16_t)(c->p[0] | ((uint16_t)c->p[1] << 8));
    c->p += 2;
    return true;
}

static bool rcur_u32(rcur_t *c, uint32_t *out) {
    if (c->overflow || c->p + 4 > c->end) { c->overflow = true; return false; }
    *out = (uint32_t)c->p[0] | ((uint32_t)c->p[1] << 8) | ((uint32_t)c->p[2] << 16) | ((uint32_t)c->p[3] << 24);
    c->p += 4;
    return true;
}

static bool rcur_u64(rcur_t *c, uint64_t *out) {
    uint32_t lo, hi;
    if (!rcur_u32(c, &lo) || !rcur_u32(c, &hi)) return false;
    *out = ((uint64_t)hi << 32) | lo;
    return true;
}

/* Reads a length-prefixed wire string into a fixed caller buffer. Always
 * bounds-checks the declared length against what's actually left in the
 * frame *before* touching it (the exact defect B11 called out: "count is
 * never validated against remaining length"). Truncates into dst rather
 * than overflowing it, but never reads past the frame to do so. Always
 * NUL-terminates dst on success. */
static bool rcur_str(rcur_t *c, char *dst, uint32_t dst_size) {
    uint16_t len;
    if (!rcur_u16(c, &len)) return false;
    if (c->overflow || c->p + len > c->end) { c->overflow = true; return false; }
    uint32_t copy = len;
    if (dst && dst_size > 0) {
        if (copy > dst_size - 1) copy = dst_size - 1;
        memcpy(dst, c->p, copy);
        dst[copy] = '\0';
    }
    c->p += len;
    return true;
}

static bool rcur_qid(rcur_t *c, p9_qid_t *q) {
    uint8_t type;
    uint32_t vers;
    uint64_t path;
    if (!rcur_u8(c, &type)) return false;
    if (!rcur_u32(c, &vers)) return false;
    if (!rcur_u64(c, &path)) return false;
    q->type = type;
    q->vers = vers;
    q->path = path;
    return true;
}

/* Validates `count` against what's actually left in the frame and hands
 * back a pointer directly into the caller's buffer (no copy) -- this is
 * the read-side counterpart of the exact B11 defect: the old code set
 * `msg->data = p` and trusted `count` unconditionally. */
static bool rcur_data(rcur_t *c, const uint8_t **out, uint32_t count) {
    if (c->overflow || c->p + count > c->end) { c->overflow = true; *out = NULL; return false; }
    *out = c->p;
    c->p += count;
    return true;
}

/* --- Path helpers --- */

/* This kernel's freestanding libc/include/string.h has no strrchr(). */
static char *p9_find_last_slash(const char *s) {
    const char *last = NULL;
    for (const char *p = s; *p; p++) {
        if (*p == '/') last = p;
    }
    return (char *)last;
}

/* Joins `base` (an absolute VFS path) with a single walk/create component,
 * handling ".." (parent directory, clamped at "/") the way 9P's Twalk
 * expects. `out` and `base` may alias. */
static void p9_path_join(char *out, uint32_t out_size, const char *base, const char *comp) {
    if (strcmp(comp, "..") == 0) {
        char tmp[128];
        strncpy(tmp, base, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *slash = p9_find_last_slash(tmp);
        if (slash && slash != tmp) *slash = '\0';
        else { tmp[0] = '/'; tmp[1] = '\0'; }
        strncpy(out, tmp, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    if (strcmp(base, "/") == 0) {
        ksnprintf(out, out_size, "/%s", comp);
    } else {
        char tmp[128];
        strncpy(tmp, base, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        ksnprintf(out, out_size, "%s/%s", tmp, comp);
    }
}

/* Stable-ish 64-bit id for a resolved absolute path, used as qid.path.
 * FNV-1a; collisions are a cosmetic issue (a client might conflate two
 * files' cache identity), not a memory-safety one, so this is fine for a
 * loopback/single-peer server. */
static uint64_t p9_path_hash(const char *path) {
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
        h ^= *s;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Packs one 9P "stat" structure (the wire format shared by Rstat and each
 * entry of a directory's Tread stream) into `out`, including its own
 * self-referential leading size field. Returns the total bytes written
 * (>= 2) or -1 if it doesn't fit in out_max. */
static int p9_pack_stat(uint8_t *out, uint32_t out_max, const char *name, uint64_t length, bool is_dir, p9_qid_t qid) {
    wcur_t c = { .p = out, .end = out + out_max, .overflow = false };
    uint8_t *size_field = c.p;
    wcur_u16(&c, 0);   // placeholder, patched below
    wcur_u16(&c, 0);   // type (kernel-use only)
    wcur_u32(&c, 0);   // dev (kernel-use only)
    wcur_qid(&c, &qid);
    wcur_u32(&c, is_dir ? (P9_DMDIR | 0755u) : 0644u);
    wcur_u32(&c, 0);   // atime -- no wall-clock epoch time available on these targets
    wcur_u32(&c, 0);   // mtime
    wcur_u64(&c, is_dir ? 0 : length);
    wcur_str(&c, name);
    wcur_str(&c, ""); // uid
    wcur_str(&c, ""); // gid
    wcur_str(&c, ""); // muid
    if (c.overflow) return -1;
    uint32_t total = (uint32_t)(c.p - out);
    uint16_t inner_size = (uint16_t)(total - 2);
    size_field[0] = (uint8_t)(inner_size & 0xFF);
    size_field[1] = (uint8_t)((inner_size >> 8) & 0xFF);
    return (int)total;
}

/* --- Fid table (A2, plan/phase5_distributed_design.md) ---
 * Real state per fid, wired to the VFS handle API from A1, replacing the
 * single 2KB global echo buffer this file used to have. Bounded and
 * flat -- P9_MAX_FIDS is small on purpose (this server has one peer at a
 * time today; a real multi-client story is Track A's later milestones),
 * so linear search is fine. */
typedef struct {
    bool in_use;
    uint32_t fid_value;
    char path[128];             // resolved absolute VFS path
    bool is_dir;
    bool is_open;
    int vfs_fd;                 // valid iff is_open && !is_dir
    uint32_t dir_read_index;    // next vfs_readdir() index for a directory Tread
    bool remove_on_clunk;       // ORCLOSE
    p9_qid_t qid;
} p9_fid_entry_t;

static p9_fid_entry_t g_fid_table[P9_MAX_FIDS];

static p9_fid_entry_t *p9_fid_lookup(uint32_t fid) {
    for (int i = 0; i < P9_MAX_FIDS; i++) {
        if (g_fid_table[i].in_use && g_fid_table[i].fid_value == fid) return &g_fid_table[i];
    }
    return NULL;
}

static void p9_fid_release(p9_fid_entry_t *e) {
    if (!e) return;
    if (e->is_open && !e->is_dir && e->vfs_fd >= 0) vfs_close(e->vfs_fd);
    memset(e, 0, sizeof(*e));
}

static p9_fid_entry_t *p9_fid_alloc(uint32_t fid) {
    if (p9_fid_lookup(fid)) return NULL; // client protocol error: fid already in use
    for (int i = 0; i < P9_MAX_FIDS; i++) {
        if (!g_fid_table[i].in_use) {
            memset(&g_fid_table[i], 0, sizeof(g_fid_table[i]));
            g_fid_table[i].in_use = true;
            g_fid_table[i].fid_value = fid;
            g_fid_table[i].vfs_fd = -1;
            return &g_fid_table[i];
        }
    }
    return NULL; // table full
}

static void p9_fid_reset_all(void) {
    for (int i = 0; i < P9_MAX_FIDS; i++) p9_fid_release(&g_fid_table[i]);
}

/* --- Message handlers --- */

static void p9_handle_tattach(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *e = p9_fid_alloc(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "attach: fid table full or fid in use"; return; }

    char path[128];
    if (req->aname[0] == '\0' || strcmp(req->aname, "/") == 0) {
        strncpy(path, "/", sizeof(path) - 1);
    } else if (req->aname[0] == '/') {
        strncpy(path, req->aname, sizeof(path) - 1);
    } else {
        path[0] = '/';
        strncpy(path + 1, req->aname, sizeof(path) - 2);
    }
    path[sizeof(path) - 1] = '\0';

    vfs_stat_t st;
    bool exists = (vfs_stat(path, &st) == 0);
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    /* An aname that doesn't resolve to a real VFS path (e.g. a bare export
     * name a client made up) is still treated as an attachable directory
     * root rather than rejected outright -- matches this server's historic
     * leniency (the old code accepted any aname) and costs nothing, since
     * every operation under it will simply fail with Rerror once it tries
     * to touch a file that isn't there. */
    e->is_dir = exists ? (st.is_dir != 0) : true;
    e->qid.type = e->is_dir ? P9_QTDIR : P9_QTFILE;
    e->qid.vers = 0;
    e->qid.path = p9_path_hash(e->path);

    resp->type = P9_RATTACH;
    resp->qid = e->qid;
}

static void p9_handle_twalk(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *src = p9_fid_lookup(req->fid);
    if (!src) { resp->type = P9_RERROR; resp->ename = "walk: unknown fid"; return; }

    if (req->nwname == 0) {
        // Clone idiom: newfid refers to the same file as fid, no walk performed.
        if (req->newfid != req->fid && p9_fid_lookup(req->newfid)) {
            resp->type = P9_RERROR; resp->ename = "walk: newfid already in use"; return;
        }
        p9_fid_entry_t *dst = (req->newfid == req->fid) ? src : p9_fid_alloc(req->newfid);
        if (!dst) { resp->type = P9_RERROR; resp->ename = "walk: fid table full"; return; }
        if (dst != src) {
            strncpy(dst->path, src->path, sizeof(dst->path));
            dst->is_dir = src->is_dir;
            dst->qid = src->qid;
        }
        resp->type = P9_RWALK;
        resp->nwqid = 0;
        return;
    }

    char cur[128];
    strncpy(cur, src->path, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    p9_qid_t qids[P9_MAX_WALK_ELEM];
    uint16_t nwalked = 0;
    bool final_is_dir = src->is_dir;

    for (uint16_t i = 0; i < req->nwname; i++) {
        char next[128];
        p9_path_join(next, sizeof(next), cur, req->wname[i]);
        vfs_stat_t st;
        if (vfs_stat(next, &st) != 0) {
            if (i == 0) { resp->type = P9_RERROR; resp->ename = "walk: no such file or directory"; return; }
            break; // partial walk: stop here, report what succeeded
        }
        strncpy(cur, next, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';
        qids[nwalked].type = st.is_dir ? P9_QTDIR : P9_QTFILE;
        qids[nwalked].vers = 0;
        qids[nwalked].path = p9_path_hash(cur);
        final_is_dir = st.is_dir != 0;
        nwalked++;
    }

    if (req->newfid != req->fid && p9_fid_lookup(req->newfid)) {
        resp->type = P9_RERROR; resp->ename = "walk: newfid already in use"; return;
    }
    p9_fid_entry_t *dst = (req->newfid == req->fid) ? src : p9_fid_alloc(req->newfid);
    if (!dst) { resp->type = P9_RERROR; resp->ename = "walk: fid table full"; return; }

    strncpy(dst->path, cur, sizeof(dst->path) - 1);
    dst->path[sizeof(dst->path) - 1] = '\0';
    dst->is_dir = final_is_dir;
    dst->qid = (nwalked > 0) ? qids[nwalked - 1] : src->qid;

    resp->type = P9_RWALK;
    resp->nwqid = nwalked;
    memcpy(resp->wqid, qids, nwalked * sizeof(p9_qid_t));
}

static void p9_handle_topen(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "open: unknown fid"; return; }
    if (e->is_open) { resp->type = P9_RERROR; resp->ename = "open: fid already open"; return; }

    if (!e->is_dir) {
        int flags;
        uint8_t access = req->mode & 0x03;
        if (access == P9_OWRITE) flags = VFS_O_WRITE;
        else if (access == P9_ORDWR || access == P9_OEXEC) flags = VFS_O_READ | VFS_O_WRITE;
        else flags = VFS_O_READ;
        if (req->mode & P9_OTRUNC) flags |= VFS_O_TRUNC;

        int fd = vfs_open(e->path, flags);
        if (fd < 0) { resp->type = P9_RERROR; resp->ename = "open: cannot open file"; return; }
        e->vfs_fd = fd;
    } else {
        e->dir_read_index = 0; // directories are served straight from vfs_readdir(), not a vfs handle
    }
    e->is_open = true;
    e->remove_on_clunk = (req->mode & P9_ORCLOSE) != 0;

    resp->type = P9_ROPEN;
    resp->qid = e->qid;
}

static void p9_handle_tcreate(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "create: unknown fid"; return; }
    if (!e->is_dir) { resp->type = P9_RERROR; resp->ename = "create: fid is not a directory"; return; }
    if (e->is_open) { resp->type = P9_RERROR; resp->ename = "create: fid already open"; return; }

    char newpath[128];
    p9_path_join(newpath, sizeof(newpath), e->path, req->name);
    bool want_dir = (req->perm & P9_DMDIR) != 0;

    int fd = -1;
    if (want_dir) {
        if (vfs_mkdir(newpath) != 0) { resp->type = P9_RERROR; resp->ename = "create: mkdir failed"; return; }
    } else {
        fd = vfs_open(newpath, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
        if (fd < 0) { resp->type = P9_RERROR; resp->ename = "create: cannot create file"; return; }
    }

    strncpy(e->path, newpath, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    e->is_dir = want_dir;
    e->is_open = true;
    e->vfs_fd = fd;
    e->dir_read_index = 0;
    e->qid.type = want_dir ? P9_QTDIR : P9_QTFILE;
    e->qid.vers = 0;
    e->qid.path = p9_path_hash(e->path);
    e->remove_on_clunk = (req->mode & P9_ORCLOSE) != 0;

    resp->type = P9_RCREATE;
    resp->qid = e->qid;
}

/* Fills `out` with as many complete packed-stat entries (see p9_pack_stat)
 * as fit within `cap` bytes, continuing from the fid's own dir_read_index
 * -- a directory Tread's `offset` is only used to detect "start over"
 * (offset 0); otherwise this simplifies the real 9P convention that
 * directory-read offsets are an opaque, server-assigned cursor rather than
 * a byte-random-access position (see the A2 completion notes for the
 * rationale). */
static int p9_read_dir_stream(p9_fid_entry_t *e, uint64_t offset, uint32_t cap, uint8_t *out) {
    if (offset == 0) e->dir_read_index = 0;

    int dfd = vfs_open(e->path, VFS_O_READ);
    if (dfd < 0) return 0;

    uint32_t used = 0;
    for (;;) {
        char name[P9_MAX_NAME_LEN];
        vfs_stat_t st;
        if (vfs_readdir(dfd, e->dir_read_index, name, sizeof(name), &st) != 0) break;

        char childpath[128];
        p9_path_join(childpath, sizeof(childpath), e->path, name);
        p9_qid_t q = { .type = st.is_dir ? P9_QTDIR : P9_QTFILE, .vers = 0, .path = p9_path_hash(childpath) };

        uint8_t entrybuf[96];
        int elen = p9_pack_stat(entrybuf, sizeof(entrybuf), name, st.size, st.is_dir != 0, q);
        if (elen < 0) break;
        if (used + (uint32_t)elen > cap) break; // doesn't fit in this reply; client will Tread again

        memcpy(out + used, entrybuf, (uint32_t)elen);
        used += (uint32_t)elen;
        e->dir_read_index++;
    }
    vfs_close(dfd);
    return (int)used;
}

static void p9_handle_tread(const p9_msg_t *req, p9_msg_t *resp, uint8_t *databuf, uint32_t databuf_max) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e || !e->is_open) { resp->type = P9_RERROR; resp->ename = "read: fid not open"; return; }

    uint32_t want = req->count;
    if (want > databuf_max) want = databuf_max;

    int n = e->is_dir ? p9_read_dir_stream(e, req->offset, want, databuf)
                       : vfs_pread(e->vfs_fd, databuf, want, req->offset);
    if (n < 0) { resp->type = P9_RERROR; resp->ename = "read: I/O error"; return; }

    resp->type = P9_RREAD;
    resp->count = (uint32_t)n;
    resp->data = databuf;
}

static void p9_handle_twrite(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e || !e->is_open || e->is_dir) { resp->type = P9_RERROR; resp->ename = "write: fid not open for writing"; return; }

    int n = vfs_pwrite(e->vfs_fd, req->data, req->count, req->offset);
    if (n < 0) { resp->type = P9_RERROR; resp->ename = "write: I/O error"; return; }

    resp->type = P9_RWRITE;
    resp->count = (uint32_t)n;
}

static void p9_handle_tstat(const p9_msg_t *req, p9_msg_t *resp, uint8_t *databuf, uint32_t databuf_max) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "stat: unknown fid"; return; }

    vfs_stat_t st;
    uint64_t size = 0;
    bool is_dir = e->is_dir;
    if (vfs_stat(e->path, &st) == 0) { size = st.size; is_dir = st.is_dir != 0; }

    const char *base = p9_find_last_slash(e->path);
    base = base ? base + 1 : e->path;
    if (*base == '\0') base = "/";

    int n = p9_pack_stat(databuf, databuf_max, base, size, is_dir, e->qid);
    if (n < 0) { resp->type = P9_RERROR; resp->ename = "stat: does not fit in reply buffer"; return; }

    resp->type = P9_RSTAT;
    resp->data = databuf;
    resp->count = (uint32_t)n;
}

static void p9_handle_tclunk(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (e) {
        if (e->remove_on_clunk) {
            if (e->is_dir) (void)vfs_rmdir(e->path);
            else (void)vfs_remove(e->path);
        }
        p9_fid_release(e);
    }
    resp->type = P9_RCLUNK;
}

static void p9_handle_tremove(const p9_msg_t *req, p9_msg_t *resp) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "remove: unknown fid"; return; }

    int rc = e->is_dir ? vfs_rmdir(e->path) : vfs_remove(e->path);
    p9_fid_release(e); // per spec, the fid is clunked whether or not the remove succeeds
    if (rc == 0) {
        resp->type = P9_RREMOVE;
    } else {
        resp->type = P9_RERROR;
        resp->ename = "remove: failed";
    }
}

/* --- Public API --- */

void p9_init(void) {
    p9_fid_reset_all();
    printk("[9P2000] Protocol Serialization Engine Initialized.\n");
}

int p9_serialize(const p9_msg_t *msg, uint8_t *buf, uint32_t buf_size) {
    if (!msg || !buf || buf_size < 7) return -1;

    wcur_t c = { .p = buf + 4, .end = buf + buf_size, .overflow = false };
    wcur_u8(&c, msg->type);
    wcur_u16(&c, msg->tag);

    switch (msg->type) {
        case P9_TVERSION:
        case P9_RVERSION:
            wcur_u32(&c, msg->msize);
            wcur_str(&c, "9P2000");
            break;
        case P9_TATTACH:
            wcur_u32(&c, msg->fid);
            wcur_u32(&c, P9_NOFID); // afid: this server never uses auth
            wcur_str(&c, msg->uname);
            wcur_str(&c, msg->aname);
            break;
        case P9_RATTACH:
            wcur_qid(&c, &msg->qid);
            break;
        case P9_RERROR:
            wcur_str(&c, msg->ename ? msg->ename : "Unknown error");
            break;
        case P9_TWALK: {
            wcur_u32(&c, msg->fid);
            wcur_u32(&c, msg->newfid);
            uint16_t n = msg->nwname > P9_MAX_WALK_ELEM ? P9_MAX_WALK_ELEM : msg->nwname;
            wcur_u16(&c, n);
            for (uint16_t i = 0; i < n; i++) wcur_str(&c, msg->wname[i]);
            break;
        }
        case P9_RWALK: {
            uint16_t n = msg->nwqid > P9_MAX_WALK_ELEM ? P9_MAX_WALK_ELEM : msg->nwqid;
            wcur_u16(&c, n);
            for (uint16_t i = 0; i < n; i++) wcur_qid(&c, &msg->wqid[i]);
            break;
        }
        case P9_TOPEN:
            wcur_u32(&c, msg->fid);
            wcur_u8(&c, msg->mode);
            break;
        case P9_ROPEN:
        case P9_RCREATE:
            wcur_qid(&c, &msg->qid);
            wcur_u32(&c, 4096); // iounit: no restriction beyond the negotiated msize
            break;
        case P9_TCREATE:
            wcur_u32(&c, msg->fid);
            wcur_str(&c, msg->name);
            wcur_u32(&c, msg->perm);
            wcur_u8(&c, msg->mode);
            break;
        case P9_TWRITE:
            wcur_u32(&c, msg->fid);
            wcur_u64(&c, msg->offset);
            wcur_u32(&c, msg->count);
            wcur_bytes(&c, msg->data, msg->count);
            break;
        case P9_RWRITE:
            wcur_u32(&c, msg->count);
            break;
        case P9_TREAD:
            wcur_u32(&c, msg->fid);
            wcur_u64(&c, msg->offset);
            wcur_u32(&c, msg->count);
            break;
        case P9_RREAD:
            wcur_u32(&c, msg->count);
            wcur_bytes(&c, msg->data, msg->count);
            break;
        case P9_RSTAT:
            wcur_u16(&c, (uint16_t)msg->count);
            wcur_bytes(&c, msg->data, msg->count);
            break;
        case P9_TSTAT:
        case P9_TCLUNK:
        case P9_TREMOVE:
            wcur_u32(&c, msg->fid);
            break;
        case P9_TFLUSH:
            wcur_u16(&c, msg->oldtag);
            break;
        case P9_RCLUNK:
        case P9_RREMOVE:
        case P9_RFLUSH:
            break; // no body
        default:
            break;
    }

    if (c.overflow) return -1;
    uint32_t total_size = (uint32_t)(c.p - buf);
    buf[0] = (uint8_t)(total_size & 0xFF);
    buf[1] = (uint8_t)((total_size >> 8) & 0xFF);
    buf[2] = (uint8_t)((total_size >> 16) & 0xFF);
    buf[3] = (uint8_t)((total_size >> 24) & 0xFF);
    return (int)total_size;
}

int p9_deserialize(const uint8_t *buf, uint32_t len, p9_msg_t *msg) {
    if (!buf || !msg || len < 7) return -1;
    memset(msg, 0, sizeof(*msg));

    rcur_t c = { .p = buf, .end = buf + len, .overflow = false };
    uint32_t size;
    if (!rcur_u32(&c, &size)) return -1;
    if (size < 7 || size > len) return -1;
    /* Clamp the working window to the frame's own declared size, so trailing
     * bytes left over in a reused fixed-size buffer (from a previous,
     * longer message) can never be read as part of this one. */
    c.end = buf + size;

    uint8_t type;
    if (!rcur_u8(&c, &type)) return -1;
    msg->type = type;
    if (!rcur_u16(&c, &msg->tag)) return -1;

    bool ok = true;
    switch (msg->type) {
        case P9_TVERSION:
        case P9_RVERSION: {
            ok = rcur_u32(&c, &msg->msize);
            char proto[16];
            if (ok) ok = rcur_str(&c, proto, sizeof(proto));
            break;
        }
        case P9_TATTACH: {
            ok = rcur_u32(&c, &msg->fid);
            uint32_t afid;
            if (ok) ok = rcur_u32(&c, &afid);
            if (ok) ok = rcur_str(&c, msg->uname, sizeof(msg->uname));
            if (ok) ok = rcur_str(&c, msg->aname, sizeof(msg->aname));
            break;
        }
        case P9_RATTACH:
            ok = rcur_qid(&c, &msg->qid);
            break;
        case P9_RERROR: {
            char ename_tmp[64];
            ok = rcur_str(&c, ename_tmp, sizeof(ename_tmp));
            break;
        }
        case P9_TWALK: {
            ok = rcur_u32(&c, &msg->fid);
            if (ok) ok = rcur_u32(&c, &msg->newfid);
            uint16_t n = 0;
            if (ok) ok = rcur_u16(&c, &n);
            if (ok && n > P9_MAX_WALK_ELEM) ok = false; // reject rather than silently truncate
            if (ok) {
                msg->nwname = n;
                for (uint16_t i = 0; ok && i < n; i++) ok = rcur_str(&c, msg->wname[i], P9_MAX_NAME_LEN);
            }
            break;
        }
        case P9_RWALK: {
            uint16_t n = 0;
            ok = rcur_u16(&c, &n);
            if (ok && n > P9_MAX_WALK_ELEM) ok = false;
            if (ok) {
                msg->nwqid = n;
                for (uint16_t i = 0; ok && i < n; i++) ok = rcur_qid(&c, &msg->wqid[i]);
            }
            break;
        }
        case P9_TOPEN:
            ok = rcur_u32(&c, &msg->fid);
            if (ok) ok = rcur_u8(&c, &msg->mode);
            break;
        case P9_ROPEN:
        case P9_RCREATE: {
            ok = rcur_qid(&c, &msg->qid);
            uint32_t iounit;
            if (ok) ok = rcur_u32(&c, &iounit);
            break;
        }
        case P9_TCREATE:
            ok = rcur_u32(&c, &msg->fid);
            if (ok) ok = rcur_str(&c, msg->name, sizeof(msg->name));
            if (ok) ok = rcur_u32(&c, &msg->perm);
            if (ok) ok = rcur_u8(&c, &msg->mode);
            break;
        case P9_TWRITE:
            ok = rcur_u32(&c, &msg->fid);
            if (ok) ok = rcur_u64(&c, &msg->offset);
            if (ok) ok = rcur_u32(&c, &msg->count);
            if (ok) ok = rcur_data(&c, &msg->data, msg->count);
            break;
        case P9_RWRITE:
            ok = rcur_u32(&c, &msg->count);
            break;
        case P9_TREAD:
            ok = rcur_u32(&c, &msg->fid);
            if (ok) ok = rcur_u64(&c, &msg->offset);
            if (ok) ok = rcur_u32(&c, &msg->count);
            break;
        case P9_RREAD:
            ok = rcur_u32(&c, &msg->count);
            if (ok) ok = rcur_data(&c, &msg->data, msg->count);
            break;
        case P9_RSTAT: {
            uint16_t n;
            ok = rcur_u16(&c, &n);
            if (ok) ok = rcur_data(&c, &msg->data, n);
            if (ok) msg->count = n;
            break;
        }
        case P9_TSTAT:
        case P9_TCLUNK:
        case P9_TREMOVE:
            ok = rcur_u32(&c, &msg->fid);
            break;
        case P9_TFLUSH:
            ok = rcur_u16(&c, &msg->oldtag);
            break;
        case P9_RCLUNK:
        case P9_RREMOVE:
        case P9_RFLUSH:
            break; // no body
        default:
            /* Unknown type: header (type/tag) parsed fine, so the caller can
             * still reply with a clean Rerror rather than dropping the
             * frame silently. p9_server_process() rejects unknown types. */
            break;
    }

    if (!ok || c.overflow) return -1;
    return 0;
}

int p9_server_process(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max) {
    p9_msg_t req;
    if (p9_deserialize(req_buf, req_len, &req) < 0) {
        p9_msg_t err = { .type = P9_RERROR, .tag = P9_NOTAG, .ename = "Invalid 9P frame" };
        return p9_serialize(&err, resp_buf, resp_max);
    }

    p9_msg_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.tag = req.tag;

    /* Scratch space for Tread/Tstat payloads. Single static buffer: this
     * server processes one request at a time (no concurrency in this
     * kernel yet), same non-reentrancy contract as ksnprintf()'s buffer. */
    static uint8_t s_databuf[P9_MAX_MSIZE];
    uint32_t header_overhead = 11; // worst case of RREAD's/RWRITE's/RSTAT's framing: size4+type1+tag2+count4
    uint32_t max_payload = (resp_max > header_overhead) ? (resp_max - header_overhead) : 0;
    if (max_payload > sizeof(s_databuf)) max_payload = sizeof(s_databuf);

    switch (req.type) {
        case P9_TVERSION:
            p9_fid_reset_all(); // Tversion (re)initializes the connection
            resp.type = P9_RVERSION;
            resp.msize = (req.msize > 0 && req.msize < P9_MAX_MSIZE) ? req.msize : P9_MAX_MSIZE;
            break;
        case P9_TATTACH:
            p9_handle_tattach(&req, &resp);
            break;
        case P9_TWALK:
            p9_handle_twalk(&req, &resp);
            break;
        case P9_TOPEN:
            p9_handle_topen(&req, &resp);
            break;
        case P9_TCREATE:
            p9_handle_tcreate(&req, &resp);
            break;
        case P9_TREAD:
            p9_handle_tread(&req, &resp, s_databuf, max_payload);
            break;
        case P9_TWRITE:
            p9_handle_twrite(&req, &resp);
            break;
        case P9_TCLUNK:
            p9_handle_tclunk(&req, &resp);
            break;
        case P9_TREMOVE:
            p9_handle_tremove(&req, &resp);
            break;
        case P9_TSTAT:
            p9_handle_tstat(&req, &resp, s_databuf, max_payload);
            break;
        case P9_TFLUSH:
            // No concurrency to flush yet; always succeeds as a no-op.
            resp.type = P9_RFLUSH;
            break;
        default:
            resp.type = P9_RERROR;
            resp.ename = "Unsupported 9P request type";
            break;
    }
    return p9_serialize(&resp, resp_buf, resp_max);
}
