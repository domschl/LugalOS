#ifndef NET_IP_H
#define NET_IP_H

#include <stdint.h>
#include <stdbool.h>
#include "net/netif.h"

/* IPv4, ARP, ICMP and UDP (R2, plan/phase19_ip_stack_and_ethernet.md).
 *
 * The layer phase 18 deliberately did not write, because the W5500 contained
 * one in silicon. Every part that replaced it hands over Ethernet frames, so
 * this is now the product rather than a scaffold -- see the phase 19 plan's
 * §0 for why that argument inverted rather than being overturned.
 *
 * **Addresses are byte arrays in network order throughout**, never packed
 * integers. A small stack spends a surprising fraction of its bugs on byte
 * order, and the cheapest way to spend none is to never convert: what comes
 * off the wire is what is stored, compared and printed.
 *
 * **Single-threaded and synchronous.** Everything below runs from net_poll(),
 * one frame at a time, on one shared transmit buffer. That is what makes a
 * stack this small possible without a packet allocator, and it is a property
 * to preserve deliberately rather than an accident to discover later.
 */

#define IPV4_LEN 4

/* What net_poll() dispatches on. Public so /proc/net can name them. */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct {
    netif_t *nif;
    bool configured;
    uint8_t ip[IPV4_LEN];
    uint8_t mask[IPV4_LEN];
    uint8_t gw[IPV4_LEN];

    /* Counters. Every drop below is a distinct decision the stack made, and
     * they are separate for the reason phase 18 learned the hard way: "the
     * network does not work" is not a diagnosis, and a single `dropped` total
     * cannot become one. */
    uint32_t rx_arp, rx_ip, rx_icmp, rx_udp, rx_tcp;
    uint32_t tx_arp, tx_ip, tx_icmp, tx_udp;
    uint32_t drop_not_for_us;      /* right wire, wrong address */
    uint32_t drop_short;           /* truncated below its own header */
    uint32_t drop_checksum;        /* header checksum failed */
    uint32_t drop_fragment;        /* a fragment; §2 says we do not reassemble */
    uint32_t drop_proto;           /* IP protocol we do not implement */
    uint32_t drop_no_route;        /* no ARP entry and no way to make one */
    uint32_t drop_no_port;         /* UDP to a port nobody bound */
} net_state_t;

/* Binds the stack to an interface. Without an address it will answer ARP for
 * nothing and route nothing; that is deliberate (see net_set_address). */
int net_stack_attach(netif_t *nif);

/* Applies an address. A zero gateway means "no router on this segment", which
 * is a legitimate configuration and not an error. Sends a gratuitous ARP so
 * the segment learns about us without being asked. Returns 0 on success. */
int net_set_address(const uint8_t ip[IPV4_LEN], const uint8_t mask[IPV4_LEN],
                    const uint8_t gw[IPV4_LEN]);

bool net_configured(void);

/* Parses one dotted quad into four bytes. False on anything else -- see the
 * implementation for why a strict parser matters here. */
bool ipv4_parse(const char *s, uint8_t out[IPV4_LEN]);
/* The human-readable report behind both `net` and `(net-status)`. */
void net_print_status(void);
const net_state_t *net_state(void);

/* Receives and dispatches at most `budget` frames. Returns how many it
 * handled. Never blocks -- callers pump it from polling loops, exactly like
 * p9_link_background_poll(). */
uint32_t net_poll(uint32_t budget);

/* --- The unclaimed-frame latch ---
 *
 * The head of the most recent frame the stack did not claim -- anything that
 * is not ARP or IPv4 -- plus a count of how many there have been.
 *
 * It exists because the stack and a bring-up diagnostic cannot both own the
 * receive queue: once `netsrv` runs, a shell command that polls the interface
 * directly loses every race, which is what happened to `net rxtest` the
 * moment R2 landed. A *latch* rather than a callback the diagnostic installs,
 * because a callback also has to be installed before the frame arrives, and
 * requiring an operator to win that race is the same bug wearing a hat.
 *
 * Sixty-four bytes, not a whole frame: what a bring-up asks is "did anything
 * arrive, from whom, of what type", and the addresses and EtherType are in
 * the first fourteen. The full length is reported separately. Keeping it
 * small is what lets this be unconditional instead of a debug build. */
#define NET_UNCLAIMED_HEAD 64
uint32_t net_unclaimed_count(void);
/* Copies the latched head into `out` (NET_UNCLAIMED_HEAD bytes) and returns
 * the frame's full length, or 0 if nothing is latched. Clears the latch. */
uint32_t net_take_unclaimed(uint8_t *out);

/* Starts the pump as a scheduled task ("netsrv"), mirroring
 * p9_server_task_start(). Must follow sched_init(). Returns the pid, or -1
 * when there is no interface to pump. */
int net_task_start(void);

/* --- The shared transmit path ---
 * Callers write their payload at net_tx_payload() and hand the length to
 * net_tx_send(). One buffer, one frame in flight, by construction. */
uint8_t *net_tx_payload(void);
uint32_t net_tx_payload_max(void);
int net_tx_send(const uint8_t dst_mac[NETIF_MAC_LEN], uint16_t ethertype, uint32_t payload_len);

/* --- ARP --- */
/* 0 and fills `mac_out` if `ip` is known. -1 otherwise, having sent a request
 * for it: this stack does not queue the datagram that missed. A caller that
 * cares retries; ICMP and UDP replies almost never miss, because the peer we
 * are answering is the peer we just heard from. §2 records that trade. */
int arp_resolve(const uint8_t ip[IPV4_LEN], uint8_t mac_out[NETIF_MAC_LEN]);
void arp_input(const uint8_t *frame, uint32_t len);
/* Records what an inbound frame already told us. Called from ip_input():
 * every IPv4 frame carries its sender's MAC in the Ethernet header, so a
 * server that is about to reply already knows where to send it. Without this
 * the first reply to every new peer misses the cache, gets dropped, and waits
 * for a retransmission timer -- measurable as a 300 ms stall on the first
 * connection after boot, and entirely avoidable. */
void arp_learn(const uint8_t ip[IPV4_LEN], const uint8_t mac[NETIF_MAC_LEN]);
void arp_announce(void);
uint32_t arp_entries(void);
/* Fills `out` with one cache line's text, "" if the slot is empty. */
void arp_entry_str(uint32_t index, char *out, uint32_t max);

/* --- IPv4 --- */
uint16_t ip_checksum(const uint8_t *data, uint32_t len);
void ip_input(const uint8_t *frame, uint32_t len);
/* Builds and sends one datagram. The payload must already be at
 * net_tx_payload() + 20 (the IPv4 header's size), which lets ICMP and UDP
 * build in place rather than copying twice. */
int ip_send(uint8_t proto, const uint8_t dst[IPV4_LEN], uint32_t payload_len);
#define IPV4_HDR_LEN 20

/* --- ICMP --- */
void icmp_input(const uint8_t src[IPV4_LEN], const uint8_t *p, uint32_t len);
/* Type 3 code 3, quoting the offending datagram's header plus eight bytes,
 * which is what makes a "connection refused" possible at the far end. */
void icmp_port_unreachable(const uint8_t src[IPV4_LEN], const uint8_t *orig_ip_hdr,
                           uint32_t orig_len);

/* --- UDP --- */
typedef void (*udp_recv_fn)(void *ctx, const uint8_t src_ip[IPV4_LEN],
                            uint16_t src_port, const uint8_t *data, uint32_t len);
int udp_bind(uint16_t port, udp_recv_fn cb, void *ctx);
int udp_unbind(uint16_t port);
int udp_send(const uint8_t dst_ip[IPV4_LEN], uint16_t dst_port, uint16_t src_port,
             const uint8_t *data, uint32_t len);
void udp_input(const uint8_t src[IPV4_LEN], const uint8_t *ip_hdr,
               const uint8_t *p, uint32_t len);
uint32_t udp_bindings(void);

#endif // NET_IP_H
