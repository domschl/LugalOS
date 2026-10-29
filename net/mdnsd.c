#include "net/mdnsd.h"
#include "net/ip.h"
#include "kernel/identity.h"
#include <stddef.h>
#include <string.h>

#define DNS_HDR 12u
#define MDNS_PORT 5353u
#define MDNS_TTL_S 120u

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool mdnsd_label_ok(const char *label) {
    size_t n = label ? strlen(label) : 0;
    if (n == 0 || n > 63) return false;
    for (size_t i = 0; i < n; i++) {
        char c = lower(label[i]);
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return label[0] != '-' && label[n - 1] != '-';
}

/* The name at q[*pos] is "<host>.local" (no compression in a question we answer): advances *pos past it. */
static bool name_is_ours(const uint8_t *q, uint32_t len, uint32_t *pos, const char *host) {
    uint32_t p = *pos;
    size_t hl = strlen(host);
    if (p >= len || q[p] != hl || p + 1 + hl > len) return false;
    for (size_t i = 0; i < hl; i++) if (lower((char)q[p + 1 + i]) != lower(host[i])) return false;
    p += 1 + (uint32_t)hl;
    if (p + 6 > len || q[p] != 5) return false;
    for (uint32_t i = 0; i < 5; i++) if (lower((char)q[p + 1 + i]) != "local"[i]) return false;
    p += 6;
    if (p >= len || q[p] != 0) return false;
    *pos = p + 1;
    return true;
}

uint32_t mdnsd_handle(const uint8_t *q, uint32_t len, const char *host, const uint8_t ip[4],
                      uint8_t *out, uint32_t cap, bool *unicast, uint16_t src_port) {
    if (!q || !host || !ip || !out || !unicast || cap < MDNSD_MAX_REPLY || len < DNS_HDR) return 0;
    if (!mdnsd_label_ok(host)) return 0;
    if (q[2] & 0x80) return 0;                               /* a response, not a query */
    uint32_t qd = (uint32_t)q[4] << 8 | q[5];
    uint32_t pos = DNS_HDR;
    for (uint32_t i = 0; i < qd && i < 8; i++) {
        uint32_t start = pos;
        bool ours = name_is_ours(q, len, &pos, host);
        if (!ours) {
            /* Skip this question's name to reach the next one. */
            pos = start;
            for (;;) {
                if (pos >= len) return 0;
                uint8_t l = q[pos];
                if (l == 0) { pos++; break; }
                if (l & 0xC0) { pos += 2; break; }
                pos += 1u + l;
            }
        }
        if (pos + 4 > len) return 0;
        uint16_t qtype = (uint16_t)((uint16_t)q[pos] << 8 | q[pos + 1]);
        uint16_t qclass = (uint16_t)((uint16_t)q[pos + 2] << 8 | q[pos + 3]);
        pos += 4;
        if (!ours || (qclass & 0x7FFF) != 1 || (qtype != 1 && qtype != 255)) continue;

        uint32_t n = 0;
        out[n++] = 0; out[n++] = 0;                          /* id: zero in an mDNS response */
        out[n++] = 0x84; out[n++] = 0;                       /* response, authoritative */
        out[n++] = 0; out[n++] = 0;                          /* no questions */
        out[n++] = 0; out[n++] = 1;                          /* one answer */
        out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 0;
        size_t hl = strlen(host);
        out[n++] = (uint8_t)hl;
        for (size_t k = 0; k < hl; k++) out[n++] = (uint8_t)lower(host[k]);
        out[n++] = 5; memcpy(out + n, "local", 5); n += 5;
        out[n++] = 0;
        out[n++] = 0; out[n++] = 1;                          /* A */
        out[n++] = 0x80; out[n++] = 1;                       /* IN, cache flush */
        out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = MDNS_TTL_S;
        out[n++] = 0; out[n++] = 4;
        memcpy(out + n, ip, 4); n += 4;
        *unicast = (qclass & 0x8000) != 0 || src_port != MDNS_PORT;
        return n;
    }
    return 0;
}

static void mdns_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port, const uint8_t *data, uint32_t len) {
    (void)ctx;
    const char *host = node_name();
    const net_state_t *st = net_state();
    if (!st || !st->configured) return;
    uint8_t out[MDNSD_MAX_REPLY];
    bool uni = false;
    uint32_t n = mdnsd_handle(data, len, host, st->ip, out, sizeof out, &uni, src_port);
    if (!n) return;
    static const uint8_t group[IPV4_LEN] = { 224, 0, 0, 251 };
    (void)udp_send(uni ? src_ip : group, uni ? src_port : MDNS_PORT, MDNS_PORT, out, n);
}

int mdnsd_start(void) {
    return udp_bind(MDNS_PORT, mdns_cb, NULL);
}
