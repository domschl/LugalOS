#include "net/net_chan.h"
#include "net/tcp.h"
#include "net/dns.h"
#include "net/http.h"
#include "kernel/chan.h"
#include "kernel/balloc.h"
#include "kernel/palloc.h"

#include <string.h>

#define NET_CHAN_MAX_SOCKETS 8u
#define NET_CHAN_MAX_HTTP    4u
#define NET_HTTP_BUF_PAGES   16u
#define NET_HTTP_BUF_MAX     (NET_HTTP_BUF_PAGES * 4096u)

typedef struct {
    uint8_t *buf;
    uint32_t len;
    uint32_t off;
    int      status;
    bool     used;
} net_http_slot_t;

/* guarded by: chan endpoint lock */
static tcp_stream_t *g_net_slots[NET_CHAN_MAX_SOCKETS];
/* guarded by: chan endpoint lock */
static net_http_slot_t g_http_slots[NET_CHAN_MAX_HTTP];

static uint16_t net_get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void net_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void net_put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >>  8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static int net_http_chunk_cb(const uint8_t *data, uint32_t len, void *ctx) {
    net_http_slot_t *hs = (net_http_slot_t *)ctx;
    if (!hs || !hs->buf) return -1;
    uint32_t avail = NET_HTTP_BUF_MAX - hs->len;
    uint32_t copy = len < avail ? len : avail;
    if (copy > 0) {
        memcpy(hs->buf + hs->len, data, copy);
        hs->len += copy;
    }
    return (copy < len) ? -1 : 0;
}

static int net_chan_handler(void *ctx, const uint8_t *req, uint32_t req_len,
                            uint8_t *resp, uint32_t resp_max) {
    (void)ctx;
    if (req_len == 0 || resp_max == 0) return -1;
    uint8_t op = req[0];

    if (op == 'H' && req_len >= 5 && resp_max >= 8) {
        uint8_t mlen = req[1];
        if (mlen == 0 || mlen > 15u || (uint32_t)(2u + mlen + 2u) > req_len) {
            resp[0] = 0u; resp[1] = 0u; return 2;
        }
        uint16_t ulen = net_get_be16(req + 2u + mlen);
        if (ulen == 0 || ulen > 255u || (uint32_t)(4u + mlen + ulen) > req_len) {
            resp[0] = 0u; resp[1] = 0u; return 2;
        }
        int slot = -1;
        for (uint32_t i = 0; i < NET_CHAN_MAX_HTTP; i++) {
            if (!g_http_slots[i].used) { slot = (int)i; break; }
        }
        if (slot < 0) { resp[0] = 0u; resp[1] = 0u; return 2; }

        uint8_t *buf = (uint8_t *)palloc_pages_bulk(NET_HTTP_BUF_PAGES);
        if (!buf) { resp[0] = 0u; resp[1] = 0u; return 2; }

        char method[16], url[256], err[64];
        memcpy(method, req + 2u, mlen); method[mlen] = '\0';
        memcpy(url, req + 4u + mlen, ulen); url[ulen] = '\0';

        g_http_slots[slot].buf = buf;
        g_http_slots[slot].len = 0;
        g_http_slots[slot].off = 0;
        g_http_slots[slot].used = true;

        http_req_t hreq;
        memset(&hreq, 0, sizeof(hreq));
        hreq.url = url;
        hreq.method = method;
        hreq.timeout_ms = 20000u;
        int status = http_request(&hreq, net_http_chunk_cb, &g_http_slots[slot], err, sizeof(err));
        if (status <= 0) {
            palloc_free(buf, NET_HTTP_BUF_PAGES);
            g_http_slots[slot].buf = NULL;
            g_http_slots[slot].used = false;
            resp[0] = 0u;
            resp[1] = 0u;
            return 2;
        }
        g_http_slots[slot].status = status;
        resp[0] = 1u;
        resp[1] = (uint8_t)slot;
        net_put_be16(resp + 2, (uint16_t)status);
        net_put_be32(resp + 4, g_http_slots[slot].len);
        return 8;
    }

    if (op == 'g' && req_len >= 4 && resp_max >= 2) {
        uint8_t h = req[1];
        if (h >= NET_CHAN_MAX_HTTP || !g_http_slots[h].used) {
            resp[0] = 2u; return 1;
        }
        net_http_slot_t *hs = &g_http_slots[h];
        if (hs->off >= hs->len) {
            resp[0] = 1u; return 1;
        }
        uint16_t max_r = net_get_be16(req + 2);
        if (max_r > resp_max - 1u) max_r = (uint16_t)(resp_max - 1u);
        uint32_t avail = hs->len - hs->off;
        uint32_t take = avail < max_r ? avail : max_r;
        memcpy(resp + 1, hs->buf + hs->off, take);
        hs->off += take;
        resp[0] = 0u;
        return (int)(1u + take);
    }

    if (op == 'c' && req_len >= 2) {
        uint8_t h = req[1];
        if (h < NET_CHAN_MAX_HTTP && g_http_slots[h].used) {
            if (g_http_slots[h].buf) {
                palloc_free(g_http_slots[h].buf, NET_HTTP_BUF_PAGES);
            }
            memset(&g_http_slots[h], 0, sizeof(g_http_slots[h]));
        }
        resp[0] = 1u;
        return 1;
    }

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
