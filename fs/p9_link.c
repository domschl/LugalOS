#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/printk.h"
#include <string.h>

static p9_link_t *g_background_link = NULL;

int p9_link_service(p9_link_t *link) {
    if (!link || !link->poll || !link->recv_frame || !link->send_frame) return -1;

    int ready = link->poll(link);
    if (ready <= 0) return ready; // 0 = nothing pending, -1 = transport error

    static uint8_t req_buf[P9_MAX_MSIZE];
    static uint8_t resp_buf[P9_MAX_MSIZE];

    int req_len = link->recv_frame(link, req_buf, sizeof(req_buf));
    if (req_len < 7) return -1;

    int resp_len = p9_server_process(req_buf, (uint32_t)req_len, resp_buf, sizeof(resp_buf));
    if (resp_len < 7) return -1;

    if (link->send_frame(link, resp_buf, (uint32_t)resp_len) < 0) return -1;
    return 1;
}

void p9_link_register_background(p9_link_t *link) {
    g_background_link = link;
    if (link) {
        printk("[9P Link] '%s' registered as background transport.\n", link->name ? link->name : "?");
    }
}

void p9_link_background_poll(void) {
    if (!g_background_link) return;
    p9_link_service(g_background_link);
}

/* --- p9_link_cat: a link-agnostic synchronous 9P client (A4) ---
 *
 * Safe to run on a link that also has a registered background server (see
 * p9_link_register_background() above), including on itself: this kernel
 * has no real task scheduler or interrupts that could preempt a C function
 * call (kernel/sched.c is a bookkeeping shim, not a scheduler -- see the
 * A1/A2 completion notes), so while p9_link_cat() is running, nothing else
 * -- including this link's own background pump -- can run concurrently and
 * misinterpret one of its frames. The remote peer only ever *responds* to
 * what this function sends (it never spontaneously originates traffic), so
 * there is no risk of the two ends' server roles colliding either.
 *
 * This does mean the roles are asymmetric by convention, not by
 * negotiation: whichever node actively calls p9_link_cat() is acting as
 * client for that one exchange, and the peer it's talking to must be
 * purely a server (running only its own background pump) for as long as
 * that holds. Good enough for A4's test topology (see the completion
 * notes); a real bidirectional peer-to-peer protocol would need tag-aware
 * multiplexing this doesn't attempt. */

/* Waits for a reply, exactly like virtio_blk_transfer()'s own
 * busy-wait-until-done poll (drivers/virtio_blk.c) -- no separate timeout
 * mechanism; a non-responsive peer hangs the caller, matching that
 * existing precedent rather than inventing a new one. */
static int p9_link_wait_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    for (;;) {
        int ready = link->poll(link);
        if (ready < 0) return -1;
        if (ready > 0) return link->recv_frame(link, buf, max_len);
    }
}

static int p9_link_roundtrip(p9_link_t *link, const p9_msg_t *req, p9_msg_t *resp,
                              uint8_t *tx, uint8_t *rx, uint32_t cap) {
    int tx_len = p9_serialize(req, tx, cap);
    if (tx_len < 7) return -1;
    if (link->send_frame(link, tx, (uint32_t)tx_len) < 0) return -1;
    int rx_len = p9_link_wait_frame(link, rx, cap);
    if (rx_len < 7) return -1;
    return p9_deserialize(rx, (uint32_t)rx_len, resp);
}

int p9_link_cat(p9_link_t *link, const char *path, char *out_buf, uint32_t out_max) {
    if (!link || !link->poll || !link->send_frame || !link->recv_frame || !path) return -1;
    while (*path == '/') path++;

    static uint8_t tx[P9_MAX_MSIZE];
    static uint8_t rx[P9_MAX_MSIZE];
    p9_msg_t req, resp;

    memset(&req, 0, sizeof(req));
    req.type = P9_TVERSION;
    req.tag = 1;
    req.msize = P9_MAX_MSIZE;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RVERSION) return -1;

    memset(&req, 0, sizeof(req));
    req.type = P9_TATTACH;
    req.tag = 2;
    req.fid = 1;
    strncpy(req.uname, "lugal", sizeof(req.uname) - 1);
    // aname left empty -> namespace root "/"
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RATTACH) return -1;

    memset(&req, 0, sizeof(req));
    req.type = P9_TWALK;
    req.tag = 3;
    req.fid = 1;
    req.newfid = 2;
    char pathcopy[128];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';
    uint16_t n = 0;
    char *tok = pathcopy;
    while (*tok && n < P9_MAX_WALK_ELEM) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok) {
            strncpy(req.wname[n], tok, P9_MAX_NAME_LEN - 1);
            n++;
        }
        if (!slash) break;
        tok = slash + 1;
    }
    req.nwname = n;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RWALK || resp.nwqid != n) return -1;

    memset(&req, 0, sizeof(req));
    req.type = P9_TOPEN;
    req.tag = 4;
    req.fid = 2;
    req.mode = P9_OREAD;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_ROPEN) return -1;

    uint32_t total = 0;
    uint64_t offset = 0;
    uint16_t tag = 5;
    bool read_failed = false;
    for (;;) {
        uint32_t room = (out_buf && out_max > 0 && total + 1 < out_max) ? (out_max - 1 - total) : 0;
        uint32_t want = room < 1024 ? room : 1024;
        if (want == 0) break;

        memset(&req, 0, sizeof(req));
        req.type = P9_TREAD;
        req.tag = tag++;
        req.fid = 2;
        req.offset = offset;
        req.count = want;
        if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RREAD) { read_failed = true; break; }
        if (resp.count == 0) break;

        if (out_buf) memcpy(out_buf + total, resp.data, resp.count);
        total += resp.count;
        offset += resp.count;
    }
    if (out_buf && out_max > 0) out_buf[(total < out_max) ? total : out_max - 1] = '\0';

    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = tag;
    req.fid = 2;
    p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)); // best-effort cleanup

    return read_failed ? -1 : (int)total;
}

/* --- Persistent remote 9P connection (A5) ---
 *
 * A fixed pool, not dynamic allocation (this kernel has none): one mount
 * per real remote peer is the expected usage (see p9_remote_mount_open()'s
 * doc comment in the header for why mounting the same underlying link
 * twice isn't meaningful -- the second Tversion would reset the first
 * mount's fids server-side). */
#define P9_REMOTE_MAX_MOUNTS 4

struct p9_remote_mount {
    bool in_use;
    p9_link_t *link;
    uint32_t root_fid;
    uint32_t next_fid;
    uint16_t next_tag;
};

static p9_remote_mount_t g_remote_mounts[P9_REMOTE_MAX_MOUNTS];
static uint8_t g_remote_tx[P9_MAX_MSIZE];
static uint8_t g_remote_rx[P9_MAX_MSIZE];

static uint16_t p9_remote_next_tag(p9_remote_mount_t *m) {
    uint16_t t = m->next_tag++;
    if (m->next_tag == 0) m->next_tag = 1; // skip 0 on wraparound, no protocol reason to avoid it beyond tidiness
    return t;
}

static int p9_remote_xchg(p9_remote_mount_t *m, const p9_msg_t *req, p9_msg_t *resp) {
    return p9_link_roundtrip(m->link, req, resp, g_remote_tx, g_remote_rx, sizeof(g_remote_tx));
}

p9_remote_mount_t *p9_remote_mount_open(p9_link_t *link) {
    if (!link) return NULL;

    p9_remote_mount_t *m = NULL;
    for (int i = 0; i < P9_REMOTE_MAX_MOUNTS; i++) {
        if (!g_remote_mounts[i].in_use) { m = &g_remote_mounts[i]; break; }
    }
    if (!m) return NULL;

    memset(m, 0, sizeof(*m));
    m->link = link;
    m->next_fid = 1;
    m->next_tag = 1;

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TVERSION;
    req.tag = p9_remote_next_tag(m);
    req.msize = P9_MAX_MSIZE;
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RVERSION) return NULL;

    uint32_t root_fid = m->next_fid++;
    memset(&req, 0, sizeof(req));
    req.type = P9_TATTACH;
    req.tag = p9_remote_next_tag(m);
    req.fid = root_fid;
    strncpy(req.uname, "lugal", sizeof(req.uname) - 1);
    // aname left empty -> the peer's namespace root "/"
    if (p9_remote_xchg(m, &req, &resp) < 0 || resp.type != P9_RATTACH) return NULL;

    m->root_fid = root_fid;
    m->in_use = true;
    return m;
}

void p9_remote_mount_close(p9_remote_mount_t *m) {
    if (!m || !m->in_use) return;
    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = p9_remote_next_tag(m);
    req.fid = m->root_fid;
    p9_remote_xchg(m, &req, &resp); // best-effort
    memset(m, 0, sizeof(*m));
}

/* Splits `path` into up to P9_MAX_WALK_ELEM wname[] components in `out`
 * (used by open/remove/mkdir below, all of which need the same
 * component-splitting logic p9_link_cat() above already has inline --
 * pulled out here since this section needs it three times). */
static uint16_t p9_remote_split_path(const char *path, char out[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN]) {
    char pathcopy[128];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';

    uint16_t n = 0;
    char *tok = pathcopy;
    while (*tok && n < P9_MAX_WALK_ELEM) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok) {
            strncpy(out[n], tok, P9_MAX_NAME_LEN - 1);
            n++;
        }
        if (!slash) break;
        tok = slash + 1;
    }
    return n;
}

int p9_remote_open(p9_remote_mount_t *mount, const char *path, uint8_t p9_mode, bool create, uint32_t *out_fid, bool *out_is_dir) {
    if (!mount || !mount->in_use || !path || !out_fid) return -1;
    while (*path == '/') path++;

    char wnames[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t n = p9_remote_split_path(path, wnames);

    uint32_t fid = mount->next_fid++;
    p9_msg_t twalk, resp;
    memset(&twalk, 0, sizeof(twalk));
    twalk.type = P9_TWALK;
    twalk.tag = p9_remote_next_tag(mount);
    twalk.fid = mount->root_fid;
    twalk.newfid = fid;
    twalk.nwname = n;
    memcpy(twalk.wname, wnames, sizeof(wnames));
    if (p9_remote_xchg(mount, &twalk, &resp) < 0) return -1;

    bool is_dir = true;
    if (resp.type == P9_RWALK && resp.nwqid == n) {
        // Full walk succeeded: the file/directory already exists.
        if (n > 0) is_dir = (resp.wqid[n - 1].type & P9_QTDIR) != 0;

        p9_msg_t topen;
        memset(&topen, 0, sizeof(topen));
        topen.type = P9_TOPEN;
        topen.tag = p9_remote_next_tag(mount);
        topen.fid = fid;
        topen.mode = p9_mode;
        if (p9_remote_xchg(mount, &topen, &resp) < 0 || resp.type != P9_ROPEN) {
            p9_remote_close(mount, fid);
            return -1;
        }
    } else if (resp.type == P9_RWALK && n > 0 && resp.nwqid == (uint16_t)(n - 1) && create) {
        // Walked exactly to the parent directory; only the final component
        // is missing -- create it there (Tcreate leaves `fid` open on the
        // new file, no separate Topen needed).
        p9_msg_t tcreate;
        memset(&tcreate, 0, sizeof(tcreate));
        tcreate.type = P9_TCREATE;
        tcreate.tag = p9_remote_next_tag(mount);
        tcreate.fid = fid;
        strncpy(tcreate.name, wnames[n - 1], sizeof(tcreate.name) - 1);
        tcreate.perm = 0644;
        tcreate.mode = p9_mode;
        if (p9_remote_xchg(mount, &tcreate, &resp) < 0 || resp.type != P9_RCREATE) {
            p9_remote_close(mount, fid);
            return -1;
        }
        is_dir = false;
    } else {
        // Walk failed outright, or stopped short of even the parent (a
        // genuinely missing intermediate directory -- this doesn't imply
        // `mkdir -p`, matching the local vfs_open()'s own semantics).
        if (resp.type == P9_RWALK) p9_remote_close(mount, fid);
        return -1;
    }

    *out_fid = fid;
    if (out_is_dir) *out_is_dir = is_dir;
    return 0;
}

int p9_remote_pread(p9_remote_mount_t *mount, uint32_t fid, void *buf, uint32_t count, uint64_t offset) {
    if (!mount || !mount->in_use || !buf) return -1;
    if (count > sizeof(g_remote_rx) - 32) count = sizeof(g_remote_rx) - 32; // framing headroom

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TREAD;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    req.offset = offset;
    req.count = count;
    if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RREAD) return -1;
    if (resp.count > count) return -1; // defensive: peer claimed more than requested

    if (resp.count > 0) memcpy(buf, resp.data, resp.count);
    return (int)resp.count;
}

int p9_remote_pwrite(p9_remote_mount_t *mount, uint32_t fid, const void *buf, uint32_t count, uint64_t offset) {
    if (!mount || !mount->in_use || !buf) return -1;
    if (count > sizeof(g_remote_tx) - 64) count = sizeof(g_remote_tx) - 64; // framing headroom

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TWRITE;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    req.offset = offset;
    req.count = count;
    req.data = (const uint8_t *)buf;
    if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RWRITE) return -1;
    return (int)resp.count;
}

/* Unpacks one 9P "stat" wire entry (the format fs/9p.c's p9_pack_stat()
 * produces, including its self-referential leading size field) starting at
 * src[0]. Returns the total bytes consumed (>= 2), or -1 if `src_len`
 * doesn't hold a complete, well-formed entry -- bounds-checked at every
 * step even though the only producer today is our own trusted server,
 * matching this codebase's general B11-era posture of not trusting wire
 * data just because nothing hostile is expected to send it yet. */
static int p9_unpack_stat_entry(const uint8_t *src, uint32_t src_len, char *name_out, uint32_t name_max, uint64_t *size_out, bool *is_dir_out) {
    if (src_len < 2) return -1;
    uint16_t inner_size = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
    uint32_t total = 2u + inner_size;
    if (total > src_len || total < 2 + 39 + 2 + 2 + 2) return -1; // fixed fields + 3 empty strings + name len

    uint32_t off = 2;
    off += 2; // type (kernel-use only)
    off += 4; // dev (kernel-use only)
    uint8_t qid_type = src[off];
    off += 1 + 4 + 8; // qid.type already read; skip qid.vers + qid.path
    uint32_t mode = (uint32_t)src[off] | ((uint32_t)src[off + 1] << 8) | ((uint32_t)src[off + 2] << 16) | ((uint32_t)src[off + 3] << 24);
    off += 4;
    off += 4 + 4; // atime, mtime
    uint64_t length = 0;
    for (int i = 0; i < 8; i++) length |= ((uint64_t)src[off + i]) << (8 * i);
    off += 8;

    if (off + 2 > total) return -1;
    uint16_t namelen = (uint16_t)(src[off] | ((uint16_t)src[off + 1] << 8));
    off += 2;
    if (off + namelen > total) return -1;
    if (name_out && name_max > 0) {
        uint32_t copy = namelen < name_max - 1 ? namelen : name_max - 1;
        memcpy(name_out, src + off, copy);
        name_out[copy] = '\0';
    }
    off += namelen;

    for (int s = 0; s < 3; s++) { // uid, gid, muid -- not needed, just skip past them
        if (off + 2 > total) return -1;
        uint16_t slen = (uint16_t)(src[off] | ((uint16_t)src[off + 1] << 8));
        off += 2;
        if (off + slen > total) return -1;
        off += slen;
    }

    if (size_out) *size_out = length;
    if (is_dir_out) *is_dir_out = ((mode & P9_DMDIR) != 0) || ((qid_type & P9_QTDIR) != 0);
    return (int)total;
}

/* Re-walks the directory's Tread stream from offset 0 on every call rather
 * than caching decoded entries in the handle -- vfs_readdir()'s index-based
 * contract doesn't map cleanly onto 9P's stream-of-stats without either a
 * per-handle cache (real memory cost on RP2350's tight SRAM budget, times
 * VFS_MAX_HANDLES, for a feature only remote directory handles ever use)
 * or this: no retained state at all, at the cost of O(directory size) wire
 * traffic per call. Every directory in this system is small; matches A1's
 * own fat32_read_at() precedent of "walk from the start every call,
 * revisit if it's ever a hot path." */
int p9_remote_readdir(p9_remote_mount_t *mount, uint32_t fid, uint32_t index, char *name_out, uint32_t name_max, p9_remote_stat_t *stat_out) {
    if (!mount || !mount->in_use) return -1;

    uint64_t offset = 0;
    uint32_t found_index = 0;
    for (;;) {
        p9_msg_t req, resp;
        memset(&req, 0, sizeof(req));
        req.type = P9_TREAD;
        req.tag = p9_remote_next_tag(mount);
        req.fid = fid;
        req.offset = offset;
        req.count = 1024;
        if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RREAD) return -1;
        if (resp.count == 0) return -1; // EOF -- index out of range

        uint32_t p = 0;
        while (p < resp.count) {
            char name[P9_MAX_NAME_LEN];
            uint64_t size;
            bool is_dir;
            int consumed = p9_unpack_stat_entry(resp.data + p, resp.count - p, name, sizeof(name), &size, &is_dir);
            if (consumed < 0) break; // incomplete entry at the tail of this chunk; next Tread continues past it

            if (found_index == index) {
                if (name_out && name_max > 0) {
                    strncpy(name_out, name, name_max - 1);
                    name_out[name_max - 1] = '\0';
                }
                if (stat_out) { stat_out->size = size; stat_out->is_dir = is_dir; }
                return 0;
            }
            found_index++;
            p += (uint32_t)consumed;
        }
        offset += resp.count;
    }
}

int p9_remote_fstat(p9_remote_mount_t *mount, uint32_t fid, p9_remote_stat_t *out) {
    if (!mount || !mount->in_use || !out) return -1;

    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TSTAT;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    if (p9_remote_xchg(mount, &req, &resp) < 0 || resp.type != P9_RSTAT) return -1;

    uint64_t size = 0;
    bool is_dir = false;
    if (p9_unpack_stat_entry(resp.data, resp.count, NULL, 0, &size, &is_dir) < 0) return -1;
    out->size = size;
    out->is_dir = is_dir;
    return 0;
}

int p9_remote_close(p9_remote_mount_t *mount, uint32_t fid) {
    if (!mount || !mount->in_use) return -1;
    p9_msg_t req, resp;
    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = p9_remote_next_tag(mount);
    req.fid = fid;
    p9_remote_xchg(mount, &req, &resp); // best-effort
    return 0;
}

int p9_remote_remove(p9_remote_mount_t *mount, const char *path) {
    if (!mount || !mount->in_use || !path) return -1;
    while (*path == '/') path++;

    char wnames[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t n = p9_remote_split_path(path, wnames);
    if (n == 0) return -1; // refuse to remove the mount root

    uint32_t fid = mount->next_fid++;
    p9_msg_t twalk, resp;
    memset(&twalk, 0, sizeof(twalk));
    twalk.type = P9_TWALK;
    twalk.tag = p9_remote_next_tag(mount);
    twalk.fid = mount->root_fid;
    twalk.newfid = fid;
    twalk.nwname = n;
    memcpy(twalk.wname, wnames, sizeof(wnames));
    if (p9_remote_xchg(mount, &twalk, &resp) < 0 || resp.type != P9_RWALK || resp.nwqid != n) {
        if (resp.type == P9_RWALK) p9_remote_close(mount, fid);
        return -1;
    }

    p9_msg_t tremove;
    memset(&tremove, 0, sizeof(tremove));
    tremove.type = P9_TREMOVE;
    tremove.tag = p9_remote_next_tag(mount);
    tremove.fid = fid;
    if (p9_remote_xchg(mount, &tremove, &resp) < 0 || resp.type != P9_RREMOVE) return -1;
    return 0;
}

int p9_remote_mkdir(p9_remote_mount_t *mount, const char *path) {
    if (!mount || !mount->in_use || !path) return -1;
    while (*path == '/') path++;

    char wnames[P9_MAX_WALK_ELEM][P9_MAX_NAME_LEN];
    uint16_t n = p9_remote_split_path(path, wnames);
    if (n == 0) return -1;

    uint32_t fid = mount->next_fid++;
    p9_msg_t twalk, resp;
    memset(&twalk, 0, sizeof(twalk));
    twalk.type = P9_TWALK;
    twalk.tag = p9_remote_next_tag(mount);
    twalk.fid = mount->root_fid;
    twalk.newfid = fid;
    twalk.nwname = (uint16_t)(n - 1); // walk to the parent only
    memcpy(twalk.wname, wnames, sizeof(wnames));
    if (p9_remote_xchg(mount, &twalk, &resp) < 0 || resp.type != P9_RWALK || resp.nwqid != n - 1) {
        if (resp.type == P9_RWALK) p9_remote_close(mount, fid);
        return -1;
    }

    p9_msg_t tcreate;
    memset(&tcreate, 0, sizeof(tcreate));
    tcreate.type = P9_TCREATE;
    tcreate.tag = p9_remote_next_tag(mount);
    tcreate.fid = fid;
    strncpy(tcreate.name, wnames[n - 1], sizeof(tcreate.name) - 1);
    tcreate.perm = P9_DMDIR | 0755;
    tcreate.mode = P9_OREAD;
    if (p9_remote_xchg(mount, &tcreate, &resp) < 0 || resp.type != P9_RCREATE) return -1;
    p9_remote_close(mount, fid); // Tcreate leaves it open; mkdir doesn't need to keep it that way
    return 0;
}
