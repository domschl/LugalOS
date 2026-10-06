/* The kernel's netif for the ESP32-C6 radio (45.7): "wlan0", whole Ethernet frames, over the two
 * rings in the radio's arena (drivers/radio/radio_main.h). The stack above polls and sends as it does
 * for every other interface; nothing here blocks. The radio's domain is the only writer of the RX
 * ring and the only reader of the TX ring; this file is the opposite end of each. */

#include "kernel/radio_c6.h"
#include "kernel/kobj_sched.h"
#include "kernel/printk.h"
#include "net/netif.h"
#include "lugalos_config.h"
#include "../drivers/radio/radio_main.h"
#include <string.h>

#if defined(CONFIG_RADIO_C6)

#define fence() __asm__ volatile("fence rw, rw" ::: "memory")

static radio_ctx_t *g_ctx;
static netif_t g_nif;
static bool g_registered;

static int rn_poll(netif_t *nif) {
    (void)nif;
    fence();
    return g_ctx->rings->rx.head != g_ctx->rings->rx.tail;
}

static int rn_recv(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    (void)nif;
    rn_rings_t *r = g_ctx->rings;
    fence();
    if (r->rx.head == r->rx.tail) return -1;
    const rn_slot_t *s = &r->rx_slot[r->rx.tail % RN_RX_SLOTS];
    uint32_t n = s->len;
    int rc = -1;
    if (n <= max_len && n <= NETIF_FRAME_MAX) { memcpy(buf, s->data, n); rc = (int)n; }
    fence();
    r->rx.tail++;                      /* a frame that does not fit is dropped, not truncated */
    return rc;
}

static int rn_send(netif_t *nif, const uint8_t *buf, uint32_t len) {
    (void)nif;
    rn_rings_t *r = g_ctx->rings;
    if (len == 0 || len > NETIF_FRAME_MAX || !g_ctx->connected) return -1;
    fence();
    uint32_t head = r->tx.head;
    if (head - r->tx.tail >= RN_TX_SLOTS) return -1;           /* the radio is behind: refuse, like a full NIC */
    rn_slot_t *s = &r->tx_slot[head % RN_TX_SLOTS];
    memcpy(s->data, buf, len);
    s->len = len;
    fence();
    r->tx.head = head + 1;
    kos_sem_give((kh_t)g_ctx->tx_sem);
    return (int)len;
}

static bool rn_link_up(netif_t *nif) { (void)nif; return g_ctx && g_ctx->connected; }

netif_t *radio_c6_netif(void) { return g_registered ? &g_nif : NULL; }

int radio_netif_register(radio_ctx_t *ctx) {
    if (g_registered) return 0;
    g_ctx = ctx;
    memset(&g_nif, 0, sizeof g_nif);
    g_nif.name = "wlan0";
    g_nif.poll = rn_poll;
    g_nif.send_frame = rn_send;
    g_nif.recv_frame = rn_recv;
    g_nif.link_up = rn_link_up;
    if (netif_register(&g_nif) != 0) return -1;
    g_registered = true;
    return 0;
}

#endif
