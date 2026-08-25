#include "net/ip.h"
#include "net/net_internal.h"
#include <string.h>

/* UDP (R2, plan/phase19_ip_stack_and_ethernet.md).
 *
 * Small enough to be nearly free once IPv4 exists, and it is what a later NTP
 * client (R6) or a DHCP client for a sensor node (§7) would be built on. Both
 * are why it is in this milestone rather than deferred to the one that wants
 * it: the layer costs a page, and adding it later would mean revisiting the
 * dispatch and the checksum in a milestone that should be about something
 * else. */

#define UDP_HDR_LEN 8
/* Four is one per plausible concurrent use -- an NTP client, a DHCP client, a
 * discovery responder, and one spare -- on a system with one application at a
 * time. */
#define UDP_MAX_BINDINGS 4

typedef struct {
    bool in_use;
    uint16_t port;
    udp_recv_fn cb;
    void *ctx;
} udp_binding_t;

static udp_binding_t g_bindings[UDP_MAX_BINDINGS];

int udp_bind(uint16_t port, udp_recv_fn cb, void *ctx) {
    if (port == 0 || !cb) return -1;
    for (uint32_t i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (g_bindings[i].in_use && g_bindings[i].port == port) return -1;   /* taken */
    }
    for (uint32_t i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (!g_bindings[i].in_use) {
            g_bindings[i].in_use = true;
            g_bindings[i].port = port;
            g_bindings[i].cb = cb;
            g_bindings[i].ctx = ctx;
            return 0;
        }
    }
    return -1;
}

int udp_unbind(uint16_t port) {
    for (uint32_t i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (g_bindings[i].in_use && g_bindings[i].port == port) {
            g_bindings[i].in_use = false;
            return 0;
        }
    }
    return -1;
}

uint32_t udp_bindings(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < UDP_MAX_BINDINGS; i++) if (g_bindings[i].in_use) n++;
    return n;
}

/* The UDP checksum covers a pseudo-header of source and destination address,
 * the protocol number and the UDP length -- which is why it cannot be
 * computed without knowing the addresses, and why this takes them as
 * arguments rather than reading them back out of a header. */
static uint16_t udp_fold(const uint8_t src[IPV4_LEN], const uint8_t dst[IPV4_LEN],
                         const uint8_t *udp, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < IPV4_LEN; i += 2) sum += (uint32_t)((uint32_t)src[i] << 8 | src[i + 1]);
    for (uint32_t i = 0; i < IPV4_LEN; i += 2) sum += (uint32_t)((uint32_t)dst[i] << 8 | dst[i + 1]);
    sum += IP_PROTO_UDP;
    sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((uint32_t)udp[i] << 8 | udp[i + 1]);
    if (len & 1u) sum += (uint32_t)udp[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)sum;
}

/* The folded sum, not inverted, is what both directions need -- and keeping
 * it that way is what lets verification sum the datagram *including* its own
 * checksum field and expect all ones, rather than copying the datagram or
 * writing a zero into the receive buffer to make the arithmetic work. */
static uint16_t udp_checksum(const uint8_t src[IPV4_LEN], const uint8_t dst[IPV4_LEN],
                             const uint8_t *udp, uint32_t len) {
    uint16_t ck = (uint16_t)(~udp_fold(src, dst, udp, len) & 0xffffu);
    /* An all-zero checksum means "not computed" on the wire, so a genuine
     * zero is transmitted as 0xffff -- one's-complement arithmetic makes them
     * the same value and the encoding does not. */
    return ck ? ck : 0xffffu;
}

int udp_send(const uint8_t dst_ip[IPV4_LEN], uint16_t dst_port, uint16_t src_port,
             const uint8_t *data, uint32_t len) {
    net_state_t *st = net_mutable_state();
    if (!st->configured || !dst_ip || (len && !data)) return -1;
    uint32_t max = net_tx_payload_max() - IPV4_HDR_LEN - UDP_HDR_LEN;
    if (len > max) return -1;

    uint8_t *u = net_tx_payload() + IPV4_HDR_LEN;
    uint32_t total = UDP_HDR_LEN + len;
    u[0] = (uint8_t)(src_port >> 8); u[1] = (uint8_t)(src_port & 0xff);
    u[2] = (uint8_t)(dst_port >> 8); u[3] = (uint8_t)(dst_port & 0xff);
    u[4] = (uint8_t)(total >> 8);    u[5] = (uint8_t)(total & 0xff);
    u[6] = 0; u[7] = 0;
    if (len) memcpy(u + UDP_HDR_LEN, data, len);

    uint16_t ck = udp_checksum(st->ip, dst_ip, u, total);
    u[6] = (uint8_t)(ck >> 8); u[7] = (uint8_t)(ck & 0xff);

    int rc = ip_send(IP_PROTO_UDP, dst_ip, total);
    if (rc > 0) st->tx_udp++;
    return rc;
}

void udp_input(const uint8_t src[IPV4_LEN], const uint8_t *ip_hdr,
               const uint8_t *p, uint32_t len) {
    net_state_t *st = net_mutable_state();
    if (len < UDP_HDR_LEN) { st->drop_short++; return; }

    uint32_t total = (uint32_t)((uint32_t)p[4] << 8 | p[5]);
    if (total < UDP_HDR_LEN || total > len) { st->drop_short++; return; }

    /* A zero checksum field means the sender did not compute one, which UDP
     * over IPv4 permits. Verify only when there is something to verify.
     *
     * The pseudo-header's destination is the datagram's own destination, read
     * back out of the IP header -- **not** our address. For a unicast they
     * are the same; for a broadcast they are not, and using ours would fail
     * every checksummed broadcast on the segment. */
    if (p[6] || p[7]) {
        if (udp_fold(src, ip_hdr + 16, p, total) != 0xffffu) {
            st->drop_checksum++;
            return;
        }
    }

    uint16_t dst_port = (uint16_t)((uint16_t)p[2] << 8 | p[3]);
    uint16_t src_port = (uint16_t)((uint16_t)p[0] << 8 | p[1]);

    for (uint32_t i = 0; i < UDP_MAX_BINDINGS; i++) {
        if (g_bindings[i].in_use && g_bindings[i].port == dst_port) {
            g_bindings[i].cb(g_bindings[i].ctx, src, src_port,
                             p + UDP_HDR_LEN, total - UDP_HDR_LEN);
            return;
        }
    }

    /* Nobody bound it. Saying so is what turns a silent timeout at the far
     * end into an immediate "connection refused", and it costs one frame.
     * Not sent for a broadcast: answering every broadcast on the segment with
     * an unreachable is how a quiet host becomes a loud one. */
    st->drop_no_port++;
    bool bcast = true;
    for (uint32_t i = 0; i < IPV4_LEN; i++) if (ip_hdr[16 + i] != 0xff) { bcast = false; break; }
    if (!bcast) icmp_port_unreachable(src, ip_hdr, IPV4_HDR_LEN + total);
}
