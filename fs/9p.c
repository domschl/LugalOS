#include "fs/9p.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sha256.h"
#include "kernel/random.h"
#include "kernel/idstore.h"
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

    /* N2: an auth fid. Not a file, not on any disk, and deliberately in the
     * same table -- 9P gives an afid a fid number from the same space, and a
     * client clunks it the same way. `is_auth` is what keeps it out of every
     * path that expects `path` to mean something. */
    bool     is_auth;
    bool     authed;            // the response HMAC has been verified
    uint8_t  nonce[SHA256_DIGEST_LEN];
    char     auth_uname[P9_MAX_NAME_LEN];
    char     auth_aname[P9_MAX_NAME_LEN];
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

/* --- The auth gate (N2, plan/phase18_networking_and_auth.md §6) --------- */

/* Longest pre-shared key this reads out of a key file. 64 bytes is HMAC's
 * block length: beyond it RFC 2104 hashes the key first, so a longer one buys
 * no strength, and a key file line longer than this is far more likely to be
 * a mistake than an intention. */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Reads a whole (small) file. Key files are lines of hex; anything that does
 * not fit this buffer is not a key file. */
static int p9_read_small_file(const char *path, char *buf, uint32_t cap) {
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0 || st.is_dir) return -1;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return -1;
    int n = vfs_pread(fd, buf, cap - 1, 0);
    vfs_close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/* The key for `uname`, or -1 if there is none.
 *
 * Format of P9_AUTH_KEYS_FILE: one identity per line, `uname` and a hex key
 * separated by spaces or a tab. Blank lines and lines beginning with '#' are
 * ignored. A line can be deleted to revoke an identity, which is the whole
 * reason this is a list rather than one secret.
 *
 * P9_AUTH_FALLBACK_KEY_FILE is a bare hex key with no uname, for a board with
 * no card: it answers for any uname, and a gateway that wants per-identity
 * keys should use the list. */
/* A key set from the local console, for this boot only.
 *
 * Two jobs, and neither is "a place to keep secrets". It bootstraps a gateway
 * whose card has no key file yet -- otherwise installing the first key means
 * pulling the SD card -- and it is how the QEMU suite tests the gate at all,
 * since a QEMU node has no writable /sd0 and its /flash0 is a read-only image.
 *
 * The alternative was baking a test key into the flash image, which would put
 * a known key on every board that image is ever written to. Not doing that is
 * the point of this variable existing.
 *
 * Safe by the same argument that lets the local channel skip authentication:
 * whoever has the console already has the board. It is never reachable over
 * 9P (it is not a file), and it does not survive a reboot. */
static uint8_t  g_console_key[P9_AUTH_KEY_MAX];
static uint32_t g_console_key_len;

void p9_auth_set_console_key(const uint8_t *key, uint32_t len) {
    if (!key || len == 0 || len > sizeof(g_console_key)) {
        memset(g_console_key, 0, sizeof(g_console_key));
        g_console_key_len = 0;
        return;
    }
    memcpy(g_console_key, key, len);
    g_console_key_len = len;
}

int p9_auth_key_for(const char *uname, uint8_t *key_out, uint32_t *key_len) {
    /* Console key first: it is the bootstrap and the override, and a gateway
     * being repaired should not have to fight its own card. */
    if (g_console_key_len > 0) {
        memcpy(key_out, g_console_key, g_console_key_len);
        *key_len = g_console_key_len;
        return 0;
    }

    char buf[512];
    int n = p9_read_small_file(P9_AUTH_KEYS_FILE, buf, sizeof(buf));

    if (n > 0) {
        const char *p = buf;
        while (*p) {
            const char *line = p;
            while (*p && *p != '\n') p++;
            const char *line_end = p;
            if (*p == '\n') p++;

            while (line < line_end && (*line == ' ' || *line == '\t' || *line == '\r')) line++;
            if (line >= line_end || *line == '#') continue;

            /* name */
            const char *name = line;
            const char *ne = name;
            while (ne < line_end && *ne != ' ' && *ne != '\t' && *ne != '\r') ne++;
            uint32_t nlen = (uint32_t)(ne - name);
            if (nlen == 0) continue;
            /* `*` matches any uname. Explicit, never implied: since R3's
             * identity work every node attaches under its own name
             * ("rp2350-gateway-3f2a"), which is what makes phase 18 §6's
             * "multiple keys identify who" real -- and which also means a
             * keys file written when every node was "lugal" stops matching.
             * A segment that genuinely shares one key writes one line and
             * says so; nothing falls back silently, because a silent
             * fallback would quietly undo the identification this exists
             * for. */
            bool wildcard = (nlen == 1 && name[0] == '*');
            if (!wildcard &&
                (nlen != strlen(uname) || strncmp(name, uname, nlen) != 0)) continue;

            /* hex key */
            const char *k = ne;
            while (k < line_end && (*k == ' ' || *k == '\t')) k++;
            uint32_t len = 0;
            while (k + 1 < line_end && len < P9_AUTH_KEY_MAX) {
                int hi = hexval(k[0]), lo = hexval(k[1]);
                if (hi < 0 || lo < 0) break;
                key_out[len++] = (uint8_t)((hi << 4) | lo);
                k += 2;
            }
            if (len == 0) continue;
            *key_len = len;
            return 0;
        }
    }

    n = p9_read_small_file(P9_AUTH_FALLBACK_KEY_FILE, buf, sizeof(buf));
    if (n > 0) {
        uint32_t len = 0;
        const char *k = buf;
        while (k[0] && k[1] && len < P9_AUTH_KEY_MAX) {
            int hi = hexval(k[0]), lo = hexval(k[1]);
            if (hi < 0 || lo < 0) break;
            key_out[len++] = (uint8_t)((hi << 4) | lo);
            k += 2;
        }
        if (len > 0) { *key_len = len; return 0; }
    }
    return -1;
}

bool p9_auth_have_keys(void) {
    if (g_console_key_len > 0) return true;
    vfs_stat_t st;
    if (vfs_stat(P9_AUTH_KEYS_FILE, &st) == 0 && !st.is_dir) return true;
    if (vfs_stat(P9_AUTH_FALLBACK_KEY_FILE, &st) == 0 && !st.is_dir) return true;
    return false;
}

/* Paths this server will not serve to anybody, over any transport.
 *
 * The gateway persona exports /sd0, and its keys live on /sd0. Without this,
 * the first thing an authenticated client could do is read the list of
 * everyone else's secrets -- which would reduce a per-identity key scheme to
 * one shared key, awkwardly spelled. Enforced on the SERVER because in this
 * threat model the client is the adversary; a check in host/fuse-p9 would be
 * a suggestion.
 *
 * Prefix match, so the directory itself and everything under it are refused
 * together, and refused identically whether the client walks to it, opens it,
 * stats it or tries to remove it. A local shell on the board can still read
 * the file: someone holding the board already has the flash. */
static bool p9_path_is_secret(const char *path) {
    if (!path) return false;
    /* I3, plan/phase21_identity_and_authentication.md §4: "the identity
     * store must join it" -- the same guard, not a second one, so a path
     * refused here is refused for exactly one reason regardless of which
     * secret it would have served. */
    if (idstore_path_is_secret(path)) return true;
    /* The single-key fallback is a key too. Guarding only the directory would
     * have left the simpler configuration -- the one a board with no SD card
     * uses -- readable by anyone who attached. */
    if (strcmp(path, P9_AUTH_FALLBACK_KEY_FILE) == 0) return true;
    size_t n = strlen(P9_AUTH_KEY_DIR);
    if (strncmp(path, P9_AUTH_KEY_DIR, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

/* Exposed for the self-test: the guard is the one piece of the auth gate that
 * is pure string logic, so it can be checked without a network, a key or a
 * peer -- and it is the piece whose failure is silent. */
bool p9_auth_path_is_secret(const char *path) { return p9_path_is_secret(path); }

/* The policy in force for the request being processed. Set once per call from
 * p9_server_process()'s argument; a static rather than a parameter threaded
 * through nine handlers, which is honest for a server that has one connection
 * at a time (Tversion resets all fid state precisely because of that). */
static p9_auth_policy_t g_auth_policy = P9_AUTH_NOT_REQUIRED;

static void p9_handle_tauth(const p9_msg_t *req, p9_msg_t *resp) {
    if (req->afid == P9_NOFID) {
        resp->type = P9_RERROR; resp->ename = "auth: afid may not be NOFID"; return;
    }
    /* Refusing here rather than at Tattach means a gateway with no keys
     * installed says so at the first step, instead of letting a client
     * complete a pointless handshake. */
    if (!p9_auth_have_keys()) {
        resp->type = P9_RERROR;
        resp->ename = "auth: no keys configured on this server";
        return;
    }

    p9_fid_entry_t *e = p9_fid_alloc(req->afid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "auth: afid in use or table full"; return; }

    e->is_auth = true;
    e->authed = false;
    random_bytes(e->nonce, sizeof(e->nonce));
    strncpy(e->auth_uname, req->uname, sizeof(e->auth_uname) - 1);
    strncpy(e->auth_aname, req->aname, sizeof(e->auth_aname) - 1);
    e->qid.type = P9_QTAUTH;
    e->qid.vers = 0;
    e->qid.path = p9_path_hash("#auth");

    resp->type = P9_RAUTH;
    resp->qid = e->qid;
}

/* The client's response, written to the auth fid: HMAC-SHA256 over the nonce
 * the server chose and the identity being claimed. Binding uname and aname
 * into the MAC is what stops a response captured for one identity from being
 * replayed as another on the same nonce. */
static void p9_auth_expected(const p9_fid_entry_t *e, const uint8_t *key,
                             uint32_t key_len, uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t msg[SHA256_DIGEST_LEN + 2u * P9_MAX_NAME_LEN];
    uint32_t n = 0;
    for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) msg[n++] = e->nonce[i];
    for (const char *p = e->auth_uname; *p; p++) msg[n++] = (uint8_t)*p;
    for (const char *p = e->auth_aname; *p; p++) msg[n++] = (uint8_t)*p;
    hmac_sha256(key, key_len, msg, n, out);
}

/* --- Self-test (N2) ---------------------------------------------------- */

int p9_auth_selftest(void) {
    int failures = 0;
    cprintf("9P auth gate selftest:\n");

    /* The guard. Every one of these is a path a client could ask for. */
    {
        static const struct { const char *path; bool secret; } CASES[] = {
            { P9_AUTH_KEY_DIR,                    true  },  /* the directory itself */
            { P9_AUTH_KEYS_FILE,                  true  },
            { P9_AUTH_KEY_DIR "/anything",        true  },
            { P9_AUTH_KEY_DIR "/deeper/still",    true  },
            { P9_AUTH_FALLBACK_KEY_FILE,          true  },  /* the single-key form */
            { "/sd0/system/etc",                  false },  /* the parent is fine */
            { "/sd0/system/etc/authorized",       false },  /* prefix, not a component */
            { "/sd0/system/etc/auth.txt",         false },
            { "/proc/version",                    false },
            { "/",                                false },
            /* I3, §4: the identity store joined this same guard --
             * idstore_selftest() already checks idstore_path_is_secret()
             * directly (pure string logic, no server needed); this line is
             * what "refused by the SAME guard" means in practice, exercised
             * through the one function every transport calls. */
            { "/dev/identity0",                   true  },
        };
        bool ok = true;
        for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
            if (p9_auth_path_is_secret(CASES[i].path) != CASES[i].secret) {
                cprintf("        %s: expected %s\n", CASES[i].path,
                        CASES[i].secret ? "refused" : "servable");
                ok = false;
            }
        }
        cprintf("  [%s] the key store and the identity store are both refused, and only those\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* The MAC binds the identity. Same key, same nonce, different uname must
     * give a different response -- otherwise a recording made for one
     * identity would authenticate another. */
    {
        p9_fid_entry_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        for (unsigned i = 0; i < SHA256_DIGEST_LEN; i++) { a.nonce[i] = (uint8_t)i; b.nonce[i] = (uint8_t)i; }
        strncpy(a.auth_uname, "alice", sizeof(a.auth_uname) - 1);
        strncpy(b.auth_uname, "bob",   sizeof(b.auth_uname) - 1);

        const uint8_t key[4] = { 1, 2, 3, 4 };
        uint8_t ma[SHA256_DIGEST_LEN], mb[SHA256_DIGEST_LEN];
        p9_auth_expected(&a, key, sizeof(key), ma);
        p9_auth_expected(&b, key, sizeof(key), mb);
        bool ok = !sha256_verify(ma, mb, sizeof(ma));

        /* ...and the same identity on a different nonce must differ too,
         * which is what makes a captured response worthless next time. */
        p9_fid_entry_t c = a;
        c.nonce[0] ^= 0xFF;
        uint8_t mc[SHA256_DIGEST_LEN];
        p9_auth_expected(&c, key, sizeof(key), mc);
        ok = ok && !sha256_verify(ma, mc, sizeof(ma));

        /* ...while the same inputs are of course stable. */
        uint8_t ma2[SHA256_DIGEST_LEN];
        p9_auth_expected(&a, key, sizeof(key), ma2);
        ok = ok && sha256_verify(ma, ma2, sizeof(ma));

        cprintf("  [%s] the response MAC binds both the nonce and the identity\n",
                ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    /* A different key must not produce the same response. */
    {
        p9_fid_entry_t a;
        memset(&a, 0, sizeof(a));
        strncpy(a.auth_uname, "alice", sizeof(a.auth_uname) - 1);
        const uint8_t k1[4] = { 1, 2, 3, 4 };
        const uint8_t k2[4] = { 1, 2, 3, 5 };
        uint8_t m1[SHA256_DIGEST_LEN], m2[SHA256_DIGEST_LEN];
        p9_auth_expected(&a, k1, sizeof(k1), m1);
        p9_auth_expected(&a, k2, sizeof(k2), m2);
        bool ok = !sha256_verify(m1, m2, sizeof(m1));
        cprintf("  [%s] a one-bit key difference changes the response\n", ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    if (failures == 0) cprintf("P9AUTH_SELFTEST_OK (3/3)\n");
    else               cprintf("P9AUTH_SELFTEST_FAIL (%d failed)\n", failures);
    return failures;
}

/* --- Message handlers --- */

static void p9_handle_tattach(const p9_msg_t *req, p9_msg_t *resp) {
    /* The gate. On a transport that requires it, an attach is only as good as
     * the afid it names: the afid must exist, be an auth fid, have had a
     * verified response written to it, and have been established for the same
     * identity now being claimed. */
    if (g_auth_policy == P9_AUTH_REQUIRED) {
        if (req->afid == P9_NOFID) {
            resp->type = P9_RERROR;
            resp->ename = "attach: this link requires authentication (no afid)";
            return;
        }
        p9_fid_entry_t *a = p9_fid_lookup(req->afid);
        if (!a || !a->is_auth) {
            resp->type = P9_RERROR; resp->ename = "attach: afid is not an auth fid"; return;
        }
        if (!a->authed) {
            resp->type = P9_RERROR; resp->ename = "attach: afid has not authenticated"; return;
        }
        if (strncmp(a->auth_uname, req->uname, sizeof(a->auth_uname)) != 0 ||
            strncmp(a->auth_aname, req->aname, sizeof(a->auth_aname)) != 0) {
            resp->type = P9_RERROR;
            resp->ename = "attach: afid was authenticated for a different uname/aname";
            return;
        }
    }

    p9_fid_entry_t *e = p9_fid_alloc(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "attach: fid table full or fid in use"; return; }

    char path[128];
    /* aname names a path, so the key store has to be refused here as well --
     * walking is not the only way to reach a directory. */
    if (p9_path_is_secret(req->aname)) {
        resp->type = P9_RERROR; resp->ename = "attach: no such tree"; return;
    }
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
    if (src->is_auth) { resp->type = P9_RERROR; resp->ename = "walk: fid is an auth fid"; return; }

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
        /* The key store is not walkable, by anyone, over any transport --
         * refused exactly like a path that is not there, so that a client
         * cannot even learn whether keys are kept here. See
         * p9_path_is_secret(). */
        if (p9_path_is_secret(next)) {
            if (i == 0) { resp->type = P9_RERROR; resp->ename = "walk: no such file or directory"; return; }
            break;
        }
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
    {
        p9_fid_entry_t *a = p9_fid_lookup(req->fid);
        if (a && a->is_auth) {
            resp->type = P9_RERROR; resp->ename = "open: fid is an auth fid"; return;
        }
    }
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

    /* An auth fid is read to collect the challenge, and is never "opened" --
     * Tauth is what opened it. */
    if (e && e->is_auth) {
        uint32_t off = (req->offset > sizeof(e->nonce)) ? sizeof(e->nonce) : (uint32_t)req->offset;
        uint32_t avail = (uint32_t)sizeof(e->nonce) - off;
        uint32_t want = req->count;
        if (want > avail) want = avail;
        if (want > databuf_max) want = databuf_max;
        for (uint32_t i = 0; i < want; i++) databuf[i] = e->nonce[off + i];
        resp->type = P9_RREAD;
        resp->count = want;
        resp->data = databuf;
        return;
    }

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

    /* The response to the challenge. */
    if (e && e->is_auth) {
        uint8_t key[P9_AUTH_KEY_MAX];
        uint32_t key_len = 0;
        if (req->count != SHA256_DIGEST_LEN) {
            resp->type = P9_RERROR; resp->ename = "auth: response must be 32 bytes"; return;
        }
        if (p9_auth_key_for(e->auth_uname, key, &key_len) != 0) {
            resp->type = P9_RERROR; resp->ename = "auth: no key for that uname"; return;
        }

        uint8_t expect[SHA256_DIGEST_LEN];
        p9_auth_expected(e, key, key_len, expect);
        memset(key, 0, sizeof(key));

        /* Constant-time, and not memcmp(): see kernel/sha256.h. */
        bool ok = sha256_verify(expect, req->data, SHA256_DIGEST_LEN);
        memset(expect, 0, sizeof(expect));

        if (!ok) {
            /* The nonce is burned either way. Without this, a client could
             * sit on one challenge and grind guesses against it; with it,
             * every attempt costs a fresh Tauth and a fresh nonce. */
            random_bytes(e->nonce, sizeof(e->nonce));
            resp->type = P9_RERROR; resp->ename = "auth: response rejected"; return;
        }

        e->authed = true;
        resp->type = P9_RWRITE;
        resp->count = req->count;
        return;
    }

    if (!e || !e->is_open || e->is_dir) { resp->type = P9_RERROR; resp->ename = "write: fid not open for writing"; return; }

    int n = vfs_pwrite(e->vfs_fd, req->data, req->count, req->offset);
    if (n < 0) { resp->type = P9_RERROR; resp->ename = "write: I/O error"; return; }

    resp->type = P9_RWRITE;
    resp->count = (uint32_t)n;
}

static void p9_handle_tstat(const p9_msg_t *req, p9_msg_t *resp, uint8_t *databuf, uint32_t databuf_max) {
    p9_fid_entry_t *e = p9_fid_lookup(req->fid);
    if (!e) { resp->type = P9_RERROR; resp->ename = "stat: unknown fid"; return; }
    /* An auth fid has no path, so this would otherwise stat "" and report
     * whatever that resolved to. It is not a file and does not get to look
     * like one. */
    if (e->is_auth) { resp->type = P9_RERROR; resp->ename = "stat: fid is an auth fid"; return; }

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
            wcur_u32(&c, msg->afid);   // P9_NOFID when this attach is unauthenticated
            wcur_str(&c, msg->uname);
            wcur_str(&c, msg->aname);
            break;
        case P9_TAUTH:
            wcur_u32(&c, msg->afid);
            wcur_str(&c, msg->uname);
            wcur_str(&c, msg->aname);
            break;
        case P9_RAUTH:
            wcur_qid(&c, &msg->qid);
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
            /* iounit: the most one Tread/Twrite can move on this
             * connection. Derived from what Tversion actually agreed, not
             * assumed -- a hardcoded 4096 here was a promise the transport
             * could not keep even at the old default msize of 4096, since
             * the reply framing needs room too (see P9_IOHDRSZ). */
            wcur_u32(&c, p9_negotiated_iounit());
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
            /* Kept, not discarded: which afid the client is attaching under is
             * the whole question when the link requires authentication. */
            if (ok) ok = rcur_u32(&c, &msg->afid);
            if (ok) ok = rcur_str(&c, msg->uname, sizeof(msg->uname));
            if (ok) ok = rcur_str(&c, msg->aname, sizeof(msg->aname));
            break;
        }
        case P9_TAUTH: {
            ok = rcur_u32(&c, &msg->afid);
            if (ok) ok = rcur_str(&c, msg->uname, sizeof(msg->uname));
            if (ok) ok = rcur_str(&c, msg->aname, sizeof(msg->aname));
            break;
        }
        case P9_RAUTH:
            ok = rcur_qid(&c, &msg->qid);
            break;
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

/* The msize agreed with the current peer, as answered in the last Rversion.
 *
 * One global, matching the connection model this server already has: the fid
 * table (g_fid_table) is global too, and Tversion resets it, so "the
 * connection" is singular here by construction. If this server ever serves
 * two peers at once, this and the fid table become per-connection together.
 *
 * Starts at this node's own maximum so a peer that skips Tversion -- which
 * is a protocol error, but should not produce a nonsense iounit -- is
 * answered with something legal. */
static uint32_t g_negotiated_msize = P9_MAX_MSIZE;

/* What an Ropen/Rcreate may advertise as its iounit: the largest payload one
 * Tread/Twrite can carry within the agreed msize. Never larger than the
 * connection can actually deliver -- see P9_IOHDRSZ. */
uint32_t p9_negotiated_iounit(void) {
    return (g_negotiated_msize > P9_IOHDRSZ) ? (g_negotiated_msize - P9_IOHDRSZ) : 0;
}

int p9_server_process(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf,
                      uint32_t resp_max, p9_auth_policy_t policy) {
    g_auth_policy = policy;
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
            /* Remember what was agreed. It used to be computed here, sent,
             * and forgotten, which left p9_serialize() with nothing to
             * derive an honest iounit from -- so it hardcoded one. */
            g_negotiated_msize = resp.msize;
            break;
        case P9_TAUTH:
            p9_handle_tauth(&req, &resp);
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
