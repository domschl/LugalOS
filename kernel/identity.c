#include "kernel/identity.h"
#include "kernel/sha256.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#include <string.h>

static char     g_name[NODE_NAME_MAX];
static uint8_t  g_mac[NODE_MAC_LEN];
static const char *g_name_source = "unset";
static const char *g_mac_source  = "unset";
static bool     g_mac_is_derived;

/* Weak, so a board can override it by simply defining the symbol. The default
 * is an honest "this silicon cannot identify itself", which is true of both
 * QEMU targets and -- until R4 wires the bootrom call and checks it against
 * two real boards -- of the RP2350 as well. */
__attribute__((weak)) bool board_unique_id(uint8_t out[8]) {
    (void)out;
    return false;
}

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
#ifdef CONFIG_NODE_NAME
    strncpy(g_name, CONFIG_NODE_NAME, NODE_NAME_MAX - 1);
    g_name[NODE_NAME_MAX - 1] = '\0';
    g_name_source = "board file";
    if (g_name[0] == '\0') derive_name();
#else
    derive_name();
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
}

const char *node_name(void) { return g_name[0] ? g_name : "lugal"; }
const uint8_t *node_mac(void) { return g_mac; }
const char *node_name_source(void) { return g_name_source; }
const char *node_mac_source(void) { return g_mac_source; }

int node_set_name(const char *name) {
    if (!name || !name[0]) return -1;
    uint32_t n = 0;
    while (name[n]) {
        /* Kept to what is safe in a 9P uname, a hostname and a log line at
         * once: letters, digits, dash and dot. */
        char c = name[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!ok) return -1;
        if (++n >= NODE_NAME_MAX) return -1;
    }
    memcpy(g_name, name, n);
    g_name[n] = '\0';
    g_name_source = "set at runtime";
    return 0;
}
