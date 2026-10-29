/* Claiming a node, and the owner part of its identity over the network (kernel/include/kernel/
 * setup.h). The state is derived, not stored: a node is CLAIMED when a grant carries `admin`, and
 * OPEN when it is not and a window is running. */

#include "lugalos_config.h"
#include "kernel/setup.h"
#include "kernel/identity.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "fs/9p.h"
#include <string.h>

#if defined(CONFIG_NETWORK_CLAIM)

#define REQ_LINE_MAX 320u

/* guarded by: g_lock; the 9P server task and the shell queue requests and read the state, the
 * worker runs them. */
static spinlock_t g_lock;
static uint64_t   g_open_until_ms;          /* 0: no window */
static uint32_t   g_attempts;               /* failed claims in this window */
static char       g_result[96] = "none";    /* the last request's outcome */
static bool       g_pending;                /* a request is queued or running */
static char       g_req[REQ_LINE_MAX];
static bool       g_req_owner;              /* /dev/owner (admin) rather than /dev/setup (claim) */
static bool       g_have_secret;            /* set once at boot, and by `setup secret` */
static bool       g_forced;                 /* the window was opened on purpose, not by boot */
static uint8_t    g_net_ip[4], g_net_mask[4];
static bool       g_net_set;

setup_state_t setup_state(void) {
    if (p9_grants_has_admin()) return SETUP_CLAIMED;
    uintptr_t f = spin_lock_irqsave(&g_lock);
    bool open = g_open_until_ms != 0 && time_get_ms() < g_open_until_ms && g_attempts < SETUP_MAX_ATTEMPTS;
    spin_unlock_irqrestore(&g_lock, f);
    return open ? SETUP_OPEN : SETUP_UNCLAIMED;
}

const char *setup_state_name(setup_state_t s) {
    return s == SETUP_CLAIMED ? "claimed" : s == SETUP_OPEN ? "setup" : "unclaimed";
}

static bool window_open(bool forced) {
    if (!g_have_secret) return false;
    uintptr_t f = spin_lock_irqsave(&g_lock);
    g_open_until_ms = time_get_ms() + (uint64_t)SETUP_WINDOW_S * 1000u;
    g_attempts = 0;
    g_forced = forced;
    spin_unlock_irqrestore(&g_lock, f);
    return true;
}

bool setup_start(void) { return window_open(true); }

void setup_stop(void) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    g_open_until_ms = 0;
    g_forced = false;
    spin_unlock_irqrestore(&g_lock, f);
}

void setup_net_set(const uint8_t ip[4], const uint8_t mask[4]) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    memcpy(g_net_ip, ip, 4);
    memcpy(g_net_mask, mask, 4);
    g_net_set = true;
    spin_unlock_irqrestore(&g_lock, f);
}

void setup_net_clear(void) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    g_net_set = false;
    spin_unlock_irqrestore(&g_lock, f);
}

bool setup_net_contains(const uint8_t ip[4]) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    bool in = g_net_set;
    for (int i = 0; in && i < 4; i++) in = (ip[i] & g_net_mask[i]) == (g_net_ip[i] & g_net_mask[i]);
    spin_unlock_irqrestore(&g_lock, f);
    return in;
}

bool setup_ap_wanted(void) {
    if (setup_state() != SETUP_OPEN) return false;
    char ssid[NODE_WLAN_SSID_MAX + 1];
    return g_forced || !node_wlan_ssid(ssid, sizeof ssid);
}

bool setup_unauth_attach_ok(void) { return setup_state() == SETUP_OPEN; }

static void set_result(const char *s) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    strncpy(g_result, s, sizeof g_result - 1);
    g_result[sizeof g_result - 1] = 0;
    spin_unlock_irqrestore(&g_lock, f);
}

/* --- the worker's half ------------------------------------------------------------ */

static const char *tok(const char *s, char *out, uint32_t cap) {
    uint32_t i = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && i < cap - 1) out[i++] = *s++;
    out[i] = 0;
    while (*s == ' ') s++;
    return s;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t hex_bytes(const char *h, uint8_t *out, uint32_t cap) {
    uint32_t n = (uint32_t)strlen(h);
    if (n == 0 || (n & 1u) || n / 2 > cap) return 0;
    for (uint32_t i = 0; i < n; i += 2) {
        int a = hex_nibble(h[i]), b = hex_nibble(h[i + 1]);
        if (a < 0 || b < 0) return 0;
        out[i / 2] = (uint8_t)((a << 4) | b);
    }
    return n / 2;
}

static bool ct_equal(const char *a, const char *b) {
    uint32_t la = (uint32_t)strlen(a), lb = (uint32_t)strlen(b);
    uint8_t d = (uint8_t)(la != lb);
    for (uint32_t i = 0; i < la && i < lb; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void claim_failed(const char *why) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    g_attempts++;
    bool closed = g_attempts >= SETUP_MAX_ATTEMPTS;
    spin_unlock_irqrestore(&g_lock, f);
    char msg[96];
    ksnprintf(msg, sizeof msg, closed ? "error: %s; setup is closed until the next trigger" : "error: %s", why);
    set_result(msg);
    printk("[Setup] claim refused: %s%s\n", why, closed ? " (setup closed)" : "");
}

/* name, wlan, then the admin grant last: the grant is what makes the node claimed, so a failure
 * before it leaves the node unclaimed, and the client can ask again. */
static void do_claim(const char *args) {
    char secret[NODE_SETUP_SECRET_MAX + 4], keyhex[160], word[16];
    args = tok(args, secret, sizeof secret);
    args = tok(args, keyhex, sizeof keyhex);
    if (setup_state() != SETUP_OPEN) { set_result("error: no setup window is open"); return; }
    char want[NODE_SETUP_SECRET_MAX + 1];
    if (!node_setup_secret(want, sizeof want)) { claim_failed("this node has no setup secret"); return; }
    if (!ct_equal(secret, want)) { memset(want, 0, sizeof want); claim_failed("wrong setup secret"); return; }
    memset(want, 0, sizeof want);
    uint8_t key[P9_AUTH_KEY_MAX];
    uint32_t klen = hex_bytes(keyhex, key, sizeof key);
    if (klen < 16) { claim_failed("the owner key must be 16 to 64 bytes of hex"); return; }

    char name[NODE_NAME_MAX] = "", ssid[NODE_WLAN_SSID_MAX + 1] = "", pskhex[NODE_WLAN_PSK_LEN * 2 + 2] = "";
    while (*args) {
        args = tok(args, word, sizeof word);
        if (strcmp(word, "name") == 0) {
            args = tok(args, name, sizeof name);
        } else if (strcmp(word, "wlan") == 0) {
            args = tok(args, ssid, sizeof ssid);
            args = tok(args, pskhex, sizeof pskhex);
        } else {
            memset(key, 0, sizeof key);
            claim_failed("unknown claim option");
            return;
        }
    }
    uint8_t psk[NODE_WLAN_PSK_LEN];
    if (ssid[0] && hex_bytes(pskhex, psk, sizeof psk) != NODE_WLAN_PSK_LEN) {
        memset(key, 0, sizeof key);
        claim_failed("the WLAN key is the 64 hex characters of the derived PSK");
        return;
    }
    if (name[0] && node_identity_rename_persistent(name) != NODE_ID_OK) { memset(key, 0, sizeof key); claim_failed("bad node name"); return; }
    if (ssid[0] && node_identity_set_wlan(ssid, (uint32_t)strlen(ssid), psk, sizeof psk) != NODE_ID_OK) {
        memset(key, 0, sizeof key);
        claim_failed("could not store the WLAN");
        return;
    }
    p9_grant_result_t rc = p9_grants_add_admin("owner", key, klen, "/");
    memset(key, 0, sizeof key);
    memset(psk, 0, sizeof psk);
    if (rc != P9_GRANT_OK) { claim_failed(p9_grant_result_str(rc)); return; }
    setup_stop();
    set_result("ok: claimed");
    printk("[Setup] node claimed\n");
}

static void do_owner(const char *args) {
    char cmd[16];
    args = tok(args, cmd, sizeof cmd);
    if (strcmp(cmd, "name") == 0) {
        char name[NODE_NAME_MAX];
        tok(args, name, sizeof name);
        set_result(name[0] && node_identity_rename_persistent(name) == NODE_ID_OK ? "ok" : "error: bad node name");
    } else if (strcmp(cmd, "wlan") == 0) {
        char ssid[NODE_WLAN_SSID_MAX + 1], pskhex[NODE_WLAN_PSK_LEN * 2 + 2];
        uint8_t psk[NODE_WLAN_PSK_LEN];
        args = tok(args, ssid, sizeof ssid);
        tok(args, pskhex, sizeof pskhex);
        if (!ssid[0] || hex_bytes(pskhex, psk, sizeof psk) != NODE_WLAN_PSK_LEN) {
            set_result("error: wlan SSID PSKHEX, the PSK being the 64 hex characters of the derived key");
        } else {
            set_result(node_identity_set_wlan(ssid, (uint32_t)strlen(ssid), psk, sizeof psk) == NODE_ID_OK
                       ? "ok" : "error: could not store the WLAN");
        }
        memset(psk, 0, sizeof psk);
    } else if (strcmp(cmd, "grant") == 0) {
        char name[P9_MAX_NAME_LEN], keyhex[160], aname[P9_MAX_NAME_LEN], mode[8];
        uint8_t key[P9_AUTH_KEY_MAX];
        args = tok(args, name, sizeof name);
        args = tok(args, keyhex, sizeof keyhex);
        args = tok(args, aname, sizeof aname);
        tok(args, mode, sizeof mode);
        uint32_t klen = hex_bytes(keyhex, key, sizeof key);
        if (aname[0] && aname[0] != '/') { strncpy(mode, aname, sizeof mode - 1); mode[sizeof mode - 1] = 0; strcpy(aname, "/"); }
        if (!name[0] || klen == 0) {
            set_result("error: grant NAME HEXKEY [ANAME] [ro|rw|admin]");
        } else {
            p9_grant_result_t rc = strcmp(mode, "admin") == 0
                ? p9_grants_add_admin(name, key, klen, aname[0] ? aname : "/")
                : p9_grants_add(name, key, klen, aname[0] ? aname : "/", strcmp(mode, "ro") == 0);
            set_result(rc == P9_GRANT_OK ? "ok" : p9_grant_result_str(rc));
        }
        memset(key, 0, sizeof key);
    } else if (strcmp(cmd, "ungrant") == 0) {
        char name[P9_MAX_NAME_LEN];
        tok(args, name, sizeof name);
        p9_grant_t all[P9_GRANTS_MAX];
        uint32_t n = p9_grants_list(all, P9_GRANTS_MAX), admins = 0;
        bool target_admin = false;
        for (uint32_t i = 0; i < n; i++) {
            if (all[i].admin) admins++;
            if (strcmp(all[i].name, name) == 0 && all[i].admin) target_admin = true;
        }
        if (target_admin && admins == 1) {
            set_result("error: that is the last admin grant; removing it would unclaim the node (setup reset-owner on the console)");
        } else {
            p9_grant_result_t rc = p9_grants_remove(name);
            set_result(rc == P9_GRANT_OK ? "ok" : p9_grant_result_str(rc));
        }
        memset(all, 0, sizeof all);
    } else if (strcmp(cmd, "setup") == 0) {
        char sub[8];
        tok(args, sub, sizeof sub);
        if (strcmp(sub, "start") == 0) set_result(setup_start() ? "ok" : "error: this node has no setup secret");
        else if (strcmp(sub, "stop") == 0) { setup_stop(); set_result("ok"); }
        else set_result("error: setup start|stop");
    } else {
        set_result("error: name | wlan | grant | ungrant | setup");
    }
}

static void setup_worker(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(200);
        char line[REQ_LINE_MAX];
        bool owner = false, have = false;
        uintptr_t f = spin_lock_irqsave(&g_lock);
        if (g_pending) {
            memcpy(line, g_req, sizeof line);
            owner = g_req_owner;
            have = true;
        }
        spin_unlock_irqrestore(&g_lock, f);
        if (!have) continue;
        if (owner) {
            do_owner(line);
        } else if (strncmp(line, "claim ", 6) == 0) {
            do_claim(line + 6);
        } else {
            set_result("error: claim SECRET KEYHEX [name NAME] [wlan SSID PSKHEX]");
        }
        memset(line, 0, sizeof line);
        f = spin_lock_irqsave(&g_lock);
        memset(g_req, 0, sizeof g_req);
        g_pending = false;
        spin_unlock_irqrestore(&g_lock, f);
    }
}

static int queue(const char *buf, uint32_t len, bool owner) {
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) len--;
    if (len == 0 || len >= REQ_LINE_MAX) return -1;
    /* Secrets -- a WLAN key, a grant's key, the owner key of a claim -- do not cross a network link
     * that anyone on the LAN can read, protected or not (a protected session is integrity, not
     * secrecy): the console, a cable, TLS (52.15) or the setup access point (52.12). */
    if (!p9_request_confidential()) {
        const bool secret = !owner || strncmp(buf, "wlan ", 5) == 0 || strncmp(buf, "grant ", 6) == 0;
        if (secret && !p9_link_is_setup_ap()) {
            set_result("error: that carries a secret, and this link can be read on the network: use the console, a cable or TLS");
            return -1;
        }
    }
    if (!owner && setup_state() != SETUP_OPEN) return -1;
    uintptr_t f = spin_lock_irqsave(&g_lock);
    bool busy = g_pending;
    if (!busy) {
        memcpy(g_req, buf, len);
        g_req[len] = 0;
        g_req_owner = owner;
        g_pending = true;
        strcpy(g_result, "pending");
    }
    spin_unlock_irqrestore(&g_lock, f);
    return busy ? -1 : (int)len;
}

int setup_ctl_write(const char *buf, uint32_t len) { return queue(buf, len, false) < 0 ? -1 : (int)len; }
int owner_ctl_write(const char *buf, uint32_t len) { return queue(buf, len, true) < 0 ? -1 : (int)len; }

uint32_t setup_ctl_read(char *out, uint32_t cap) {
    setup_state_t st = setup_state();
    uintptr_t f = spin_lock_irqsave(&g_lock);
    uint64_t now = time_get_ms();
    uint32_t left = (st == SETUP_OPEN && g_open_until_ms > now) ? (uint32_t)((g_open_until_ms - now) / 1000u) : 0;
    uint32_t att = g_attempts;
    char res[sizeof g_result];
    memcpy(res, g_result, sizeof res);
    spin_unlock_irqrestore(&g_lock, f);
    return (uint32_t)ksnprintf(out, cap, "state=%s\nsecs_left=%u\nfailed_attempts=%u\nmax_attempts=%u\nresult=%s\n",
                               setup_state_name(st), (unsigned)left, (unsigned)att, (unsigned)SETUP_MAX_ATTEMPTS, res);
}

uint32_t owner_ctl_read(char *out, uint32_t cap) {
    uint32_t n = 0;
    char ssid[NODE_WLAN_SSID_MAX + 1];
    n += (uint32_t)ksnprintf(out + n, cap - n, "name=%s\nssid=%s\n", node_name(),
                             node_wlan_ssid(ssid, sizeof ssid) ? ssid : "");
    p9_grant_t g[P9_GRANTS_MAX];
    uint32_t cnt = p9_grants_list(g, P9_GRANTS_MAX);
    for (uint32_t i = 0; i < cnt && n < cap; i++)
        n += (uint32_t)ksnprintf(out + n, cap - n, "grant=%s %s %s\n", g[i].name, g[i].aname,
                                 g[i].admin ? "admin" : g[i].read_only ? "ro" : "rw");
    memset(g, 0, sizeof g);
    return n < cap ? n : cap;
}

void setup_init(void) {
    setup_button_start();
    setup_ap_start();
    char probe[NODE_SETUP_SECRET_MAX + 1];
    g_have_secret = node_setup_secret(probe, sizeof probe);
    memset(probe, 0, sizeof probe);
    if (task_create_sized("setupd", setup_worker, NULL, 3) < 0) printk("[Setup] could not start the worker\n");
    if (g_have_secret && !p9_grants_has_admin()) {
        (void)window_open(false);
        printk("[Setup] unclaimed: a setup window is open for %u s\n", (unsigned)SETUP_WINDOW_S);
    }
}

#else   /* a build that cannot be claimed over the network */

setup_state_t setup_state(void) { return p9_grants_has_admin() ? SETUP_CLAIMED : SETUP_UNCLAIMED; }
const char *setup_state_name(setup_state_t s) { return s == SETUP_CLAIMED ? "claimed" : "unclaimed"; }
bool setup_start(void) { return false; }
void setup_stop(void) { }
bool setup_unauth_attach_ok(void) { return false; }
void setup_net_set(const uint8_t ip[4], const uint8_t mask[4]) { (void)ip; (void)mask; }
void setup_net_clear(void) { }
bool setup_net_contains(const uint8_t ip[4]) { (void)ip; return false; }
bool setup_ap_wanted(void) { return false; }
void setup_init(void) { setup_button_start(); }
uint32_t setup_ctl_read(char *out, uint32_t cap) { (void)out; (void)cap; return 0; }
int setup_ctl_write(const char *buf, uint32_t len) { (void)buf; (void)len; return -1; }
uint32_t owner_ctl_read(char *out, uint32_t cap) { (void)out; (void)cap; return 0; }
int owner_ctl_write(const char *buf, uint32_t len) { (void)buf; (void)len; return -1; }

#endif

node_id_result_t setup_reset_owner(void) {
    node_id_result_t rc = node_identity_reset_owner();
    if (rc == NODE_ID_OK) {
        p9_grants_cache_clear();
        setup_stop();
        printk("[Setup] owner part wiped: the node is unclaimed\n");
    }
    return rc;
}
