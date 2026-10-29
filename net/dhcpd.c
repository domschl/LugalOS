#include "net/dhcpd.h"
#include "net/ip.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

#define DHCP_MAGIC 0x63825363u
enum { MSG_DISCOVER = 1, MSG_OFFER = 2, MSG_REQUEST = 3, MSG_DECLINE = 4, MSG_ACK = 5, MSG_NAK = 6, MSG_RELEASE = 7 };
#define OFFER_HOLD_S 30u        /* an offer that is never requested frees its address after this */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

void dhcpd_init(dhcpd_t *d, const dhcpd_config_t *cfg) {
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    for (uint32_t i = 0; i < DHCPD_LEASES; i++) {
        memcpy(d->lease[i].ip, cfg->first_ip, 4);
        d->lease[i].ip[3] = (uint8_t)(cfg->first_ip[3] + i);
    }
}

static bool lease_live(const dhcpd_lease_t *l, uint32_t now_s) { return l->until_s != 0 && (int32_t)(l->until_s - now_s) > 0; }

/* The slot this client already has, else a free (or expired) one; NULL if none. */
static dhcpd_lease_t *lease_for(dhcpd_t *d, const uint8_t mac[6], uint32_t now_s) {
    for (uint32_t i = 0; i < DHCPD_LEASES; i++)
        if (lease_live(&d->lease[i], now_s) && memcmp(d->lease[i].mac, mac, 6) == 0) return &d->lease[i];
    for (uint32_t i = 0; i < DHCPD_LEASES; i++)
        if (!lease_live(&d->lease[i], now_s)) return &d->lease[i];
    return NULL;
}

static const uint8_t *find_option(const uint8_t *opts, uint32_t n, uint8_t code, uint8_t *len_out) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = opts[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= n) break;
        uint8_t l = opts[i + 1];
        if (i + 2 + l > n) break;
        if (c == code) { *len_out = l; return opts + i + 2; }
        i += 2u + l;
    }
    return NULL;
}

uint32_t dhcpd_handle(dhcpd_t *d, const uint8_t *req, uint32_t len, uint32_t now_s,
                      uint8_t reply[DHCPD_MAX_REPLY]) {
    if (len < 240 || req[0] != 1 || req[1] != 1 || req[2] != 6) return 0;      /* BOOTREQUEST, Ethernet */
    if (rd32(req + 236) != DHCP_MAGIC) return 0;
    const uint8_t *opts = req + 240;
    uint32_t on = len - 240;
    uint8_t l = 0;
    const uint8_t *t = find_option(opts, on, 53, &l);
    if (!t || l != 1) return 0;
    uint8_t type = t[0];
    const uint8_t *mac = req + 28;

    /* A request naming another server is that server's business. */
    const uint8_t *sid = find_option(opts, on, 54, &l);
    if (sid && l == 4 && memcmp(sid, d->cfg.server_ip, 4) != 0) return 0;

    uint8_t rtype;
    dhcpd_lease_t *lease = NULL;
    if (type == MSG_RELEASE || type == MSG_DECLINE) {
        for (uint32_t i = 0; i < DHCPD_LEASES; i++)
            if (memcmp(d->lease[i].mac, mac, 6) == 0 && lease_live(&d->lease[i], now_s)) d->lease[i].until_s = 0;
        return 0;
    }
    if (type == MSG_DISCOVER) {
        lease = lease_for(d, mac, now_s);
        if (!lease) return 0;                       /* four clients is the limit: the fifth is not answered */
        memcpy(lease->mac, mac, 6);
        lease->until_s = now_s + OFFER_HOLD_S;
        lease->offered_only = true;
        rtype = MSG_OFFER;
    } else if (type == MSG_REQUEST) {
        const uint8_t *want = find_option(opts, on, 50, &l);
        uint8_t ask[4] = { 0 };
        if (want && l == 4) memcpy(ask, want, 4);
        else memcpy(ask, req + 12, 4);              /* ciaddr: a renewal */
        lease = lease_for(d, mac, now_s);
        bool ok = lease && memcmp(lease->ip, ask, 4) == 0;
        if (!ok) {
            lease = NULL;
            rtype = MSG_NAK;
        } else {
            memcpy(lease->mac, mac, 6);
            lease->until_s = now_s + DHCPD_LEASE_S;
            lease->offered_only = false;
            rtype = MSG_ACK;
        }
    } else {
        return 0;                                   /* INFORM and the rest: nothing to say */
    }

    memset(reply, 0, 240);
    reply[0] = 2; reply[1] = 1; reply[2] = 6;       /* BOOTREPLY, Ethernet, hlen 6 */
    memcpy(reply + 4, req + 4, 4);                  /* xid */
    reply[10] = 0x80;                               /* the broadcast flag: this reply goes to everyone */
    if (lease) memcpy(reply + 16, lease->ip, 4);    /* yiaddr */
    memcpy(reply + 20, d->cfg.server_ip, 4);        /* siaddr */
    memcpy(reply + 28, mac, 6);                     /* chaddr */
    wr32(reply + 236, DHCP_MAGIC);

    uint8_t *o = reply + 240;
    *o++ = 53; *o++ = 1; *o++ = rtype;
    *o++ = 54; *o++ = 4; memcpy(o, d->cfg.server_ip, 4); o += 4;
    if (rtype != MSG_NAK) {
        *o++ = 51; *o++ = 4; wr32(o, DHCPD_LEASE_S); o += 4;
        *o++ = 58; *o++ = 4; wr32(o, DHCPD_LEASE_S / 2); o += 4;
        *o++ = 59; *o++ = 4; wr32(o, DHCPD_LEASE_S * 7 / 8); o += 4;
        *o++ = 1;  *o++ = 4; memcpy(o, d->cfg.mask, 4); o += 4;
        *o++ = 3;  *o++ = 4; memcpy(o, d->cfg.server_ip, 4); o += 4;
        *o++ = 6;  *o++ = 4; memcpy(o, d->cfg.server_ip, 4); o += 4;
        if (d->cfg.portal_uri) {
            uint32_t pl = (uint32_t)strlen(d->cfg.portal_uri);
            if (pl > 0 && pl < 100) { *o++ = 114; *o++ = (uint8_t)pl; memcpy(o, d->cfg.portal_uri, pl); o += pl; }
        }
    }
    *o++ = 255;
    uint32_t n = (uint32_t)(o - reply);
    return n < 300u ? 300u : n;                     /* BOOTP's minimum */
}


/* --- DNS ----------------------------------------------------------------------------- */

uint32_t dnsd_handle(const uint8_t ip[4], const uint8_t *req, uint32_t len, uint8_t *reply, uint32_t cap) {
    if (len < 12 || cap < 12) return 0;
    uint16_t flags = (uint16_t)(req[2] << 8 | req[3]);
    if (flags & 0x8000u) return 0;                  /* a response, not a query */
    if (((flags >> 11) & 0xF) != 0) return 0;       /* not a standard query */
    if ((uint16_t)(req[4] << 8 | req[5]) < 1) return 0;

    /* The first question: a name, then type and class. Compression cannot occur in a question's name. */
    uint32_t p = 12;
    while (p < len && req[p] != 0) {
        if ((req[p] & 0xC0) != 0) return 0;
        p += 1u + req[p];
    }
    if (p + 5 > len) return 0;
    uint32_t qend = p + 1 + 4;
    uint16_t qtype = (uint16_t)(req[p + 1] << 8 | req[p + 2]);
    uint16_t qclass = (uint16_t)(req[p + 3] << 8 | req[p + 4]);
    bool answer = qtype == 1 && qclass == 1;
    uint32_t need = qend + (answer ? 16u : 0u);
    if (need > cap) return 0;

    memcpy(reply, req, qend);                       /* id, the question as asked */
    reply[2] = (uint8_t)(0x80u | (req[2] & 0x01u)); /* QR, opcode 0, RD echoed */
    reply[3] = 0x80;                                /* RA, RCODE 0 */
    reply[4] = 0; reply[5] = 1;
    reply[6] = 0; reply[7] = answer ? 1 : 0;
    memset(reply + 8, 0, 4);
    if (!answer) return qend;
    uint8_t *a = reply + qend;
    a[0] = 0xC0; a[1] = 0x0C;                       /* the name, by pointer to the question */
    a[2] = 0; a[3] = 1; a[4] = 0; a[5] = 1;         /* A, IN */
    wr32(a + 6, 60);                                /* TTL */
    a[10] = 0; a[11] = 4;
    memcpy(a + 12, ip, 4);
    return need;
}

/* --- binding to UDP ------------------------------------------------------------------- */

/* guarded by: net_lock() -- both callbacks run on the network task, which holds it. */
static dhcpd_t g_dhcpd;
static uint8_t g_ip[4];
static bool g_running;

static void dhcpd_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port, const uint8_t *data, uint32_t len) {
    (void)ctx; (void)src_ip; (void)src_port;
    uint8_t reply[DHCPD_MAX_REPLY];
    uint32_t n = dhcpd_handle(&g_dhcpd, data, len, (uint32_t)(time_get_ms() / 1000u), reply);
    if (!n) return;
    static const uint8_t bcast[4] = { 255, 255, 255, 255 };
    (void)udp_send(bcast, 68, 67, reply, n);
}

static void dnsd_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port, const uint8_t *data, uint32_t len) {
    (void)ctx;
    uint8_t reply[200];
    uint32_t n = dnsd_handle(g_ip, data, len, reply, sizeof reply);
    if (n) (void)udp_send(src_ip, src_port, 53, reply, n);
}

uint32_t dhcpd_active_leases(uint32_t now_s) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < DHCPD_LEASES; i++) if (lease_live(&g_dhcpd.lease[i], now_s) && !g_dhcpd.lease[i].offered_only) n++;
    return n;
}

int setup_net_services_start(const dhcpd_config_t *cfg) {
    if (g_running) return 0;
    dhcpd_init(&g_dhcpd, cfg);
    memcpy(g_ip, cfg->server_ip, 4);
    if (udp_bind(67, dhcpd_cb, NULL) != 0) return -1;
    if (udp_bind(53, dnsd_cb, NULL) != 0) { udp_unbind(67); return -1; }
    g_running = true;
    return 0;
}

void setup_net_services_stop(void) {
    if (!g_running) return;
    udp_unbind(67);
    udp_unbind(53);
    g_running = false;
}
