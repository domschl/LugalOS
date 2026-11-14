#include "net/net_chan.h"
#include "net/tcp.h"
#include "net/dns.h"
#include "kernel/chan.h"
#include "kernel/balloc.h"

#include <string.h>

#define NET_CHAN_MAX_SOCKETS 8u

/* guarded by: chan endpoint lock */
static tcp_stream_t *g_net_slots[NET_CHAN_MAX_SOCKETS];

static uint16_t net_get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void net_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static int net_chan_handler(void *ctx, const uint8_t *req, uint32_t req_len,
                            uint8_t *resp, uint32_t resp_max) {
    (void)ctx;
    if (req_len == 0 || resp_max == 0) return -1;
    uint8_t op = req[0];

    if (op == 'D' && req_len >= 2 && resp_max >= 5) {
        char host[128];
        uint32_t hl = req_len - 1u;
        if (hl >= sizeof(host)) hl = sizeof(host) - 1u;
        memcpy(host, req + 1, hl);
        host[hl] = '\0';
        uint8_t ip[IPV4_LEN] = { 0, 0, 0, 0 };
        bool ok = dns_resolve(host, ip);
        resp[0] = ok ? 1u : 0u;
        memcpy(resp + 1, ip, IPV4_LEN);
        return 5;
    }

    if (op == 'O' && req_len >= 7 && resp_max >= 2) {
        int slot = -1;
        for (uint32_t i = 0; i < NET_CHAN_MAX_SOCKETS; i++) {
            if (!g_net_slots[i]) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            resp[0] = 0u;
            resp[1] = 0u;
            return 2;
        }
        tcp_stream_t *s = (tcp_stream_t *)balloc_alloc(sizeof(tcp_stream_t));
        if (!s) {
            resp[0] = 0u;
            resp[1] = 0u;
            return 2;
        }
        memset(s, 0, sizeof(*s));
        uint16_t port = net_get_be16(req + 5);
        if (tcp_stream_open(s, req + 1, port) != 0) {
            balloc_free(s);
            resp[0] = 0u;
            resp[1] = 0u;
            return 2;
        }
        g_net_slots[slot] = s;
        resp[0] = 1u;
        resp[1] = (uint8_t)slot;
        return 2;
    }

    if (req_len < 2) return -1;
    uint8_t h = req[1];
    if (h >= NET_CHAN_MAX_SOCKETS || !g_net_slots[h]) {
        resp[0] = 0u;
        return 1;
    }
    tcp_stream_t *s = g_net_slots[h];

    if (op == 'R') {
        int r = tcp_stream_ready(s);
        resp[0] = (r > 0) ? 1u : (r < 0 ? 2u : 0u);
        return 1;
    }

    if (op == 'W' && resp_max >= 3) {
        uint32_t wlen = req_len - 2u;
        int w = tcp_stream_write(s, req + 2, wlen);
        if (w < 0) {
            resp[0] = 0u;
            net_put_be16(resp + 1, 0u);
        } else {
            resp[0] = 1u;
            net_put_be16(resp + 1, (uint16_t)w);
        }
        return 3;
    }

    if (op == 'G' && req_len >= 4 && resp_max >= 2) {
        uint16_t max_r = net_get_be16(req + 2);
        if (max_r > resp_max - 1u) max_r = (uint16_t)(resp_max - 1u);
        int n = tcp_stream_read(s, resp + 1, max_r);
        if (n < 0) {
            resp[0] = 2u;
            return 1;
        }
        if (n == 0 && tcp_stream_peer_closed(s)) {
            resp[0] = 1u;
            return 1;
        }
        resp[0] = 0u;
        return 1 + n;
    }

    if (op == 'C') {
        tcp_stream_close(s);
        balloc_free(s);
        g_net_slots[h] = NULL;
        resp[0] = 1u;
        return 1;
    }

    return -1;
}

void net_chan_init(void) {
    uint8_t *req = (uint8_t *)balloc_alloc(280u);
    uint8_t *resp = (uint8_t *)balloc_alloc(280u);
    if (!req || !resp) return;
    (void)chan_register("net", net_chan_handler, NULL, req, 280u, resp, 280u);
}
