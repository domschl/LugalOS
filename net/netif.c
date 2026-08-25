#include "net/netif.h"
#include "kernel/printk.h"
#include "kernel/identity.h"
#include <string.h>

static netif_t *g_netifs[NETIF_MAX];
static uint32_t g_num_netifs;

int netif_register(netif_t *nif) {
    if (!nif || !nif->name || !nif->poll || !nif->send_frame || !nif->recv_frame) return -1;
    /* A driver that could not get a MAC from its own hardware leaves the
     * field zeroed and gets the node's. Drivers whose part supplies one --
     * virtio's config space, an EEPROM on a NIC -- keep it: the platform
     * saying "you are this address" outranks anything we can derive. */
    bool have_mac = false;
    for (uint32_t i = 0; i < NETIF_MAC_LEN; i++) if (nif->mac[i]) { have_mac = true; break; }
    if (!have_mac) memcpy(nif->mac, node_mac(), NETIF_MAC_LEN);
    if (g_num_netifs >= NETIF_MAX) {
        printk("[Net] No free interface slot for '%s' (max %u).\n", nif->name, NETIF_MAX);
        return -1;
    }
    for (uint32_t i = 0; i < g_num_netifs; i++) {
        if (strcmp(g_netifs[i]->name, nif->name) == 0) {
            printk("[Net] An interface named '%s' is already registered.\n", nif->name);
            return -1;
        }
    }
    nif->rx_frames = nif->tx_frames = 0;
    nif->rx_bytes = nif->tx_bytes = 0;
    nif->rx_dropped = nif->tx_errors = 0;
    g_netifs[g_num_netifs++] = nif;
    return 0;
}

uint32_t netif_count(void) { return g_num_netifs; }

netif_t *netif_at(uint32_t index) {
    return (index < g_num_netifs) ? g_netifs[index] : NULL;
}

netif_t *netif_find(const char *name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_num_netifs; i++) {
        if (strcmp(g_netifs[i]->name, name) == 0) return g_netifs[i];
    }
    return NULL;
}

netif_t *netif_default(void) {
    return g_num_netifs ? g_netifs[0] : NULL;
}

int netif_send(netif_t *nif, const uint8_t *buf, uint32_t len) {
    if (!nif || !buf) return -1;
    /* 60 rather than 64: the FCS is not ours to count (see NETIF_FRAME_MAX).
     * Anything shorter is a caller bug rather than something to pad over
     * silently -- padding a runt would hide the bug in the layer above. */
    if (len < 14u || len > NETIF_FRAME_MAX) { nif->tx_errors++; return -1; }

    int n = nif->send_frame(nif, buf, len);
    if (n < 0) { nif->tx_errors++; return -1; }
    nif->tx_frames++;
    nif->tx_bytes += (uint32_t)n;
    return n;
}

int netif_poll(netif_t *nif) {
    return nif ? nif->poll(nif) : -1;
}

int netif_recv(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    if (!nif || !buf) return -1;
    int n = nif->recv_frame(nif, buf, max_len);
    if (n < 0) return -1;
    nif->rx_frames++;
    nif->rx_bytes += (uint32_t)n;
    return n;
}

bool netif_link_up(netif_t *nif) {
    if (!nif) return false;
    return nif->link_up ? nif->link_up(nif) : true;
}

static char hex_digit(uint8_t v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

void netif_mac_str(const uint8_t mac[NETIF_MAC_LEN], char *out) {
    if (!mac || !out) return;
    uint32_t o = 0;
    for (uint32_t i = 0; i < NETIF_MAC_LEN; i++) {
        out[o++] = hex_digit((uint8_t)(mac[i] >> 4));
        out[o++] = hex_digit((uint8_t)(mac[i] & 0x0f));
        if (i + 1 < NETIF_MAC_LEN) out[o++] = ':';
    }
    out[o] = '\0';
}
