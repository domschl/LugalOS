#ifndef NET_NETIF_H
#define NET_NETIF_H

#include <stdint.h>
#include <stdbool.h>

/* A frame source (R1, plan/phase19_ip_stack_and_ethernet.md).
 *
 * The seam below the IP stack, and deliberately the same shape as
 * `p9_link_t` (fs/include/fs/p9_link.h): a name, a non-blocking `poll()`, a
 * send, a receive, an opaque `ctx`. That is not imitation for its own sake --
 * this tree already has one transport abstraction that every reviewer knows
 * how to read, and a second one with different conventions would be a second
 * thing to learn for no gain. The only new obligations are that a netif
 * carries whole **Ethernet frames** rather than 9P messages, and that it has
 * a MAC address and a link state.
 *
 * The layering this sits in:
 *
 *     9P server            (fs/9p.c)
 *     p9_link_t            <- an accepted TCP connection is one of these (R3)
 *     net/ip.c, net/tcp.c  <- the stack (R2, R3)
 *     netif_t              <- HERE
 *     drivers/virtio_net.c | drivers/enc28j60.c | drivers/cyw43.c
 *
 * Phase 18's W5500 skipped every layer between the driver and `p9_link_t` by
 * terminating TCP in silicon. Nothing else will, which is why this seam
 * exists at all -- see the phase 19 plan's §0.
 */

/* 1500 bytes of payload, 14 of Ethernet II header, no FCS: neither virtio-net
 * nor the ENC28J60 hands one up (both check and strip it), so counting four
 * bytes nobody ever sees would only make every buffer in the stack wrong by
 * the same amount. */
#define NETIF_MTU         1500u
#define NETIF_FRAME_MAX   1514u
#define NETIF_MAC_LEN     6u

typedef struct netif {
    const char *name;

    /* Filled by the driver before netif_register(). */
    uint8_t mac[NETIF_MAC_LEN];

    /* Pumps the driver's RX path. Returns 1 once a *complete* frame is
     * buffered and ready for recv_frame(), 0 if nothing is ready, -1 on a
     * transport error. Must never block: callers invoke it from polling
     * loops, exactly as p9_link_t's poll() is used. */
    int (*poll)(struct netif *nif);

    /* Sends one complete Ethernet frame (destination MAC first, no FCS).
     * Returns the byte count sent, or -1. May block only for as long as the
     * hardware needs to accept the buffer -- never for an acknowledgement
     * from the far end, which is not a thing at this layer. */
    int (*send_frame)(struct netif *nif, const uint8_t *buf, uint32_t len);

    /* Copies one buffered frame out. Returns its length, or -1 if none is
     * ready or it does not fit in `max_len`. */
    int (*recv_frame)(struct netif *nif, uint8_t *buf, uint32_t max_len);

    /* Carrier state. NULL means "always up" -- true of a virtual device and
     * of nothing with a socket on it. */
    bool (*link_up)(struct netif *nif);

    void *ctx;

    /* Owned by net/netif.c, not by drivers: every count below is made by the
     * netif_send()/netif_recv() wrappers, so two drivers cannot disagree
     * about what "a dropped frame" means. A driver that counts its own
     * hardware-level errors puts them somewhere of its own. */
    uint32_t rx_frames, tx_frames;
    uint32_t rx_bytes, tx_bytes;
    uint32_t rx_dropped;   /* a frame arrived and we had nowhere to put it */
    uint32_t tx_errors;    /* send_frame() refused or failed */
} netif_t;

/* Up to this many interfaces at once. Two is one more than any board in this
 * tree has, and the second slot is what makes a gateway with both a wire and
 * a radio expressible without a rebuild. */
#define NETIF_MAX 2

/* Registers `nif`. The driver owns the storage (a static, as everywhere else
 * in this kernel -- there is no allocation on this path). Returns 0, or -1 if
 * the table is full or the name is already taken. Zeroes the counters. */
int netif_register(netif_t *nif);

uint32_t netif_count(void);
netif_t *netif_at(uint32_t index);
netif_t *netif_find(const char *name);

/* The interface the stack uses when nothing says otherwise: the first
 * registered one. NULL if there is none, which is the normal state of every
 * board in this tree until R4 fits a part. */
netif_t *netif_default(void);

/* The counted wrappers. Prefer these over calling the function pointers
 * directly; the direct call is not wrong, it just does not count. */
int netif_send(netif_t *nif, const uint8_t *buf, uint32_t len);
int netif_poll(netif_t *nif);
int netif_recv(netif_t *nif, uint8_t *buf, uint32_t max_len);
bool netif_link_up(netif_t *nif);

/* Formats `mac` as aa:bb:cc:dd:ee:ff into `out` (at least 18 bytes). */
void netif_mac_str(const uint8_t mac[NETIF_MAC_LEN], char *out);

#endif // NET_NETIF_H
