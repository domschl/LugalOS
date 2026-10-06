/* A DHCP client (45.7, plan/phase45_esp32c6.md): the first address a sensor node gets on a network
 * whose router hands them out, instead of one provisioned into its identity record.
 *
 * DISCOVER -> OFFER -> REQUEST -> ACK, RFC 2131, with only the options a station needs: subnet mask,
 * router, lease time. The broadcast flag is set so the server answers to 255.255.255.255 -- this
 * stack has no way to receive a datagram addressed to an address it does not yet hold, and that is
 * the whole of the bootstrapping problem. Renewal is simple and honest: at half the lease the
 * exchange is run again from scratch (a server gives the same address back to the same client).
 *
 * It runs in its own kernel task, because it must wait for replies that only `netsrv` (which polls
 * the interface) can deliver, and netsrv must not be blocked.
 *
 * Not here: DNS and NTP server options (the node reads its time server from the identity record), a
 * decline/NAK dance beyond "start over", and anything IPv6. */

#include "net/ip.h"
#include "net/netif.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/random.h"
#include "kernel/identity.h"
#include "net_internal.h"
#include <string.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC       0x63825363u

enum { DHCPDISCOVER = 1, DHCPOFFER = 2, DHCPREQUEST = 3, DHCPNAK = 6, DHCPACK = 5 };

typedef struct {
    volatile bool got;
    uint8_t type;
    uint8_t yiaddr[4], server[4], mask[4], router[4];
    uint32_t lease_s;
    uint32_t xid;
} dhcp_rx_t;

static dhcp_rx_t g_rx;
static uint32_t g_xid;
static bool g_owned;          /* the address in use is one this client set (so it may renew it) */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void rx_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port, const uint8_t *d, uint32_t len) {
    (void)ctx; (void)src_ip;
    if (g_rx.got || src_port != DHCP_SERVER_PORT || len < 240) return;
    if (d[0] != 2 || get32(d + 4) != g_xid) return;                       /* a reply to *our* transaction */
    if (get32(d + 236) != DHCP_MAGIC) return;
    const netif_t *nif = net_state()->nif;
    if (!nif || memcmp(d + 28, nif->mac, NETIF_MAC_LEN) != 0) return;     /* chaddr: ours */

    dhcp_rx_t r;
    memset(&r, 0, sizeof r);
    memcpy(r.yiaddr, d + 16, 4);
    for (uint32_t i = 240; i + 1 < len && d[i] != 255; ) {
        uint8_t code = d[i];
        if (code == 0) { i++; continue; }
        uint8_t n = d[i + 1];
        if (i + 2 + n > len) break;
        const uint8_t *v = d + i + 2;
        if (code == 53 && n == 1) r.type = v[0];
        else if (code == 54 && n == 4) memcpy(r.server, v, 4);
        else if (code == 1 && n == 4) memcpy(r.mask, v, 4);
        else if (code == 3 && n >= 4) memcpy(r.router, v, 4);
        else if (code == 51 && n == 4) r.lease_s = get32(v);
        i += 2u + n;
    }
    if (r.type == 0) return;
    r.xid = g_xid;
    g_rx = r;
    g_rx.got = true;
}

static int send_msg(uint8_t type, const uint8_t *req_ip, const uint8_t *server) {
    const netif_t *nif = net_state()->nif;
    uint8_t m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;                                         /* BOOTREQUEST, Ethernet, hlen 6 */
    put32(m + 4, g_xid);
    m[10] = 0x80;                                                         /* flags: broadcast the reply */
    memcpy(m + 28, nif->mac, NETIF_MAC_LEN);
    put32(m + 236, DHCP_MAGIC);
    uint32_t o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = type;
    m[o++] = 61; m[o++] = 7; m[o++] = 1; memcpy(m + o, nif->mac, 6); o += 6;          /* client identifier */
    const char *host = node_name();
    uint32_t hl = host ? (uint32_t)strlen(host) : 0;
    if (hl > 0 && hl < 32) { m[o++] = 12; m[o++] = (uint8_t)hl; memcpy(m + o, host, hl); o += hl; }
    if (req_ip)  { m[o++] = 50; m[o++] = 4; memcpy(m + o, req_ip, 4); o += 4; }
    if (server)  { m[o++] = 54; m[o++] = 4; memcpy(m + o, server, 4); o += 4; }
    m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 51;                     /* parameter request: mask, router, lease */
    m[o++] = 255;
    static const uint8_t bcast[4] = { 255, 255, 255, 255 };
    return udp_send(bcast, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, m, o < 300 ? 300 : o) > 0 ? 0 : -1;
}

static bool wait_reply(uint8_t want, uint32_t ms) {
    uint64_t until = time_get_ms() + ms;
    while (time_get_ms() < until) {
        if (g_rx.got) {
            if (g_rx.type == want) return true;
            if (g_rx.type == DHCPNAK) return false;
            g_rx.got = false;                                              /* not what we waited for: keep waiting */
        }
        sched_yield();
    }
    return false;
}

/* One full exchange. On success fills the three out-parameters and the lease (seconds). */
static bool dhcp_exchange(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4], uint32_t *lease_s) {
    random_bytes(&g_xid, sizeof g_xid);
    memset(&g_rx, 0, sizeof g_rx);
    if (udp_bind(DHCP_CLIENT_PORT, rx_cb, NULL) != 0) return false;

    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        g_rx.got = false;
        if (send_msg(DHCPDISCOVER, NULL, NULL) != 0) { task_sleep_ms(500); continue; }
        if (!wait_reply(DHCPOFFER, 2000)) continue;
        uint8_t offered[4], server[4];
        memcpy(offered, g_rx.yiaddr, 4);
        memcpy(server, g_rx.server, 4);
        g_rx.got = false;
        if (send_msg(DHCPREQUEST, offered, server) != 0) continue;
        if (!wait_reply(DHCPACK, 2000)) continue;
        memcpy(ip, g_rx.yiaddr, 4);
        memcpy(mask, g_rx.mask, 4);
        memcpy(gw, g_rx.router, 4);
        *lease_s = g_rx.lease_s ? g_rx.lease_s : 3600;
        ok = true;
    }
    udp_unbind(DHCP_CLIENT_PORT);
    return ok;
}

static void dhcp_task(void *arg) {
    (void)arg;
    for (;;) {
        netif_t *nif = (netif_t *)net_state()->nif;
        if (!nif || !netif_link_up(nif)) { task_sleep_ms(500); continue; }

        /* Until we hold an address the stack must still be willing to send and to accept a
         * broadcast: "configured" with 0.0.0.0, which `ip_send` uses as the source address. */
        net_state_t *st = net_mutable_state();
        if (st->configured && !g_owned) { task_sleep_ms(2000); continue; }   /* an explicit (net-config) wins; leave it alone */
        bool had_address = st->configured;
        if (!had_address) { memset(st->ip, 0, 4); memset(st->mask, 0, 4); memset(st->gw, 0, 4); st->configured = true; }

        uint8_t ip[4], mask[4], gw[4];
        uint32_t lease_s = 0;
        bool ok = dhcp_exchange(ip, mask, gw, &lease_s);
        /* Take back the placeholder -- unless an explicit (net-config) filled it in meanwhile. */
        if (!had_address && st->ip[0] == 0 && st->ip[1] == 0 && st->ip[2] == 0 && st->ip[3] == 0) st->configured = false;

        if (!ok) { task_sleep_ms(5000); continue; }
        if (net_set_address(ip, mask, gw) == 0) {
            g_owned = true;
            printk("[DHCP] %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u, lease %us\n", ip[0], ip[1], ip[2], ip[3],
                   mask[0], mask[1], mask[2], mask[3], gw[0], gw[1], gw[2], gw[3], (unsigned)lease_s);
        }
        /* Renew at half the lease (clamped to something that is not a busy loop). */
        uint32_t wait = lease_s / 2;
        if (wait < 30) wait = 30;
        for (uint32_t s = 0; s < wait; s++) {
            task_sleep_ms(1000);
            if (!netif_link_up(nif)) break;                                /* link lost: acquire again when it is back */
        }
    }
}

int dhcp_start(void) {
    static int pid = -1;
    if (pid >= 0) return pid;
    pid = task_create_sized("dhcpc", dhcp_task, NULL, 1);   /* measured peak under 1 KB (C6, 2026-10-06) */
    if (pid < 0) printk("[DHCP] could not start the client task\n");
    return pid;
}
