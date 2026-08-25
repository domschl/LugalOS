#include "net/ip.h"
#include "net/net_internal.h"
#include <string.h>

/* ICMP (R2, plan/phase19_ip_stack_and_ethernet.md): echo reply, and
 * destination-unreachable for a port nobody bound.
 *
 * Fifty lines that pay for themselves the first time somebody asks whether
 * the board is on the network, because `ping` is what they will type. From
 * this milestone on that answer comes from our code rather than from a chip's
 * firmware, which is the difference the whole phase is about. */

#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO_REQUEST 8
#define ICMP_CODE_PORT_UNREACH 3
#define ICMP_HDR_LEN      8

void icmp_input(const uint8_t src[IPV4_LEN], const uint8_t *p, uint32_t len) {
    net_state_t *st = net_mutable_state();
    if (len < ICMP_HDR_LEN) { st->drop_short++; return; }
    if (ip_checksum(p, len) != 0) { st->drop_checksum++; return; }
    if (p[0] != ICMP_ECHO_REQUEST || p[1] != 0) return;   /* not ours to answer */

    uint32_t max = net_tx_payload_max() - IPV4_HDR_LEN;
    if (len > max) { st->drop_short++; return; }

    /* An echo reply is the request with the type changed and the checksum
     * redone -- identifier, sequence and the whole payload are echoed back
     * verbatim, which is what lets `ping` match replies to requests and
     * report a round-trip time. */
    /* memcpy is safe here and the buffers really are distinct: `p` points
     * into the stack's receive frame and `out` into its transmit frame, which
     * net/stack.c keeps as two separate statics. */
    uint8_t *out = net_tx_payload() + IPV4_HDR_LEN;
    memcpy(out, p, len);
    out[0] = ICMP_ECHO_REPLY;
    out[2] = 0; out[3] = 0;
    uint16_t ck = ip_checksum(out, len);
    out[2] = (uint8_t)(ck >> 8); out[3] = (uint8_t)(ck & 0xff);

    if (ip_send(IP_PROTO_ICMP, src, len) > 0) st->tx_icmp++;
}

void icmp_port_unreachable(const uint8_t src[IPV4_LEN], const uint8_t *orig_ip_hdr,
                           uint32_t orig_len) {
    net_state_t *st = net_mutable_state();
    if (!orig_ip_hdr) return;

    /* RFC 792: the message quotes the offending datagram's IP header plus the
     * first eight bytes of its payload -- which for UDP is the whole header,
     * and therefore the ports. That is what lets the far end turn this into
     * "connection refused" for the right socket instead of a vague failure. */
    uint32_t quote = orig_len < IPV4_HDR_LEN + 8u ? orig_len : IPV4_HDR_LEN + 8u;
    uint32_t len = ICMP_HDR_LEN + quote;
    if (len > net_tx_payload_max() - IPV4_HDR_LEN) return;

    /* Same distinct-buffers argument as icmp_input() above: the quote comes
     * out of the receive frame, the message is built in the transmit frame. */
    uint8_t *out = net_tx_payload() + IPV4_HDR_LEN;
    out[0] = ICMP_DEST_UNREACH;
    out[1] = ICMP_CODE_PORT_UNREACH;
    out[2] = 0; out[3] = 0;
    memset(out + 4, 0, 4);                  /* unused for code 3 */
    memcpy(out + ICMP_HDR_LEN, orig_ip_hdr, quote);

    uint16_t ck = ip_checksum(out, len);
    out[2] = (uint8_t)(ck >> 8); out[3] = (uint8_t)(ck & 0xff);

    if (ip_send(IP_PROTO_ICMP, src, len) > 0) st->tx_icmp++;
}
