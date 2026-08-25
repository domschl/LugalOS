#include "net/ip.h"
#include "net/net_internal.h"
#include <string.h>

/* IPv4 (R2, plan/phase19_ip_stack_and_ethernet.md).
 *
 * Unicast in and out, the header checksum, and a hard refusal to reassemble.
 * §2 of the plan states the trade: a fragment is counted and dropped, and we
 * never emit one, because a reassembly queue is both the largest RAM line
 * item and the largest bug surface in a small stack. On a LAN with a 2 KB
 * msize nothing legitimately fragments. */

#define IP_VERSION_IHL   0x45   /* v4, 5 words of header, no options */
#define IP_DEFAULT_TTL   64
#define IP_FLAG_MF       0x2000
#define IP_FRAG_MASK     0x1fff

uint16_t ip_checksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)((uint32_t)data[i] << 8 | data[i + 1]);
    }
    if (len & 1u) sum += (uint32_t)data[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/* Where the next hop for `dst` lives: on our own subnet, or behind the
 * gateway. A zero gateway with an off-subnet destination is not an error at
 * configuration time (an isolated segment is a legitimate setup) but it is
 * one here, and it is counted rather than silently broadcast. */
static int next_hop(const uint8_t dst[IPV4_LEN], uint8_t out[IPV4_LEN]) {
    const net_state_t *st = net_state();
    bool same_subnet = true;
    for (uint32_t i = 0; i < IPV4_LEN; i++) {
        if ((dst[i] & st->mask[i]) != (st->ip[i] & st->mask[i])) { same_subnet = false; break; }
    }
    if (same_subnet) { memcpy(out, dst, IPV4_LEN); return 0; }

    bool have_gw = false;
    for (uint32_t i = 0; i < IPV4_LEN; i++) if (st->gw[i]) { have_gw = true; break; }
    if (!have_gw) return -1;
    memcpy(out, st->gw, IPV4_LEN);
    return 0;
}

int ip_send(uint8_t proto, const uint8_t dst[IPV4_LEN], uint32_t payload_len) {
    net_state_t *st = net_mutable_state();
    if (!st->configured) return -1;
    if (payload_len > net_tx_payload_max() - IPV4_HDR_LEN) return -1;

    /* Broadcast goes to the broadcast MAC without consulting ARP -- there is
     * nothing to resolve, and asking would fail. */
    uint8_t dst_mac[NETIF_MAC_LEN];
    bool is_broadcast = true;
    for (uint32_t i = 0; i < IPV4_LEN; i++) if (dst[i] != 0xff) { is_broadcast = false; break; }
    if (is_broadcast) {
        memcpy(dst_mac, net_broadcast_mac(), NETIF_MAC_LEN);
    } else {
        uint8_t hop[IPV4_LEN];
        if (next_hop(dst, hop) != 0) { st->drop_no_route++; return -1; }
        if (arp_resolve(hop, dst_mac) != 0) { st->drop_no_route++; return -1; }
    }

    uint8_t *h = net_tx_payload();
    uint32_t total = IPV4_HDR_LEN + payload_len;
    h[0] = IP_VERSION_IHL;
    h[1] = 0;                                    /* no DSCP, no ECN */
    h[2] = (uint8_t)(total >> 8); h[3] = (uint8_t)(total & 0xff);
    /* Identification is zero, and legitimately so: it only ever matters for
     * reassembly, and this stack never fragments (RFC 6864 §4.1 permits a
     * constant on non-fragmented datagrams). */
    h[4] = 0; h[5] = 0;
    h[6] = 0x40; h[7] = 0;                       /* Don't Fragment */
    h[8] = IP_DEFAULT_TTL;
    h[9] = proto;
    h[10] = 0; h[11] = 0;                        /* checksum, filled below */
    memcpy(h + 12, st->ip, IPV4_LEN);
    memcpy(h + 16, dst, IPV4_LEN);

    uint16_t ck = ip_checksum(h, IPV4_HDR_LEN);
    h[10] = (uint8_t)(ck >> 8); h[11] = (uint8_t)(ck & 0xff);

    int rc = net_tx_send(dst_mac, ETHERTYPE_IPV4, total);
    if (rc > 0) st->tx_ip++;
    return rc;
}

void ip_input(const uint8_t *frame, uint32_t len) {
    net_state_t *st = net_mutable_state();
    if (len < ETH_HDR_LEN + IPV4_HDR_LEN) { st->drop_short++; return; }

    const uint8_t *h = frame + ETH_HDR_LEN;
    uint32_t avail = len - ETH_HDR_LEN;

    if ((h[0] >> 4) != 4) { st->drop_short++; return; }
    uint32_t ihl = (uint32_t)(h[0] & 0x0f) * 4u;
    if (ihl < IPV4_HDR_LEN || ihl > avail) { st->drop_short++; return; }

    uint32_t total = (uint32_t)((uint32_t)h[2] << 8 | h[3]);
    if (total < ihl || total > avail) {
        /* Shorter than its own header, or claiming more than arrived. The
         * second is the interesting one: a frame padded to 60 bytes makes
         * `avail` larger than `total`, which is fine, but the reverse means
         * the sender and the wire disagree. */
        st->drop_short++;
        return;
    }

    if (ip_checksum(h, ihl) != 0) { st->drop_checksum++; return; }

    uint16_t frag = (uint16_t)((uint16_t)h[6] << 8 | h[7]);
    if ((frag & IP_FLAG_MF) || (frag & IP_FRAG_MASK)) { st->drop_fragment++; return; }

    /* Ours, or the segment's broadcast? Anything else on this wire is not our
     * business even though the MAC filter let it through (a promiscuous
     * device, or a broadcast MAC carrying a unicast IP). */
    bool mine = st->configured && memcmp(h + 16, st->ip, IPV4_LEN) == 0;
    bool bcast = true;
    for (uint32_t i = 0; i < IPV4_LEN; i++) if (h[16 + i] != 0xff) { bcast = false; break; }
    if (!mine && !bcast) { st->drop_not_for_us++; return; }

    const uint8_t *src = h + 12;
    const uint8_t *payload = h + ihl;
    uint32_t payload_len = total - ihl;

    switch (h[9]) {
        case IP_PROTO_ICMP:
            st->rx_icmp++;
            icmp_input(src, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            st->rx_udp++;
            udp_input(src, h, payload, payload_len);
            break;
        default:
            /* TCP lands here until R3, which is the honest state of affairs
             * and worth a counter rather than silence. */
            st->drop_proto++;
            break;
    }
}
