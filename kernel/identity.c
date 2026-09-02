#include "kernel/identity.h"
#include "kernel/sha256.h"
#include "kernel/idstore.h"
#include "kernel/scratch.h"
#include "kernel/random.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#include <string.h>

static char     g_name[NODE_NAME_MAX];
static uint8_t  g_mac[NODE_MAC_LEN];
static const char *g_name_source = "unset";
static const char *g_mac_source  = "unset";
static bool     g_mac_is_derived;

static uint8_t  g_uid[NODE_UID_LEN];
static bool     g_has_uid;
static const char *g_uid_source = "unset";

/* Weak, so a board can override it by simply defining the symbol. The default
 * is an honest "this silicon cannot identify itself", which is true of both
 * QEMU targets and -- until R4 wires the bootrom call and checks it against
 * two real boards -- of the RP2350 as well. */
__attribute__((weak)) bool board_unique_id(uint8_t out[8]) {
    (void)out;
    return false;
}

/* Weak, so a target that has one can plug in its identity store's block
 * device without this file having to know virtio-blk exists (I2,
 * plan/phase21_identity_and_authentication.md). QEMU's is
 * drivers/virtio_blk_id.c; the RP2350's reserved-flash-sector backend (I7)
 * overrides the same symbol. The default -- no backend at all -- is what
 * every board answers until one of those lands, or on any board persona
 * that never gets one. */
__attribute__((weak)) block_dev_t *identity_store_device(void) { return NULL; }

static char hex_digit(uint8_t v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

/* SHA-256 over whatever this node can say about itself: the silicon's own id
 * when it has one, the build seed when it does not, and the persona either
 * way. The seed alone would give two boards flashed from one build the same
 * identity, which is precisely the case a unique id exists to fix -- so the
 * unique id is mixed in first and dominates when present. */
static void identity_digest(uint8_t out[SHA256_DIGEST_LEN]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint8_t uid[8];
    if (board_unique_id(uid)) {
        sha256_update(&ctx, "uid:", 4);
        sha256_update(&ctx, uid, sizeof(uid));
    } else {
        sha256_update(&ctx, "seed:", 5);
        sha256_update(&ctx, CONFIG_NODE_SEED, (uint32_t)strlen(CONFIG_NODE_SEED));
    }
    sha256_update(&ctx, "/", 1);
    sha256_update(&ctx, CONFIG_NODE_PERSONA, (uint32_t)strlen(CONFIG_NODE_PERSONA));
    sha256_final(&ctx, out);
}

static void derive_mac(void) {
    uint8_t d[SHA256_DIGEST_LEN];
    uint8_t uid[8];
    bool from_silicon = board_unique_id(uid);
    identity_digest(d);
    g_mac[0] = 0x02;   /* locally administered, unicast -- see identity.h */
    g_mac[1] = 0x4C;   /* 'L' */
    g_mac[2] = 0x47;   /* 'G' */
    g_mac[3] = d[0];
    g_mac[4] = d[1];
    g_mac[5] = d[2];
    g_mac_is_derived = true;
    g_mac_source = from_silicon ? "derived (silicon id)" : "derived (build seed)";
}

/* "aa:bb:cc:dd:ee:ff" -> six bytes. Returns false on anything else, because a
 * board file with a typo in its MAC should fall back to a derived address
 * that works rather than to a malformed one that does not.
 *
 * Compiled only where it is called: no board pins a MAC today, and a function
 * nobody calls is a warning, which this tree does not keep. */
#ifdef CONFIG_NODE_MAC
static bool parse_mac(const char *s, uint8_t out[NODE_MAC_LEN]) {
    if (!s) return false;
    uint32_t n = 0;
    for (;;) {
        uint8_t hi, lo;
        if (s[0] >= '0' && s[0] <= '9') hi = (uint8_t)(s[0] - '0');
        else if (s[0] >= 'a' && s[0] <= 'f') hi = (uint8_t)(s[0] - 'a' + 10);
        else if (s[0] >= 'A' && s[0] <= 'F') hi = (uint8_t)(s[0] - 'A' + 10);
        else return false;
        if (s[1] >= '0' && s[1] <= '9') lo = (uint8_t)(s[1] - '0');
        else if (s[1] >= 'a' && s[1] <= 'f') lo = (uint8_t)(s[1] - 'a' + 10);
        else if (s[1] >= 'A' && s[1] <= 'F') lo = (uint8_t)(s[1] - 'A' + 10);
        else return false;
        if (n >= NODE_MAC_LEN) return false;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
        if (*s == '\0') break;
        if (*s != ':' && *s != '-') return false;
        s++;
    }
    if (n != NODE_MAC_LEN) return false;
    /* A multicast or broadcast source address is a subtler failure than no
     * address at all: frames leave, nothing comes back, and every register
     * reads correct. */
    return (out[0] & 0x01) == 0;
}
#endif

static void derive_name(void) {
    uint8_t d[SHA256_DIGEST_LEN];
    identity_digest(d);
    uint32_t n = 0;
    const char *p = CONFIG_NODE_PERSONA;
    /* Four hex digits of suffix, and the persona truncated to leave room for
     * them: a name that loses its distinguishing tail is worse than one that
     * loses a few characters of a prefix every node on the segment shares. */
    while (*p && n < NODE_NAME_MAX - 6) g_name[n++] = *p++;
    g_name[n++] = '-';
    g_name[n++] = hex_digit((uint8_t)(d[0] >> 4));
    g_name[n++] = hex_digit((uint8_t)(d[0] & 0x0f));
    g_name[n++] = hex_digit((uint8_t)(d[1] >> 4));
    g_name[n++] = hex_digit((uint8_t)(d[1] & 0x0f));
    g_name[n] = '\0';
    g_name_source = "derived";
}

void node_identity_init(void) {
    /* §4's resolution ladder, name half: provisioned record > board file >
     * derived from the build seed. The record is checked first precisely
     * because it is the only tier meant to survive a reflash -- a board file
     * is baked into the image, so it cannot outrank something that isn't.
     *
     * `rec` is local, not static: it holds the whole 4 KB record only for
     * the duration of this call, which is the one place anything needs it
     * until I3's toolset exists. A permanent static copy would cost every
     * target -- RP2350 included -- 4 KB of .bss for a value read exactly
     * once at boot; this way the cost is stack, and only while resolving.
     * Called before sched_init() (kernel/main.c), on the 16 KB boot stack
     * (linker/rp2350.ld), with nothing deeper than kernel_main() beneath it
     * -- comfortably inside that budget. */
    block_dev_t *id_dev = identity_store_device();
    idstore_t rec;
    bool rec_valid = id_dev && idstore_read(id_dev, &rec) == IDSTORE_VALID;

    char rec_name[NODE_NAME_MAX];
    int rec_name_len = rec_valid
        ? idstore_get_field(&rec, IDSTORE_FIELD_NAME, rec_name, sizeof(rec_name) - 1)
        : -1;

    if (rec_name_len > 0) {
        uint32_t n = (uint32_t)rec_name_len < NODE_NAME_MAX - 1 ? (uint32_t)rec_name_len : NODE_NAME_MAX - 1;
        memcpy(g_name, rec_name, n);
        g_name[n] = '\0';
        g_name_source = "record";
    }
#ifdef CONFIG_NODE_NAME
    else {
        strncpy(g_name, CONFIG_NODE_NAME, NODE_NAME_MAX - 1);
        g_name[NODE_NAME_MAX - 1] = '\0';
        g_name_source = "board file";
        if (g_name[0] == '\0') derive_name();
    }
#else
    else {
        derive_name();
    }
#endif

#ifdef CONFIG_NODE_MAC
    if (parse_mac(CONFIG_NODE_MAC, g_mac)) {
        g_mac_source = "board file";
        g_mac_is_derived = false;
    } else {
        printk("[Node] CONFIG_NODE_MAC is not a usable unicast address; deriving one.\n");
        derive_mac();
    }
#else
    derive_mac();
#endif

    /* The UID (§2's device scope): silicon, when this board can identify
     * itself, outranks a provisioned record -- §3.1 says the RP2350's OTP
     * chip id "needs no provisioning at all"; the record's own UID field is
     * for targets that have nothing to read, QEMU foremost among them. */
    if (board_unique_id(g_uid)) {
        g_has_uid = true;
        g_uid_source = "silicon";
    } else {
        uint8_t rec_uid[NODE_UID_LEN];
        if (rec_valid &&
            idstore_get_field(&rec, IDSTORE_FIELD_UID, rec_uid, sizeof(rec_uid)) == (int)sizeof(rec_uid)) {
            memcpy(g_uid, rec_uid, sizeof(g_uid));
            g_has_uid = true;
            g_uid_source = "record";
        } else {
            memset(g_uid, 0, sizeof(g_uid));
            g_has_uid = false;
            g_uid_source = "none (unprovisioned, no silicon id)";
        }
    }
}

const char *node_name(void) { return g_name[0] ? g_name : "lugal"; }
const uint8_t *node_mac(void) { return g_mac; }
const char *node_name_source(void) { return g_name_source; }
const char *node_mac_source(void) { return g_mac_source; }

bool node_uid(uint8_t out[NODE_UID_LEN]) {
    if (!g_has_uid) return false;
    memcpy(out, g_uid, NODE_UID_LEN);
    return true;
}
const char *node_uid_source(void) { return g_uid_source; }

/* Shared by node_set_name() and node_identity_rename_persistent() (I3):
 * the same string must be safe as a 9P uname, a hostname and a log line at
 * once -- letters, digits, dash and dot. */
static bool name_is_valid(const char *name, uint32_t *len_out) {
    if (!name || !name[0]) return false;
    uint32_t n = 0;
    while (name[n]) {
        char c = name[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!ok) return false;
        if (++n >= NODE_NAME_MAX) return false;
    }
    *len_out = n;
    return true;
}

int node_set_name(const char *name) {
    uint32_t n;
    if (!name_is_valid(name, &n)) return -1;
    memcpy(g_name, name, n);
    g_name[n] = '\0';
    g_name_source = "set at runtime";
    return 0;
}

/* --- I3, plan/phase21_identity_and_authentication.md: the toolset's data
 * layer -- read-modify-write onto the identity store, plus §5.1's key
 * validation. See kernel/include/kernel/identity.h for what each of these
 * is for; the reasoning lives there, next to the declarations. */

bool node_devkey(uint8_t *out, uint32_t cap, uint32_t *len_out) {
    block_dev_t *dev = identity_store_device();
    if (!dev) return false;
    idstore_t rec;
    if (idstore_read(dev, &rec) != IDSTORE_VALID) return false;

    uint8_t buf[NODE_DEVKEY_MAX];
    int n = idstore_get_field(&rec, IDSTORE_FIELD_DEVKEY, buf, sizeof(buf));
    if (n <= 0) return false;

    uint32_t take = (uint32_t)n < cap ? (uint32_t)n : cap;
    if (out && take) memcpy(out, buf, take);
    if (len_out) *len_out = (uint32_t)n;
    memset(buf, 0, sizeof(buf));
    return true;
}

/* One field per thing the identity record can hold; a NULL/zero-length
 * pointer means "carry forward whatever is already there". A struct
 * rather than a growing positional parameter list -- I6 is the third
 * caller to need "change one field, leave the rest alone", and a fourth
 * (WLAN) needing two fields at once (ssid+psk) is where a positional
 * signature stops being readable at the call site. */
typedef struct {
    const uint8_t *uid;                          /* NODE_UID_LEN bytes, or NULL */
    const char    *name;    uint32_t name_len;
    const uint8_t *key;     uint32_t key_len;
    const char    *ssid;    uint32_t ssid_len;
    const uint8_t *psk;     uint32_t psk_len;     /* NODE_WLAN_PSK_LEN bytes, or NULL */
    /* The IPv4 config, as one 12-byte blob. `clear_ipv4` is not the same as
     * leaving `ipv4` NULL: NULL means "carry whatever is on record forward",
     * which is the right default for every other setter, while clearing has
     * to be expressible or a stale address could only ever be overwritten,
     * never removed. */
    const uint8_t *ipv4;
    bool           clear_ipv4;
    /* Same NULL-means-carry-forward / explicit-clear pair as ipv4 above, and
     * for the same reason: withdrawing the last grant has to be expressible
     * or a list could only ever grow. */
    const char    *grants;  uint32_t grants_len;
    bool           clear_grants;
} identity_patch_t;

/* Reads whatever is currently on `dev` (if anything valid), then writes a
 * fresh record with every field in `patch` that is non-NULL overriding the
 * matching field and every other field carried forward unchanged -- so
 * setting the name does not silently drop an already-provisioned uid, key
 * or WLAN credential, and vice versa. Returns 0, or -1 if the device write
 * failed. Callers check identity_store_device() themselves first, so a
 * NULL device here would mean this file's own callers stopped agreeing
 * with each other, not a condition worth its own error path. */
static int identity_store_write(const identity_patch_t *patch) {
    block_dev_t *dev = identity_store_device();
    if (!dev) return -1;

    /* Both records come off the heap, not the stack.
     *
     * idstore_t and idstore_writer_t are a 4 KB buffer each, so holding the
     * old record and the new one at once is 8 KB in a single frame -- and
     * this is reached from `peers add`, which has already spent about 5 KB on
     * p9_grants_add()'s parse and serialise buffers. Thirteen kilobytes on a
     * shell stack that does not have it: the console simply stopped, with no
     * message, which is what a stack that runs off its end looks like from
     * outside. Found on hardware 2026-09-02, the first time a grant was added
     * on a board whose identity store is real flash.
     *
     * kernel/scratch.h is this tree's own answer to exactly this shape -- a
     * large working buffer that is not live at boot and not on a hot path.
     * Identity writes happen a handful of times in a board's life. */
    scratch_t sc;
    if (!scratch_acquire(&sc, sizeof(idstore_t) + sizeof(idstore_writer_t))) {
        printk("[Identity] not enough free heap to rewrite the record\n");
        return -1;
    }
    idstore_t        *oldp = (idstore_t *)sc.base;
    idstore_writer_t *wp   = (idstore_writer_t *)((uint8_t *)sc.base + sizeof(idstore_t));

    bool old_valid = idstore_read(dev, oldp) == IDSTORE_VALID;

    uint8_t final_uid[NODE_UID_LEN];
    bool    have_uid = false;
    if (patch->uid) {
        memcpy(final_uid, patch->uid, sizeof(final_uid));
        have_uid = true;
    } else if (old_valid) {
        have_uid = idstore_get_field(oldp, IDSTORE_FIELD_UID, final_uid, sizeof(final_uid)) == (int)sizeof(final_uid);
    }

    char     final_name[NODE_NAME_MAX];
    uint32_t final_name_len = 0;
    if (patch->name) {
        final_name_len = patch->name_len;
        memcpy(final_name, patch->name, final_name_len);
    } else if (old_valid) {
        int n = idstore_get_field(oldp, IDSTORE_FIELD_NAME, final_name, sizeof(final_name));
        if (n > 0) final_name_len = (uint32_t)n;
    }

    uint8_t  final_key[NODE_DEVKEY_MAX];
    uint32_t final_key_len = 0;
    if (patch->key) {
        final_key_len = patch->key_len;
        memcpy(final_key, patch->key, final_key_len);
    } else if (old_valid) {
        int n = idstore_get_field(oldp, IDSTORE_FIELD_DEVKEY, final_key, sizeof(final_key));
        if (n > 0) final_key_len = (uint32_t)n;
    }

    char     final_ssid[NODE_WLAN_SSID_MAX];
    uint32_t final_ssid_len = 0;
    if (patch->ssid) {
        final_ssid_len = patch->ssid_len;
        memcpy(final_ssid, patch->ssid, final_ssid_len);
    } else if (old_valid) {
        int n = idstore_get_field(oldp, IDSTORE_FIELD_WLAN_SSID, final_ssid, sizeof(final_ssid));
        if (n > 0) final_ssid_len = (uint32_t)n;
    }

    uint8_t  final_psk[NODE_WLAN_PSK_LEN];
    bool     have_psk = false;
    if (patch->psk) {
        memcpy(final_psk, patch->psk, sizeof(final_psk));
        have_psk = true;
    } else if (old_valid) {
        have_psk = idstore_get_field(oldp, IDSTORE_FIELD_WLAN_PSK, final_psk, sizeof(final_psk)) == (int)sizeof(final_psk);
    }

    uint8_t  final_ipv4[3 * NODE_IPV4_LEN];
    bool     have_ipv4 = false;
    if (patch->ipv4) {
        memcpy(final_ipv4, patch->ipv4, sizeof(final_ipv4));
        have_ipv4 = true;
    } else if (old_valid && !patch->clear_ipv4) {
        have_ipv4 = idstore_get_field(oldp, IDSTORE_FIELD_IPV4, final_ipv4,
                                      sizeof(final_ipv4)) == (int)sizeof(final_ipv4);
    }

    idstore_writer_init(wp);
    int rc = 0;
    if (have_uid)            rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_UID, final_uid, sizeof(final_uid));
    if (final_name_len > 0)  rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_NAME, final_name, (uint16_t)final_name_len);
    if (final_key_len > 0)   rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_DEVKEY, final_key, (uint16_t)final_key_len);
    if (final_ssid_len > 0)  rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_WLAN_SSID, final_ssid, (uint16_t)final_ssid_len);
    if (have_psk)            rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_WLAN_PSK, final_psk, sizeof(final_psk));
    /* Carried forward by pointer, not by copy. This is the one field big
     * enough that copying it on every unrelated write -- renaming the node,
     * storing a PSK -- would cost either a ~1.5 KB stack frame or a permanent
     * .bss buffer of that size, and on RP2350 .bss and the heap are the same
     * budget. `old` outlives the writer below, so the pointer is valid for
     * exactly as long as it is used. */
    const char *final_grants = NULL;
    uint32_t    final_grants_len = 0;
    if (patch->clear_grants) {
        final_grants_len = 0;
    } else if (patch->grants && patch->grants_len) {
        final_grants = patch->grants;
        final_grants_len = patch->grants_len;
    } else if (old_valid) {
        final_grants = (const char *)idstore_field_ptr(oldp, IDSTORE_FIELD_GRANTS,
                                                       &final_grants_len);
        if (!final_grants) final_grants_len = 0;
    }

    if (have_ipv4)           rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_IPV4, final_ipv4, sizeof(final_ipv4));
    if (final_grants_len)    rc |= idstore_writer_add_field(wp, IDSTORE_FIELD_GRANTS, final_grants, final_grants_len);
    memset(final_key, 0, sizeof(final_key));
    memset(final_psk, 0, sizeof(final_psk));

    int result = (rc != 0) ? -1 : idstore_writer_commit(wp, dev);
    scratch_release(&sc);
    return result;
}

node_id_result_t node_identity_provision(bool force) {
    block_dev_t *dev = identity_store_device();
    if (!dev) return NODE_ID_ERR_NO_BACKEND;

    idstore_t rec;
    if (idstore_read(dev, &rec) == IDSTORE_VALID && !force) return NODE_ID_ERR_POPULATED;

    uint8_t uid[NODE_UID_LEN];
    random_bytes(uid, sizeof(uid));  /* a public identifier: §5.1's entropy gate is about keys, not this */

    const char *name = node_name();
    uint32_t name_len = (uint32_t)strlen(name);
    if (name_len >= NODE_NAME_MAX) name_len = NODE_NAME_MAX - 1;

    identity_patch_t patch = { .uid = uid, .name = name, .name_len = name_len };
    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;

    /* Reflect immediately: a freshly provisioned node should not need a
     * reboot to report its own new uid/name as coming from the record. */
    node_identity_init();
    return NODE_ID_OK;
}

node_id_result_t node_identity_rename_persistent(const char *name) {
    uint32_t n;
    if (!name_is_valid(name, &n)) return NODE_ID_ERR_BAD_INPUT;
    if (!identity_store_device()) return NODE_ID_ERR_NO_BACKEND;
    identity_patch_t patch = { .name = name, .name_len = n };
    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;

    /* g_mac is untouched above and below -- a persisted rename keeps the
     * same non-obviousness guarantee node_set_name() already gives. */
    memcpy(g_name, name, n);
    g_name[n] = '\0';
    g_name_source = "record";
    return NODE_ID_OK;
}

/* §5.1's "trivially patterned": not full pattern detection, just the two
 * shapes a key typed by hand while distracted actually produces -- one
 * repeated byte value (all-zero included), or a straight +1/-1 run across
 * every byte. */
static bool key_looks_trivial(const uint8_t *key, uint32_t len) {
    if (len < 2) return true;

    bool all_same = true;
    for (uint32_t i = 1; i < len; i++) {
        if (key[i] != key[0]) { all_same = false; break; }
    }
    if (all_same) return true;

    bool ascending = true, descending = true;
    for (uint32_t i = 1; i < len; i++) {
        if ((uint8_t)(key[i - 1] + 1) != key[i]) ascending = false;
        if ((uint8_t)(key[i - 1] - 1) != key[i]) descending = false;
    }
    return ascending || descending;
}

node_id_result_t node_identity_set_key(const uint8_t *key, uint32_t key_len) {
    if (!key || key_len == 0 || key_len > NODE_DEVKEY_MAX) return NODE_ID_ERR_BAD_INPUT;
    if (key_looks_trivial(key, key_len)) return NODE_ID_ERR_BAD_INPUT;
    if (!identity_store_device()) return NODE_ID_ERR_NO_BACKEND;
    identity_patch_t patch = { .key = key, .key_len = key_len };
    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;
    return NODE_ID_OK;
}

const char *node_id_result_str(node_id_result_t rc) {
    switch (rc) {
        case NODE_ID_OK:               return "ok";
        case NODE_ID_ERR_NO_BACKEND:   return "no identity store on this target (plan/phase21_identity_and_authentication.md, I7 brings one to RP2350)";
        case NODE_ID_ERR_POPULATED:    return "already provisioned; use --force to overwrite";
        case NODE_ID_ERR_BAD_INPUT:    return "rejected (bad name/ssid, or a key that's empty, oversized, or trivially patterned)";
        case NODE_ID_ERR_NO_ENTROPY:   return "no hardware entropy source on this target; install a key by hand instead";
        case NODE_ID_ERR_WRITE_FAILED: return "the device write failed";
    }
    return "?";
}

node_id_result_t node_identity_generate_key(void) {
    if (!random_is_hardware()) return NODE_ID_ERR_NO_ENTROPY;
    uint8_t key[32];
    random_bytes(key, sizeof(key));
    node_id_result_t rc = node_identity_set_key(key, sizeof(key));
    memset(key, 0, sizeof(key));
    return rc;
}

/* --- I6, plan/phase21_identity_and_authentication.md §5.3: WLAN
 * credentials. Lands with phase 19's R5 (the CYW43 driver) and is unused
 * before it, same as the device key was between I3 and I4 -- this is
 * storage and the toolset, not a radio. */

bool node_wlan_ssid(char *out, uint32_t cap) {
    block_dev_t *dev = identity_store_device();
    if (!dev) return false;
    idstore_t rec;
    if (idstore_read(dev, &rec) != IDSTORE_VALID) return false;

    char buf[NODE_WLAN_SSID_MAX];
    int n = idstore_get_field(&rec, IDSTORE_FIELD_WLAN_SSID, buf, sizeof(buf));
    if (n <= 0) return false;

    if (out && cap > 0) {
        uint32_t take = (uint32_t)n < cap - 1 ? (uint32_t)n : cap - 1;
        memcpy(out, buf, take);
        out[take] = '\0';
    }
    return true;
}

bool node_wlan_psk(uint8_t out[NODE_WLAN_PSK_LEN]) {
    block_dev_t *dev = identity_store_device();
    if (!dev) return false;
    idstore_t rec;
    if (idstore_read(dev, &rec) != IDSTORE_VALID) return false;
    return idstore_get_field(&rec, IDSTORE_FIELD_WLAN_PSK, out, NODE_WLAN_PSK_LEN) == (int)NODE_WLAN_PSK_LEN;
}

node_id_result_t node_identity_set_wlan(const char *ssid, uint32_t ssid_len,
                                        const uint8_t *psk, uint32_t psk_len) {
    if (!ssid || ssid_len == 0 || ssid_len > NODE_WLAN_SSID_MAX) return NODE_ID_ERR_BAD_INPUT;
    if (!psk || psk_len != NODE_WLAN_PSK_LEN) return NODE_ID_ERR_BAD_INPUT;
    if (!identity_store_device()) return NODE_ID_ERR_NO_BACKEND;
    identity_patch_t patch = { .ssid = ssid, .ssid_len = ssid_len, .psk = psk, .psk_len = psk_len };
    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;
    return NODE_ID_OK;
}

/* --- Network autoconfig ------------------------------------------------- */

static bool ipv4_all_zero(const uint8_t q[NODE_IPV4_LEN]) {
    return (q[0] | q[1] | q[2] | q[3]) == 0;
}

bool node_ipv4(uint8_t ip[NODE_IPV4_LEN], uint8_t mask[NODE_IPV4_LEN],
               uint8_t gw[NODE_IPV4_LEN]) {
    block_dev_t *dev = identity_store_device();
    if (!dev) return false;
    idstore_t rec;
    if (idstore_read(dev, &rec) != IDSTORE_VALID) return false;

    uint8_t buf[3 * NODE_IPV4_LEN];
    if (idstore_get_field(&rec, IDSTORE_FIELD_IPV4, buf, sizeof(buf)) != (int)sizeof(buf)) {
        return false;
    }
    if (ip)   memcpy(ip,   buf,                     NODE_IPV4_LEN);
    if (mask) memcpy(mask, buf + NODE_IPV4_LEN,     NODE_IPV4_LEN);
    if (gw)   memcpy(gw,   buf + 2 * NODE_IPV4_LEN, NODE_IPV4_LEN);
    return true;
}

bool node_grants(char *out, uint32_t cap, uint32_t *len_out) {
    if (!out || cap == 0) return false;
    block_dev_t *dev = identity_store_device();
    if (!dev) return false;

    idstore_t rec;
    if (idstore_read(dev, &rec) != IDSTORE_VALID) return false;

    int n = idstore_get_field(&rec, IDSTORE_FIELD_GRANTS, out, cap);
    if (n <= 0) return false;
    if (len_out) *len_out = (uint32_t)(n < (int)cap ? n : (int)cap);
    return true;
}

node_id_result_t node_identity_set_grants(const char *text, uint32_t len) {
    if (!identity_store_device()) return NODE_ID_ERR_NO_BACKEND;
    if (len > NODE_GRANTS_MAX) return NODE_ID_ERR_BAD_INPUT;

    identity_patch_t patch = { 0 };
    if (len == 0) patch.clear_grants = true;
    else          { patch.grants = text; patch.grants_len = len; }

    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;
    return NODE_ID_OK;
}

node_id_result_t node_identity_set_ipv4(const uint8_t ip[NODE_IPV4_LEN],
                                        const uint8_t mask[NODE_IPV4_LEN],
                                        const uint8_t gw[NODE_IPV4_LEN]) {
    if (!ip || !mask || !gw) return NODE_ID_ERR_BAD_INPUT;

    /* Both of these describe a board that would come up looking configured
     * and answering nothing, which is a worse outcome than refusing: 0.0.0.0
     * is not an address, and a zero mask puts every destination off-link with
     * no way to reach even a neighbour. A zero *gateway* is deliberately
     * allowed -- that is a segment with no router, which is a real setup. */
    if (ipv4_all_zero(ip) || ipv4_all_zero(mask)) return NODE_ID_ERR_BAD_INPUT;
    if (!identity_store_device()) return NODE_ID_ERR_NO_BACKEND;

    uint8_t blob[3 * NODE_IPV4_LEN];
    memcpy(blob,                     ip,   NODE_IPV4_LEN);
    memcpy(blob + NODE_IPV4_LEN,     mask, NODE_IPV4_LEN);
    memcpy(blob + 2 * NODE_IPV4_LEN, gw,   NODE_IPV4_LEN);

    identity_patch_t patch = { .ipv4 = blob };
    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;
    return NODE_ID_OK;
}

node_id_result_t node_identity_clear_ipv4(void) {
    if (!identity_store_device()) return NODE_ID_ERR_NO_BACKEND;
    identity_patch_t patch = { .clear_ipv4 = true };
    if (identity_store_write(&patch) != 0) return NODE_ID_ERR_WRITE_FAILED;
    return NODE_ID_OK;
}
