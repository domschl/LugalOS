#include "net/dns.h"
#include "kernel/lock.h"
#include "net/ip.h"
#include "fs/vfs.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/random.h"
#include "kernel/console.h"
#include "libc/include/string.h"

#define DNS_PORT 53
#define DNS_CACHE_SIZE 4
#define DNS_TIMEOUT_MS 2500

typedef struct {
    char     name[64];
    uint8_t  ip[IPV4_LEN];
    uint64_t expires_ms;
} dns_cache_entry_t;

/* guarded by: g_dns_lock (dns_resolve() holds it). */
static dns_cache_entry_t g_dns_cache[DNS_CACHE_SIZE];
static uint32_t          g_dns_cache_idx = 0;

static uint8_t g_explicit_dns[IPV4_LEN];
static bool    g_has_explicit_dns = false;

typedef struct {
    volatile bool got;
    uint16_t      xid;
    uint8_t       ip[IPV4_LEN];
} dns_reply_t;

/* guarded by: g_dns_lock (dns_resolve() holds it). */
static dns_reply_t g_dns_reply;
/* guarded by: g_dns_lock, held for a whole resolve: one reply slot, and two
 * terminals resolving at once would take each other's answer. The reply
 * callback (the stack task) only fills the slot of the query in flight. */
static ylock_t g_dns_lock;

void dns_set_server(const uint8_t ip[IPV4_LEN]) {
    if (!ip) {
        g_has_explicit_dns = false;
        return;
    }
    memcpy(g_explicit_dns, ip, IPV4_LEN);
    g_has_explicit_dns = true;
}

bool dns_get_server(uint8_t ip_out[IPV4_LEN]) {
    if (!ip_out) return false;
    if (g_has_explicit_dns) {
        memcpy(ip_out, g_explicit_dns, IPV4_LEN);
        return true;
    }
    if (dhcp_dns_server(ip_out)) {
        return true;
    }
    const net_state_t *st = net_state();
    if (st && st->configured && (st->gw[0] || st->gw[1] || st->gw[2] || st->gw[3])) {
        memcpy(ip_out, st->gw, IPV4_LEN);
        return true;
    }
    return false;
}

/* Parse static hosts file (/sd0/etc/hosts or /flash0/etc/hosts) */
static bool check_hosts_file(const char *path, const char *name, uint8_t ip_out[IPV4_LEN]) {
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return false;

    char line[128];
    uint32_t line_pos = 0;
    uint8_t buf[256];
    uint64_t offset = 0;
    bool found = false;

    for (;;) {
        int rd = vfs_pread(fd, buf, sizeof(buf), offset);
        if (rd <= 0) break;
        offset += (uint64_t)rd;

        for (int i = 0; i < rd; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                line[line_pos] = '\0';
                line_pos = 0;

                /* Parse line: "<ip> <host1> [<host2>...]" */
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\0') continue;

                char *ip_str = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (!*p) continue;
                *p++ = '\0';

                while (*p) {
                    while (*p == ' ' || *p == '\t') p++;
                    if (*p == '#' || *p == '\0') break;
                    char *h = p;
                    while (*p && *p != ' ' && *p != '\t' && *p != '#') p++;
                    char end_c = *p;
                    *p = '\0';

                    if (strcmp(h, name) == 0) {
                        if (ipv4_parse(ip_str, ip_out)) {
                            found = true;
                            break;
                        }
                    }
                    if (end_c == '\0' || end_c == '#') break;
                    p++;
                }
                if (found) break;
            } else if (line_pos < sizeof(line) - 1) {
                line[line_pos++] = c;
            }
        }
        if (found) break;
    }

    vfs_close(fd);
    return found;
}

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void dns_rx_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port,
                      const uint8_t *d, uint32_t len) {
    (void)ctx; (void)src_ip; (void)src_port;
    if (len < 12 || g_dns_reply.got) return;

    uint16_t id = get_be16(d);
    if (id != g_dns_reply.xid) return;

    uint16_t flags = get_be16(d + 2);
    /* QR bit must be 1 (reply), RCODE (bits 0..3) must be 0 (no error) */
    if ((flags & 0x8000) == 0 || (flags & 0x000F) != 0) return;

    uint16_t qdcount = get_be16(d + 4);
    uint16_t ancount = get_be16(d + 6);
    if (ancount == 0) return;

    uint32_t p = 12;

    /* Skip question records */
    for (uint16_t q = 0; q < qdcount && p < len; q++) {
        while (p < len && d[p] != 0) {
            if ((d[p] & 0xC0) == 0xC0) { p += 2; goto q_done; }
            p += (uint32_t)d[p] + 1;
        }
        if (p < len && d[p] == 0) p++;
    q_done:
        p += 4; /* QTYPE + QCLASS */
    }

    /* Parse answer records */
    for (uint16_t a = 0; a < ancount && p + 10 <= len; a++) {
        /* Skip name (labels or pointer) */
        if ((d[p] & 0xC0) == 0xC0) {
            p += 2;
        } else {
            while (p < len && d[p] != 0) {
                if ((d[p] & 0xC0) == 0xC0) { p += 2; break; }
                p += (uint32_t)d[p] + 1;
            }
            if (p < len && d[p] == 0) p++;
        }

        if (p + 10 > len) break;
        uint16_t rtype = get_be16(d + p);
        uint16_t rclass = get_be16(d + p + 2);
        /* skip TTL (4 bytes) */
        uint16_t rdlen = get_be16(d + p + 8);
        p += 10;

        if (p + rdlen > len) break;

        /* Type 1 = A, Class 1 = IN, rdlen = 4 */
        if (rtype == 1 && rclass == 1 && rdlen == 4) {
            memcpy(g_dns_reply.ip, d + p, 4);
            g_dns_reply.got = true;
            return;
        }
        p += rdlen;
    }
}

static bool dns_resolve_locked(const char *name, uint8_t ip_out[IPV4_LEN]) {
    if (!name || !*name || !ip_out) return false;

    /* 1. If name is already an IPv4 dotted quad string, parse directly */
    if (ipv4_parse(name, ip_out)) {
        return true;
    }

    /* 2. Check local DNS cache */
    uint64_t now = time_get_ms();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (g_dns_cache[i].name[0] && strcmp(g_dns_cache[i].name, name) == 0) {
            if (g_dns_cache[i].expires_ms > now) {
                memcpy(ip_out, g_dns_cache[i].ip, IPV4_LEN);
                return true;
            }
        }
    }

    /* 3. Check static hosts files (/sd0/etc/hosts then /flash0/etc/hosts) */
    if (check_hosts_file("/sd0/etc/hosts", name, ip_out) ||
        check_hosts_file("/flash0/etc/hosts", name, ip_out)) {
        /* Cache result for 60 seconds */
        strncpy(g_dns_cache[g_dns_cache_idx].name, name, sizeof(g_dns_cache[0].name) - 1);
        g_dns_cache[g_dns_cache_idx].name[sizeof(g_dns_cache[0].name) - 1] = '\0';
        memcpy(g_dns_cache[g_dns_cache_idx].ip, ip_out, IPV4_LEN);
        g_dns_cache[g_dns_cache_idx].expires_ms = now + 60000;
        g_dns_cache_idx = (g_dns_cache_idx + 1) % DNS_CACHE_SIZE;
        return true;
    }

    /* 4. Query upstream DNS server via UDP 53 */
    uint8_t server_ip[IPV4_LEN];
    if (!dns_get_server(server_ip)) {
        return false;
    }

    /* Format DNS query packet (RFC 1035) */
    uint8_t pkt[256];
    uint16_t xid;
    random_bytes(&xid, sizeof(xid));
    if (xid == 0) xid = 0x1234;

    put_be16(pkt + 0, xid);
    put_be16(pkt + 2, 0x0100); /* RD = 1 (Recursion Desired) */
    put_be16(pkt + 4, 1);      /* QDCOUNT = 1 */
    put_be16(pkt + 6, 0);      /* ANCOUNT = 0 */
    put_be16(pkt + 8, 0);      /* NSCOUNT = 0 */
    put_be16(pkt + 10, 0);     /* ARCOUNT = 0 */

    uint32_t p = 12;
    const char *s = name;

    /* Encode labels: "ai-server.local" -> 9ai-server5local0 */
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t label_len = dot ? (size_t)(dot - s) : strlen(s);
        if (label_len == 0 || label_len > 63 || p + label_len + 1 >= sizeof(pkt) - 4) {
            return false;
        }
        pkt[p++] = (uint8_t)label_len;
        memcpy(pkt + p, s, label_len);
        p += (uint32_t)label_len;
        if (!dot) break;
        s = dot + 1;
    }
    pkt[p++] = 0; /* Root label */

    put_be16(pkt + p, 1);     /* QTYPE = A */
    put_be16(pkt + p + 2, 1); /* QCLASS = IN */
    p += 4;

    uint16_t client_port = (uint16_t)(53000 + (xid & 0x03FF));
    g_dns_reply.got = false;
    g_dns_reply.xid = xid;

    if (udp_bind(client_port, dns_rx_cb, NULL) != 0) {
        return false;
    }

    if (udp_send(server_ip, DNS_PORT, client_port, pkt, p) <= 0) {
        udp_unbind(client_port);
        return false;
    }

    uint64_t deadline = time_get_ms() + DNS_TIMEOUT_MS;
    while (!g_dns_reply.got && time_get_ms() < deadline) {
        if (console_interrupt_requested()) break;
        sched_yield();
    }

    udp_unbind(client_port);

    if (g_dns_reply.got) {
        memcpy(ip_out, g_dns_reply.ip, IPV4_LEN);

        /* Store in cache */
        strncpy(g_dns_cache[g_dns_cache_idx].name, name, sizeof(g_dns_cache[0].name) - 1);
        g_dns_cache[g_dns_cache_idx].name[sizeof(g_dns_cache[0].name) - 1] = '\0';
        memcpy(g_dns_cache[g_dns_cache_idx].ip, ip_out, IPV4_LEN);
        g_dns_cache[g_dns_cache_idx].expires_ms = time_get_ms() + 30000;
        g_dns_cache_idx = (g_dns_cache_idx + 1) % DNS_CACHE_SIZE;
        return true;
    }

    return false;
}

/* One query at a time, under g_dns_lock. */
bool dns_resolve(const char *name, uint8_t ip_out[IPV4_LEN]) {
    ylock_acquire(&g_dns_lock);
    bool r = dns_resolve_locked(name, ip_out);
    ylock_release(&g_dns_lock);
    return r;
}
