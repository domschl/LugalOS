#include "fs/9p.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sha256.h"
#include "kernel/random.h"
#include "kernel/idstore.h"
#include "kernel/identity.h"
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

    /* I5, §5.2: set at Tattach time from the grant that matched this fid's
     * uname (false -- unrestricted -- when no grant matched, e.g. the
     * console key or the flash fallback, both of which predate grants).
     * Twalk propagates it to every fid descended from this one; every
     * mutating handler (Topen for write, Tcreate, Twrite, Tremove) refuses
     * when it is set. */
    bool read_only;

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

/* --- I5: the grants list (§5.2) ---------------------------------------
 *
 * Format of P9_AUTH_KEYS_FILE, 4 columns: `name`, hex key, `aname` (the
 * subtree this peer may attach at), `mode` ("ro" or "rw"). Whitespace-
 * separated, one entry per line; blank lines and lines beginning with '#'
 * are ignored. The last two columns are optional -- a line with only a
 * name and a key (phase 18's original 2-column format) reads as
 * aname="/", read_only=false: unrestricted, exactly what a bare "uname
 * hexkey" line has always meant. A line can be deleted to revoke an
 * identity, which is the whole reason this is a list rather than one
 * secret -- and post-I5, deleting it takes effect on the very next
 * lookup, since nothing here is ever cached.
 *
 * P9_AUTH_FALLBACK_KEY_FILE is unrelated to grants: a bare hex key with no
 * uname, for a board with no card. It answers for any uname with no
 * restriction, predating I5 and untouched by it. */

static bool parse_grant_line(const char *line, const char *line_end, p9_grant_t *g) {
    while (line < line_end && (*line == ' ' || *line == '\t' || *line == '\r')) line++;
    if (line >= line_end || *line == '#') return false;

    memset(g, 0, sizeof(*g));

    /* name */
    const char *ne = line;
    while (ne < line_end && *ne != ' ' && *ne != '\t' && *ne != '\r') ne++;
    uint32_t nlen = (uint32_t)(ne - line);
    if (nlen == 0) return false;
    if (nlen >= sizeof(g->name)) nlen = sizeof(g->name) - 1;
    memcpy(g->name, line, nlen);
    g->name[nlen] = '\0';

    /* hex key */
    const char *k = ne;
    while (k < line_end && (*k == ' ' || *k == '\t')) k++;
    uint32_t klen = 0;
    while (k + 1 < line_end && klen < P9_AUTH_KEY_MAX) {
        int hi = hexval(k[0]), lo = hexval(k[1]);
        if (hi < 0 || lo < 0) break;
        g->key[klen++] = (uint8_t)((hi << 4) | lo);
        k += 2;
    }
    if (klen == 0) return false;
    g->key_len = klen;

    /* aname, defaulting to "/" -- unrestricted. */
    while (k < line_end && (*k == ' ' || *k == '\t')) k++;
    const char *ae = k;
    while (ae < line_end && *ae != ' ' && *ae != '\t' && *ae != '\r') ae++;
    uint32_t alen = (uint32_t)(ae - k);
    if (alen == 0) {
        g->aname[0] = '/';
    } else {
        if (alen >= sizeof(g->aname)) alen = sizeof(g->aname) - 1;
        memcpy(g->aname, k, alen);
        g->aname[alen] = '\0';
    }

    /* mode: only an exact "ro" sets read-only. Absent, "rw", or anything
     * else that isn't "ro" reads as rw -- the same fail-open-to-what-a-
     * missing-column-already-meant shape every other optional piece of
     * this line gets. This file is written by the toolset
     * (kernel/shell.c's `peers add`, which always spells the word exactly)
     * -- a hand-edited typo is an admin's own mistake, on a medium the
     * threat model (§7) already assumes physical access to. */
    const char *m = ae;
    while (m < line_end && (*m == ' ' || *m == '\t')) m++;
    g->read_only = (line_end - m >= 2 && m[0] == 'r' && m[1] == 'o');

    return true;
}

/* Grants list read buffer: sized for P9_GRANTS_MAX lines at their longest
 * -- name(31) + ' ' + hex key(128) + ' ' + aname(31) + ' ' + mode(2) +
 * '\n', times 8, comfortably inside 2048. The pre-I5 512-byte buffer this
 * replaces could not actually hold 8 max-length entries either; it was
 * never exercised past ~3 before I5 needed the headroom for real. */
#define P9_GRANTS_BUF_MAX 2048

/* The grant that answers for `uname`: an exact name match, or (absent one)
 * the wildcard "*" row -- the same resolution key lookup has always used.
 * Returns false if the file is missing/empty or nothing matches. */
static bool p9_grants_find(const char *uname, p9_grant_t *out) {
    /* Through p9_grants_list(), so there is exactly one piece of code that
     * knows where this board keeps its grants.
     *
     * This used to read P9_AUTH_KEYS_FILE itself, with its own copy of the
     * parse loop below. That was harmless while the file was the only
     * storage and actively dangerous the moment it stopped being: adding the
     * identity-record backend redirected p9_grants_list() and
     * p9_grants_add(), and left *this* -- the reader that actually authorises
     * an attach -- still looking at an empty file. `peers` listed the grant,
     * `p9auth` said keys were configured, and every attach was refused, which
     * is about as misleading as a failure gets. */
    p9_grant_t entries[P9_GRANTS_MAX];
    uint32_t count = p9_grants_list(entries, P9_GRANTS_MAX);

    bool have_wildcard = false;
    p9_grant_t wildcard;
    for (uint32_t i = 0; i < count; i++) {
        p9_grant_t g = entries[i];

        /* `*` matches any uname. Explicit, never implied: since R3's
         * identity work every node attaches under its own name
         * ("rp2350-gateway-3f2a"), which is what makes phase 18 §6's
         * "multiple keys identify who" real -- and which also means a
         * keys file written when every node was "lugal" stops matching.
         * A segment that genuinely shares one key writes one line and
         * says so; nothing falls back silently, because a silent
         * fallback would quietly undo the identification this exists
         * for. */
        if (strcmp(g.name, "*") == 0) { wildcard = g; have_wildcard = true; continue; }
        if (strcmp(g.name, uname) == 0) { *out = g; return true; }
    }
    if (have_wildcard) { *out = wildcard; return true; }
    return false;
}

static p9_grant_result_t p9_grants_store(const char *text, uint32_t len);

/* The record-backed grants, cached in RAM.
 *
 * Not an optimisation. `idstore_read()` goes to a block device, and the 9P
 * server task cannot do that -- an attach arriving on it found
 * p9_auth_have_keys() reading the record, getting nothing, and refusing a
 * peer whose grant was sitting there correctly (found by the I4 test, which
 * is the only one that gives a node both an identity disk and a grant).
 *
 * Caching is the right answer rather than a workaround, because an *attach*
 * doing disk I/O is wrong on its own terms: it is on the latency path and it
 * would re-read the same sector for every peer that ever connects. So the
 * record is read once, from the boot task where that is safe, and written
 * through on every change.
 *
 * The file path is deliberately left uncached: it already worked from any
 * task, /sd0 is where an administrator may edit grants by hand behind the
 * system's back, and a cache would start ignoring them. */
static char     g_grants_cache[NODE_GRANTS_MAX];
static uint32_t g_grants_cache_len;
static bool     g_grants_cached;

/* Reads the record's grants into the cache. Call from the boot task only.
 * Absence of a record, or of the field, is not an error -- it means this
 * board keeps its grants in a file, and p9_grants_list() falls through. */
void p9_grants_cache_load(void) {
    uint32_t n = 0;
    if (node_grants(g_grants_cache, sizeof(g_grants_cache), &n) && n > 0) {
        g_grants_cache_len = n;
        g_grants_cached = true;
    } else {
        g_grants_cache_len = 0;
        g_grants_cached = false;
    }
}

uint32_t p9_grants_list(p9_grant_t *out, uint32_t cap) {
    char buf[P9_GRANTS_BUF_MAX];

    /* The identity record first, then the file.
     *
     * Not a preference between two equal homes: a board with no SD card had
     * nowhere to keep a grant at all, so it refused every authenticated
     * attach forever with no way to authorise anyone -- networked 9P simply
     * could not work on it. The record is per-board, survives a reflash, and
     * is already the place phase 21 §5.2 argued grants belong.
     *
     * The file stays, and stays first-class, because a gateway with a card
     * can be administered by editing a text file, which is worth keeping. The
     * record wins where both exist: it is the more specific statement about
     * *this* board, and a card moved between boards should not silently carry
     * someone else's grants with it.
     *
     * Same bytes either way -- this is a storage decision, not a format one,
     * and the parser below does not know which it got. */
    int n;
    if (g_grants_cached && g_grants_cache_len > 0) {
        n = (int)(g_grants_cache_len < sizeof(buf) ? g_grants_cache_len : sizeof(buf));
        memcpy(buf, g_grants_cache, (size_t)n);
    } else {
        n = p9_read_small_file(P9_AUTH_KEYS_FILE, buf, sizeof(buf));
    }
    if (n <= 0) return 0;

    uint32_t count = 0;
    const char *p = buf;
    const char *end = buf + n;
    while (p < end && count < P9_GRANTS_MAX && count < cap) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        const char *line_end = p;
        if (p < end) p++;

        p9_grant_t g;
        if (!parse_grant_line(line, line_end, &g)) continue;
        out[count++] = g;
    }
    return count;
}

/* mkdir on each ancestor of P9_AUTH_KEY_DIR, ignoring failures -- an
 * "already exists" and a genuine failure look the same from here, and the
 * write that follows reports the genuine kind honestly either way. This is
 * the first code that ever creates this directory rather than assuming an
 * operator already put it on the card by hand. */
static void p9_auth_ensure_key_dir(void) {
    vfs_mkdir("/sd0/system");
    vfs_mkdir("/sd0/system/etc");
    vfs_mkdir(P9_AUTH_KEY_DIR);
}

static uint32_t p9_grant_serialize(const p9_grant_t *entries, uint32_t count, char *out, uint32_t cap) {
    static const char hex[] = "0123456789abcdef";
    uint32_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t need = (uint32_t)strlen(entries[i].name) + 1 + entries[i].key_len * 2 + 1 +
                        (uint32_t)strlen(entries[i].aname) + 1 + 2 + 1;
        if (used + need > cap) break; /* would not fit; truncate rather than overrun */

        for (const char *s = entries[i].name; *s; s++) out[used++] = *s;
        out[used++] = ' ';
        for (uint32_t b = 0; b < entries[i].key_len; b++) {
            out[used++] = hex[entries[i].key[b] >> 4];
            out[used++] = hex[entries[i].key[b] & 0x0f];
        }
        out[used++] = ' ';
        for (const char *s = entries[i].aname; *s; s++) out[used++] = *s;
        out[used++] = ' ';
        out[used++] = 'r';
        out[used++] = entries[i].read_only ? 'o' : 'w';
        out[used++] = '\n';
    }
    return used;
}

p9_grant_result_t p9_grants_add(const char *name, const uint8_t *key, uint32_t key_len,
                                const char *aname, bool read_only) {
    if (!name || !name[0] || strlen(name) >= P9_MAX_NAME_LEN) return P9_GRANT_ERR_BAD_INPUT;
    if (!key || key_len == 0 || key_len > P9_AUTH_KEY_MAX) return P9_GRANT_ERR_BAD_INPUT;
    if (aname && aname[0] && (strlen(aname) >= P9_MAX_NAME_LEN || aname[0] != '/')) return P9_GRANT_ERR_BAD_INPUT;

    p9_grant_t entries[P9_GRANTS_MAX];
    uint32_t count = p9_grants_list(entries, P9_GRANTS_MAX);

    int replace_idx = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) { replace_idx = (int)i; break; }
    }
    if (replace_idx < 0 && count >= P9_GRANTS_MAX) return P9_GRANT_ERR_FULL;

    p9_grant_t g;
    memset(&g, 0, sizeof(g));
    strncpy(g.name, name, sizeof(g.name) - 1);
    memcpy(g.key, key, key_len);
    g.key_len = key_len;
    if (aname && aname[0]) strncpy(g.aname, aname, sizeof(g.aname) - 1);
    else g.aname[0] = '/';
    g.read_only = read_only;

    if (replace_idx >= 0) entries[replace_idx] = g;
    else entries[count++] = g;

    char out[P9_GRANTS_BUF_MAX];
    uint32_t used = p9_grant_serialize(entries, count, out, sizeof(out));
    return p9_grants_store(out, used);
}

/* Writes the serialised list wherever this board keeps it: the identity
 * record when it has one, the file otherwise. Split out so add and remove
 * cannot disagree about where grants live -- which is exactly the bug that a
 * second copy of this decision would eventually become. */
static p9_grant_result_t p9_grants_store(const char *text, uint32_t len) {
    if (identity_store_device()) {
        node_id_result_t rc = node_identity_set_grants(text, len);
        if (rc == NODE_ID_OK) {
            /* Written through rather than invalidated: the next reader may be
             * the 9P server task, which cannot reload it itself. */
            g_grants_cache_len = len < sizeof(g_grants_cache) ? len : sizeof(g_grants_cache);
            memcpy(g_grants_cache, text, g_grants_cache_len);
            g_grants_cached = g_grants_cache_len > 0;
            return P9_GRANT_OK;
        }
        /* A board that HAS a store and could not be written is a failure, not
         * a reason to quietly write somewhere else: the next read would prefer
         * the record and find the stale list, so falling back here would make
         * a failed write look like a successful one. */
        return P9_GRANT_ERR_WRITE_FAILED;
    }
    p9_auth_ensure_key_dir();
    if (vfs_write(P9_AUTH_KEYS_FILE, text, len) != 0) return P9_GRANT_ERR_WRITE_FAILED;
    return P9_GRANT_OK;
}

p9_grant_result_t p9_grants_remove(const char *name) {
    if (!name || !name[0]) return P9_GRANT_ERR_BAD_INPUT;

    p9_grant_t entries[P9_GRANTS_MAX];
    uint32_t count = p9_grants_list(entries, P9_GRANTS_MAX);

    int found = -1;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) { found = (int)i; break; }
    }
    if (found < 0) return P9_GRANT_ERR_NOT_FOUND;
    for (uint32_t i = (uint32_t)found; i + 1 < count; i++) entries[i] = entries[i + 1];
    count--;

    char out[P9_GRANTS_BUF_MAX];
    uint32_t used = p9_grant_serialize(entries, count, out, sizeof(out));

    /* Same store the add path uses, and p9_grants_store() is what makes that
     * true by construction rather than by two functions agreeing. (Its file
     * branch does not call p9_auth_ensure_key_dir(): a remove that runs
     * before anything was ever added has nothing to remove, and
     * P9_GRANT_ERR_NOT_FOUND above already answered that case -- the add path
     * is where the directory gets created.) */
    return p9_grants_store(out, used);
}

const char *p9_grant_result_str(p9_grant_result_t rc) {
    switch (rc) {
        case P9_GRANT_OK:               return "ok";
        case P9_GRANT_ERR_FULL:         return "the grants list is full (8 entries) -- remove one first";
        case P9_GRANT_ERR_BAD_INPUT:    return "bad input (name, key, or aname)";
        case P9_GRANT_ERR_NOT_FOUND:    return "no such peer";
        case P9_GRANT_ERR_WRITE_FAILED: return "the write failed (no /sd0, or it is read-only)";
    }
    return "?";
}

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

int p9_auth_own_key(uint8_t *key_out, uint32_t *key_len) {
    /* Console key first, same bootstrap-override argument as always. */
    if (g_console_key_len > 0) {
        memcpy(key_out, g_console_key, g_console_key_len);
        *key_len = g_console_key_len;
        return 0;
    }
    /* I4: the identity record. This is the only other place this node's
     * own key lives -- deliberately never the grants list or the flash
     * fallback below, which as of I5 answer a different question ("who
     * may attach to me"), not this one. */
    uint8_t rec_key[NODE_DEVKEY_MAX];
    uint32_t rec_key_len = 0;
    if (node_devkey(rec_key, sizeof(rec_key), &rec_key_len) && rec_key_len > 0) {
        memcpy(key_out, rec_key, rec_key_len);
        *key_len = rec_key_len;
        memset(rec_key, 0, sizeof(rec_key));
        return 0;
    }
    memset(rec_key, 0, sizeof(rec_key));
    return -1;
}

int p9_auth_key_for(const char *uname, uint8_t *key_out, uint32_t *key_len) {
    /* Console key first: it is the bootstrap and the override, and a gateway
     * being repaired should not have to fight its own card. */
    if (g_console_key_len > 0) {
        memcpy(key_out, g_console_key, g_console_key_len);
        *key_len = g_console_key_len;
        return 0;
    }

    /* I5: the grants list -- what `uname` must present to attach to ME.
     * Deliberately does NOT consult this node's own identity-record key:
     * that answers p9_auth_own_key()'s question, not this one, and
     * conflating them is exactly what §1.2 says breaks once this list
     * carries scope (aname/mode) rather than just identity. */
    p9_grant_t g;
    if (p9_grants_find(uname, &g)) {
        memcpy(key_out, g.key, g.key_len);
        *key_len = g.key_len;
        return 0;
    }

    char buf[512];
    int n = p9_read_small_file(P9_AUTH_FALLBACK_KEY_FILE, buf, sizeof(buf));
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
    /* Whatever p9_auth_key_for() (the *server-side*, verify-an-incoming-
     * peer ladder) would actually consult -- console key, the grants list,
     * the flash fallback. Deliberately not node_devkey(): since I5, this
     * node's own record key answers p9_auth_own_key()'s question, not
     * this one, and checking it here would report "yes" on a node no
     * incoming peer could ever actually attach to. That was I4's mistake,
     * corrected the moment I5 gave p9_auth_key_for() somewhere else to
     * look instead. */
    if (g_console_key_len > 0) return true;
    /* The record before the files, matching p9_grants_list()'s own order.
     *
     * Missing this is not a cosmetic reporting bug: the caller at Tauth
     * refuses *every* attach when this returns false, so a board whose only
     * grants live in its identity record would answer "authentication
     * required" and then reject the correct key -- the exact failure
     * IDSTORE_FIELD_GRANTS exists to remove, reintroduced one function
     * further along. Caught by the I4 test on the first run after the field
     * landed. */
    /* The cache directly, not p9_grants_list(): that parses into a 2 KB
     * stack buffer, and this runs on the 9P server task inside a Tauth. */
    if (g_grants_cached && g_grants_cache_len > 0) return true;
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

/* True if `path` is exactly `prefix`, or `prefix` plus a "/"-rooted
 * continuation -- the same component-boundary matching p9_path_is_secret()
 * uses, so "/sd0/pgn" matches "/sd0/pgn" and "/sd0/pgn/x" but not "/sd0" or
 * "/". A grant's aname of "/" matches everything: every absolute path this
 * server ever computes already starts with "/". */
static bool p9_path_within(const char *prefix, const char *path) {
    size_t n = strlen(prefix);
    if (n > 0 && prefix[n - 1] == '/') n--; /* tolerate a trailing slash in the grant */
    if (strncmp(path, prefix, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

static void p9_handle_tattach(const p9_msg_t *req, p9_msg_t *resp) {
    bool grant_matched = false;
    bool grant_read_only = false;

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

        /* I5, §5.2: does this uname's grant (if any) allow attaching here?
         * A key checked via the console or the flash fallback -- neither
         * of which predates grants -- has no entry here and is
         * unrestricted, matching phase 18's original behaviour exactly. A
         * verified identity with no matching grant line, on a link that
         * DOES have grants configured for other names, still gets no
         * restriction: §5.2 describes an allow-list of *scopes*, not a
         * second authentication step -- the Tauth/afid gate above already
         * decided whether this uname may attach at all. */
        p9_grant_t g;
        if (p9_grants_find(req->uname, &g)) {
            grant_matched = true;
            grant_read_only = g.read_only;
            char granted[128];
            if (g.aname[0] == '\0' || strcmp(g.aname, "/") == 0) {
                strncpy(granted, "/", sizeof(granted) - 1);
            } else if (g.aname[0] == '/') {
                strncpy(granted, g.aname, sizeof(granted) - 1);
            } else {
                granted[0] = '/';
                strncpy(granted + 1, g.aname, sizeof(granted) - 2);
            }
            granted[sizeof(granted) - 1] = '\0';

            char requested[128];
            if (req->aname[0] == '\0' || strcmp(req->aname, "/") == 0) {
                strncpy(requested, "/", sizeof(requested) - 1);
            } else if (req->aname[0] == '/') {
                strncpy(requested, req->aname, sizeof(requested) - 1);
            } else {
                requested[0] = '/';
                strncpy(requested + 1, req->aname, sizeof(requested) - 2);
            }
            requested[sizeof(requested) - 1] = '\0';

            if (!p9_path_within(granted, requested)) {
                resp->type = P9_RERROR;
                resp->ename = "attach: not granted at this aname";
                return;
            }
        }
    }

    /* aname names a path, so the key store has to be refused here as well --
     * walking is not the only way to reach a directory. Checked before
     * allocating a fid, like the grant check above: a refused attach must
     * not cost a slot in an 8-entry table. */
    if (p9_path_is_secret(req->aname)) {
        resp->type = P9_RERROR; resp->ename = "attach: no such tree"; return;
    }

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
    e->read_only = grant_matched && grant_read_only;
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
            dst->read_only = src->read_only;  /* I5: a grant's mode follows every fid descended from it */
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
    dst->read_only = src->read_only;  /* I5: a grant's mode follows every fid descended from it */

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

    /* I5: ORCLOSE is a delete-on-clunk request regardless of whether this
     * is a file or a directory, so it is checked once, ahead of the
     * is_dir split below rather than duplicated into both branches. */
    if (e->read_only && (req->mode & P9_ORCLOSE)) {
        resp->type = P9_RERROR; resp->ename = "open: this peer is granted read-only access"; return;
    }

    if (!e->is_dir) {
        int flags;
        uint8_t access = req->mode & 0x03;
        if (access == P9_OWRITE) flags = VFS_O_WRITE;
        else if (access == P9_ORDWR || access == P9_OEXEC) flags = VFS_O_READ | VFS_O_WRITE;
        else flags = VFS_O_READ;
        if (req->mode & P9_OTRUNC) flags |= VFS_O_TRUNC;

        /* I5, §5.2: "a read-only mode is a flag consulted by the write
         * paths." Checked here as well as at Twrite so a `ro` grant fails
         * at open time -- the same point vfs_open()'s own mount-level
         * read_only check already fails at, for the same reason. */
        if (e->read_only && (flags & (VFS_O_WRITE | VFS_O_TRUNC))) {
            resp->type = P9_RERROR; resp->ename = "open: this peer is granted read-only access"; return;
        }

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
    if (e->read_only) {
        resp->type = P9_RERROR; resp->ename = "create: this peer is granted read-only access"; return;
    }

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

        /* FAT32 stores "." and ".." as real on-disk entries in every
         * subdirectory (not in a volume root), and vfs_readdir() reports
         * them faithfully -- which is right for the shell's own `ls`, but
         * wrong on the wire: a 9P2000 directory read carries a directory's
         * *contents*, and a client reaches the parent by walking the name
         * "..", never by finding it in a listing. Leaking them made every
         * 9P client's tree walk self-referential (fuse-p9's `ls -la` showed
         * "." and ".." twice, once from the server and once from the pair
         * FUSE prepends itself; a recursive p9lib walk would never
         * terminate). Skipped here rather than in vfs_readdir() so the
         * console `ls` keeps showing what is actually on the card, and
         * skipped rather than stopped -- the cursor still has to advance
         * past them to reach the entries behind. Both names stay walkable;
         * only the listing drops them. */
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            e->dir_read_index++;
            continue;
        }

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
    /* I5, §5.2: the concrete case its own verify list names -- "a peer
     * granted `ro` is refused a Twrite and told why." Topen already
     * refuses to open a file for writing under a `ro` grant, so this is
     * reached only if a client walks straight to Twrite without going
     * through Topen's own gate -- still refused, same reason either way. */
    if (e->read_only) {
        resp->type = P9_RERROR; resp->ename = "write: this peer is granted read-only access"; return;
    }

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

    /* I5: a `ro` grant refuses the remove itself, but the fid is still
     * clunked below -- per spec, and per this function's own existing
     * comment on that, unaffected by why the remove failed. */
    if (e->read_only) {
        p9_fid_release(e);
        resp->type = P9_RERROR; resp->ename = "remove: this peer is granted read-only access"; return;
    }

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
