#include "net/ip.h"
#include "net/net_internal.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include <string.h>

/* ARP (R2, plan/phase19_ip_stack_and_ethernet.md).
 *
 * Request, reply, a fixed cache, and a gratuitous announcement on address
 * change. No proxy ARP, and -- see arp_resolve() in net/ip.h -- no queue of
 * datagrams waiting on a resolution. */

#define ARP_HDR_LEN     28
#define ARP_HTYPE_ETH   1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

/* Eight is more hosts than any segment this project runs on will hold, and
 * small enough that a linear scan is the right lookup. */
#define ARP_CACHE_SIZE  8
/* Two minutes. Long enough that a busy exchange never re-resolves, short
 * enough that a host that changed its NIC is not unreachable for an hour. */
#define ARP_TTL_MS      120000u

typedef struct {
    bool in_use;
    uint8_t ip[IPV4_LEN];
    uint8_t mac[NETIF_MAC_LEN];
    uint64_t learned_ms;
} arp_entry_t;

static arp_entry_t g_cache[ARP_CACHE_SIZE];
/* Round-robin victim when every slot is live and unexpired. Evicting the
 * oldest would be better and needs a scan; with eight slots the difference is
 * unmeasurable and this is one line. */
static uint32_t g_next_victim;

static bool expired(const arp_entry_t *e, uint64_t now) {
    return (now - e->learned_ms) > ARP_TTL_MS;
}

static void cache_put(const uint8_t ip[IPV4_LEN], const uint8_t mac[NETIF_MAC_LEN]) {
    uint64_t now = time_get_ms();
    int free_slot = -1;
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && memcmp(g_cache[i].ip, ip, IPV4_LEN) == 0) {
            memcpy(g_cache[i].mac, mac, NETIF_MAC_LEN);
            g_cache[i].learned_ms = now;
            return;
        }
        if (free_slot < 0 && (!g_cache[i].in_use || expired(&g_cache[i], now))) free_slot = (int)i;
    }
    uint32_t slot = (free_slot >= 0) ? (uint32_t)free_slot
                                     : (g_next_victim++ % ARP_CACHE_SIZE);
    g_cache[slot].in_use = true;
    memcpy(g_cache[slot].ip, ip, IPV4_LEN);
    memcpy(g_cache[slot].mac, mac, NETIF_MAC_LEN);
    g_cache[slot].learned_ms = now;
}

/* Builds an ARP packet into the shared transmit payload and sends it. */
static int arp_emit(uint16_t op, const uint8_t target_ip[IPV4_LEN],
                    const uint8_t target_mac[NETIF_MAC_LEN],
                    const uint8_t dst_mac[NETIF_MAC_LEN]) {
    net_state_t *st = net_mutable_state();
    if (!st->nif) return -1;

    uint8_t *p = net_tx_payload();
    p[0] = 0; p[1] = ARP_HTYPE_ETH;
    p[2] = (uint8_t)(ETHERTYPE_IPV4 >> 8); p[3] = (uint8_t)(ETHERTYPE_IPV4 & 0xff);
    p[4] = NETIF_MAC_LEN;
    p[5] = IPV4_LEN;
    p[6] = (uint8_t)(op >> 8); p[7] = (uint8_t)(op & 0xff);
    memcpy(p + 8, st->nif->mac, NETIF_MAC_LEN);
    memcpy(p + 14, st->ip, IPV4_LEN);
    memcpy(p + 18, target_mac, NETIF_MAC_LEN);
    memcpy(p + 24, target_ip, IPV4_LEN);

    int rc = net_tx_send(dst_mac, ETHERTYPE_ARP, ARP_HDR_LEN);
    if (rc > 0) st->tx_arp++;
    return rc;
}

int arp_resolve(const uint8_t ip[IPV4_LEN], uint8_t mac_out[NETIF_MAC_LEN]) {
    uint64_t now = time_get_ms();
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && !expired(&g_cache[i], now) &&
            memcmp(g_cache[i].ip, ip, IPV4_LEN) == 0) {
            memcpy(mac_out, g_cache[i].mac, NETIF_MAC_LEN);
            return 0;
        }
    }
    static const uint8_t zero_mac[NETIF_MAC_LEN] = { 0, 0, 0, 0, 0, 0 };
    arp_emit(ARP_OP_REQUEST, ip, zero_mac, net_broadcast_mac());
    return -1;
}

void arp_announce(void) {
    net_state_t *st = net_mutable_state();
    if (!st->configured) return;
    /* A gratuitous ARP is a request for our *own* address: it asks nothing
     * and updates every cache that hears it. */
    static const uint8_t zero_mac[NETIF_MAC_LEN] = { 0, 0, 0, 0, 0, 0 };
    arp_emit(ARP_OP_REQUEST, st->ip, zero_mac, net_broadcast_mac());
}

void arp_input(const uint8_t *frame, uint32_t len) {
    net_state_t *st = net_mutable_state();
    if (len < ETH_HDR_LEN + ARP_HDR_LEN) { st->drop_short++; return; }

    const uint8_t *p = frame + ETH_HDR_LEN;
    uint16_t htype = (uint16_t)((uint16_t)p[0] << 8 | p[1]);
    uint16_t ptype = (uint16_t)((uint16_t)p[2] << 8 | p[3]);
    if (htype != ARP_HTYPE_ETH || ptype != ETHERTYPE_IPV4 ||
        p[4] != NETIF_MAC_LEN || p[5] != IPV4_LEN) return;

    uint16_t op = (uint16_t)((uint16_t)p[6] << 8 | p[7]);
    const uint8_t *sender_mac = p + 8;
    const uint8_t *sender_ip  = p + 14;
    const uint8_t *target_ip  = p + 24;

    /* Learn from anything addressed to us, request or reply alike -- a host
     * that is asking for our address is a host we are about to answer, and
     * caching it now saves the reverse resolution. Do not learn from traffic
     * merely overheard: a segment full of chatter would otherwise evict the
     * entries actually in use. */
    bool for_us = st->configured && memcmp(target_ip, st->ip, IPV4_LEN) == 0;
    if (for_us) cache_put(sender_ip, sender_mac);

    if (op == ARP_OP_REQUEST && for_us) {
        arp_emit(ARP_OP_REPLY, sender_ip, sender_mac, sender_mac);
    }
}

uint32_t arp_entries(void) {
    uint64_t now = time_get_ms();
    uint32_t n = 0;
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && !expired(&g_cache[i], now)) n++;
    }
    return n;
}

void arp_entry_str(uint32_t index, char *out, uint32_t max) {
    if (!out || max == 0) return;
    out[0] = '\0';
    if (index >= ARP_CACHE_SIZE || !g_cache[index].in_use) return;
    if (expired(&g_cache[index], time_get_ms())) return;

    char mac[18];
    netif_mac_str(g_cache[index].mac, mac);
    const uint8_t *a = g_cache[index].ip;
    ksnprintf(out, max, "%u.%u.%u.%u %s\n", a[0], a[1], a[2], a[3], mac);
}
