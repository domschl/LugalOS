/* An LLP node: the binding between kernel/llp.c and this kernel (49.2,
 * plan/phase49_c6_network_nodes.md). kernel/include/kernel/llp_node.h says
 * what it does; this file is how. */

#include "kernel/llp_node.h"
#include "kernel/llp.h"
#include "kernel/sched.h"
#include "kernel/lock.h"
#include "kernel/time.h"
#include "kernel/random.h"
#include "kernel/device.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "net/netif.h"
#include "net/ip.h"
#include "fs/9p.h"
#include "kernel/palloc.h"
#include <string.h>

/* --- The channel ----------------------------------------------------------------
 * The serial binding's byte stream: what a board has to provide. QEMU: its
 * virtio-console, taken off the 9P pump. The C6-Zero: its one USB port, shared
 * with the console (drivers/usbjtag_esp32c6.c, 49.3). */

#if defined(CONFIG_BOARD_ESP32C6)
#include "drivers/usbjtag_esp32c6.h"
static int  chan_read(uint8_t *buf, uint32_t max) { return usbjtag_llp_read(buf, max); }
static bool chan_present(void) { return true; }
static void chan_claim(void) { usbjtag_llp_arm(true); }
static void chan_service(void) { usbjtag_llp_service(); }
/* Frames are streamed to the FIFO rather than encoded into a buffer first:
 * the buffer would be 3 of the ~11 pages a C6 has free with the radio up. */
#define CHAN_STREAMS 1
static void chan_put(void *ctx, uint8_t b) { (void)ctx; usbjtag_llp_putc(b); }
static void chan_end(void) { usbjtag_llp_end(); }
/* The C6 is only ever the radio, and its SRAM is the tightest budget here:
 * no Ethernet queues (frames go straight to the radio), two 9P slots in and
 * one out (its 9P server answers one request at a time), a one-page task. */
#define ETH_SLOTS    0u
#define P9_SLOTS     2u
#define P9_TX_SLOTS  1u
#define PUMP_PAGES   1u
#else
#include "drivers/virtio_console.h"
static int  chan_read(uint8_t *buf, uint32_t max) { return virtio_console_read(buf, max); }
static int  chan_write(const uint8_t *buf, uint32_t len) { return virtio_console_write(buf, len); }
static bool chan_present(void) { return virtio_console_get_link() != NULL; }
/* The channel is LLP's now, not the 9P pump's (they share its ring). */
static void chan_claim(void) {
    p9_link_t *vlink = virtio_console_get_link();
    if (vlink) p9_link_unregister_background(vlink);
    dev_release("vconsole");
    if (dev_claim("llp") != 0) cprintf("llp: the channel's wire is held by someone else\n");
}
static void chan_service(void) {}
#define CHAN_STREAMS 0
#define ETH_SLOTS    8u
#define P9_SLOTS     4u
#define P9_TX_SLOTS  4u
#define PUMP_PAGES   2u
#endif

/* --- Frame queues ----------------------------------------------------------------
 * Fixed slots, one producer side and one consumer side each. The receive
 * queues are filled by the pump alone; the transmit queues have several
 * producers (the stack's task, the DHCP client, 9P server and clients), so
 * their put is under a spinlock, and the pump alone takes from them. */

typedef struct {
    uint8_t          *mem;
    uint16_t         *len;
    uint32_t          slot, nslots;
    volatile uint32_t head, tail;    /* free-running; head - tail = used */
    uint32_t          drops;         /* refused because full */
} fq_t;

static uint32_t fq_used(const fq_t *q) { return q->head - q->tail; }
static uint32_t fq_free(const fq_t *q) { return q->nslots - fq_used(q); }

static bool fq_put(fq_t *q, const uint8_t *d, uint32_t n) {
    if (n > q->slot || fq_free(q) == 0) { q->drops++; return false; }
    uint32_t i = q->head % q->nslots;
    memcpy(q->mem + (size_t)i * q->slot, d, n);
    q->len[i] = (uint16_t)n;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->head++;
    return true;
}

static const uint8_t *fq_peek(const fq_t *q, uint32_t *n) {
    if (fq_used(q) == 0) return NULL;
    uint32_t i = q->tail % q->nslots;
    *n = q->len[i];
    return q->mem + (size_t)i * q->slot;
}

static void fq_drop(fq_t *q) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->tail++;
}

/* The slots come from pages when the node starts, sized for its role (a
 * radio has no Ethernet queues): nothing is spent on a board that never runs
 * LLP. The length arrays are small enough to keep. */
static uint16_t g_eth_rx_len[ETH_SLOTS ? ETH_SLOTS : 1], g_eth_tx_len[ETH_SLOTS ? ETH_SLOTS : 1];
static uint16_t g_p9_rx_len[P9_SLOTS], g_p9_tx_len[P9_TX_SLOTS];
static fq_t g_eth_rx = { NULL, g_eth_rx_len, NETIF_FRAME_MAX, 0, 0, 0, 0 };
static fq_t g_eth_tx = { NULL, g_eth_tx_len, NETIF_FRAME_MAX, 0, 0, 0, 0 };
static fq_t g_p9_rx  = { NULL, g_p9_rx_len,  P9_MAX_MSIZE,    0, 0, 0, 0 };
static fq_t g_p9_tx  = { NULL, g_p9_tx_len,  P9_MAX_MSIZE,    0, 0, 0, 0 };
static spinlock_t g_tx_lock = SPINLOCK_INIT;
static uint32_t g_pages;        /* what the node took, for `llp` */

static uint8_t *take(uint32_t bytes) {
    uint32_t n = (bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint8_t *p = (uint8_t *)palloc_pages(n);
    if (p) g_pages += n;
    return p;
}

static bool fq_alloc(fq_t *q, uint32_t nslots) {
    q->nslots = nslots;
    if (!nslots) return true;
    q->mem = take(q->slot * nslots);
    return q->mem != NULL;
}

static bool tx_put(fq_t *q, const uint8_t *d, uint32_t n) {
    uintptr_t f = spin_lock_irqsave(&g_tx_lock);
    bool ok = fq_put(q, d, n);
    spin_unlock_irqrestore(&g_tx_lock, f);
    return ok;
}

/* --- State ----------------------------------------------------------------------- */

static llp_node_role_t g_role;
static llp_link_t      g_link;
static llp_serial_rx_t g_srx;
static uint8_t        *g_wire;
#define SRX_BYTES  (LLP_HDR_LEN + LLP_MAX_PAYLOAD + LLP_CRC_LEN)
#define WIRE_BYTES LLP_SERIAL_WIRE_MAX(LLP_MAX_PAYLOAD)
static volatile bool   g_ev_up, g_ev_down;
static uint32_t        g_chan_errors;

/* Radio role: the interface the link stands in for, and a frame read from it
 * that is waiting for credit. */
static netif_t        *g_bridged;
static uint8_t        *g_pending;              /* one frame */
static uint32_t        g_pending_len;
static int             g_sent_link = -1;      /* last STATE link value sent */
static uint32_t        g_fwd_to_link, g_fwd_to_if;

/* Host role: wlan0, registered on the first handshake (it needs the radio's MAC). */
static bool            g_wlan_registered;

static uint32_t now_ms(void) { return (uint32_t)time_get_ms(); }

/* --- LLP callbacks (all run on the pump task) ------------------------------------ */

static int on_send(void *ctx, const uint8_t hdr[LLP_HDR_LEN], const uint8_t *payload, uint16_t len) {
    (void)ctx;
#if CHAN_STREAMS
    llp_serial_emit(hdr, payload, len, chan_put, NULL);
    chan_end();
#else
    int n = llp_serial_encode(hdr, payload, len, g_wire, WIRE_BYTES);
    if (n < 0 || chan_write(g_wire, (uint32_t)n) < 0) { g_chan_errors++; return -1; }
#endif
    return 0;
}

/* One window for both kinds of data frame (doc/llp.md §5.2 counts frames,
 * not types): the smaller of what the two receive queues can still take. On
 * the radio, Ethernet frames go straight to the interface and only 9P waits. */
static uint32_t on_rx_free(void *ctx) {
    (void)ctx;
    uint32_t p9 = fq_free(&g_p9_rx);
    if (g_role == LLP_NODE_RADIO) return p9;
    uint32_t eth = fq_free(&g_eth_rx);
    return eth < p9 ? eth : p9;
}

static void on_event(void *ctx, llp_event_t ev, const llp_link_t *l) {
    (void)ctx; (void)l;
    if (ev == LLP_EV_UP) g_ev_up = true;
    if (ev == LLP_EV_DOWN) g_ev_down = true;
    if (ev == LLP_EV_MISMATCH)
        printk("[LLP] peer speaks LLP %u.%u, this node %u.%u: no data will pass until it restarts\n",
               l->peer_major, l->peer_minor, LLP_VERSION_MAJOR, LLP_VERSION_MINOR);
}

/* --- wlan0 (host role) ------------------------------------------------------------ */

static int wlan_poll(netif_t *nif) { (void)nif; return fq_used(&g_eth_rx) ? 1 : 0; }

static int wlan_recv(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    (void)nif;
    uint32_t n;
    const uint8_t *p = fq_peek(&g_eth_rx, &n);
    if (!p) return -1;
    int rc = -1;
    if (n <= max_len) { memcpy(buf, p, n); rc = (int)n; }
    fq_drop(&g_eth_rx);
    return rc;
}

static int wlan_send(netif_t *nif, const uint8_t *buf, uint32_t len) {
    (void)nif;
    return tx_put(&g_eth_tx, buf, len) ? (int)len : -1;
}

/* Up when the link is and the radio says its station is associated. */
static bool wlan_link_up(netif_t *nif) {
    (void)nif;
    return g_link.state == LLP_UP && g_link.peer_link;
}

static netif_t g_wlan = {
    .name = "wlan0", .poll = wlan_poll, .send_frame = wlan_send,
    .recv_frame = wlan_recv, .link_up = wlan_link_up,
};

/* --- The 9P link over LLP ---------------------------------------------------------
 * A cable, not a network: no authentication (fs/p9_link.h, auth_required). */

static int p9_poll(p9_link_t *link) { (void)link; return fq_used(&g_p9_rx) ? 1 : 0; }

static int p9_recv(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    uint32_t n;
    const uint8_t *p = fq_peek(&g_p9_rx, &n);
    if (!p) return -1;
    int rc = -1;
    if (n <= max_len) { memcpy(buf, p, n); rc = (int)n; }
    fq_drop(&g_p9_rx);
    return rc;
}

/* A 9P sender expects its message to go: wait for room rather than fail, but
 * not forever (a peer that stopped reading must cost seconds, not a task). */
static int p9_send(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    uint32_t t0 = now_ms();
    while (!tx_put(&g_p9_tx, buf, len)) {
        if (len > P9_MAX_MSIZE || now_ms() - t0 > 2000u) return -1;
        sched_yield();
    }
    return (int)len;
}

static p9_link_t g_p9 = {
    .name = "llp", .poll = p9_poll, .send_frame = p9_send, .recv_frame = p9_recv,
    .auth_required = false,
};

p9_link_t *llp_node_p9_link(void) { return g_role != LLP_NODE_OFF ? &g_p9 : NULL; }

/* --- The pump --------------------------------------------------------------------- */

static void deliver(const llp_frame_t *f) {
    if (f->type == LLP_T_P9) {
        (void)fq_put(&g_p9_rx, f->payload, f->len);
    } else if (g_role == LLP_NODE_RADIO) {
        if (netif_send(g_bridged, f->payload, f->len) >= 0) g_fwd_to_if++;
    } else {
        (void)fq_put(&g_eth_rx, f->payload, f->len);
    }
}

static void read_channel(uint32_t now) {
    static uint8_t buf[512];
    for (int round = 0; round < 8; round++) {
        int n = chan_read(buf, sizeof buf);
        if (n <= 0) return;
        for (int i = 0; i < n; i++) {
            llp_frame_t f, data;
            if (llp_serial_feed(&g_srx, buf[i], &f) && llp_link_input(&g_link, &f, now, &data))
                deliver(&data);
        }
    }
}

/* Queued frames out, 9P before Ethernet (doc/llp.md §5: a binding's
 * priority), as far as credit allows. */
static void drain(fq_t *q, uint8_t type, uint32_t now) {
    uint32_t n;
    const uint8_t *p;
    while ((p = fq_peek(q, &n)) != NULL) {
        int rc = llp_link_send(&g_link, type, p, (uint16_t)n, now);
        if (rc == -2 || rc == -1) return;           /* no credit / not up: keep it */
        fq_drop(q);                                 /* sent, or unsendable (-3/-4): gone */
    }
}

static void radio_forward(uint32_t now) {
    if (!g_bridged) return;
    for (int k = 0; k < 8; k++) {
        if (!g_pending_len) {
            if (netif_poll(g_bridged) != 1) return;
            int n = netif_recv(g_bridged, g_pending, NETIF_FRAME_MAX);
            if (n <= 0) continue;
            g_pending_len = (uint32_t)n;
        }
        if (g_link.state != LLP_UP) { g_pending_len = 0; continue; }   /* nobody to give it to */
        if (llp_link_send(&g_link, LLP_T_ETH, g_pending, (uint16_t)g_pending_len, now) == -2) return;
        g_fwd_to_link++;
        g_pending_len = 0;
    }
}

static void handle_events(uint32_t now) {
    if (g_ev_down) {
        g_ev_down = false;
        printk("[LLP] link down\n");
        g_sent_link = -1;
    }
    if (g_ev_up) {
        g_ev_up = false;
        char mac[18];
        netif_mac_str(g_link.peer_mac, mac);
        printk("[LLP] link up: peer %s, LLP %u.%u, mac %s\n",
               g_link.peer_role == LLP_ROLE_RADIO ? "radio" : "host", g_link.peer_major, g_link.peer_minor, mac);
        if (g_role == LLP_NODE_HOST) {
            memcpy(g_wlan.mac, g_link.peer_mac, NETIF_MAC_LEN);
            if (!g_wlan_registered) {
                if (netif_register(&g_wlan) == 0 && net_stack_attach(&g_wlan) == 0) {
                    g_wlan_registered = true;
                    net_task_start();
                    dhcp_start();
                    printk("[LLP] wlan0 registered, mac %s\n", mac);
                } else {
                    printk("[LLP] could not register wlan0\n");
                }
            }
        }
    }
    /* The radio's link state, sent when it changes and after every handshake. */
    if (g_role == LLP_NODE_RADIO && g_link.state == LLP_UP) {
        int up = netif_link_up(g_bridged) ? 1 : 0;
        if (up != g_sent_link) {
            llp_link_send_state(&g_link, (uint8_t)up, 0, now);
            g_sent_link = up;
        }
    }
}

static void pump(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t now = now_ms();
        read_channel(now);
        handle_events(now);
        if (g_role == LLP_NODE_RADIO) radio_forward(now);
        drain(&g_p9_tx, LLP_T_P9, now);
        drain(&g_eth_tx, LLP_T_ETH, now);
        llp_link_tick(&g_link, now);
        chan_service();
        sched_yield();
    }
}

/* --- Start ------------------------------------------------------------------------- */

int llp_node_start(llp_node_role_t role) {
    if (g_role != LLP_NODE_OFF) { cprintf("llp: already running\n"); return -1; }
    if (role != LLP_NODE_HOST && role != LLP_NODE_RADIO) return -1;
    if (!chan_present()) { cprintf("llp: no channel (no virtio-console)\n"); return -1; }

    llp_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    if (role == LLP_NODE_RADIO) {
        g_bridged = netif_default();
        if (!g_bridged) { cprintf("llp: no network interface to bridge\n"); return -1; }
        memcpy(cfg.mac, g_bridged->mac, NETIF_MAC_LEN);
        cfg.role = LLP_ROLE_RADIO;
    } else {
        cfg.role = LLP_ROLE_HOST;
    }
    cfg.max_payload = LLP_MAX_PAYLOAD;
    /* The window LLP may advertise: the radio only queues 9P (Ethernet goes
     * straight to its interface), the host queues both. */
    cfg.rx_window = (uint8_t)(role == LLP_NODE_RADIO ? P9_SLOTS : (ETH_SLOTS < P9_SLOTS ? ETH_SLOTS : P9_SLOTS));
    cfg.send = on_send;
    cfg.rx_free = on_rx_free;
    cfg.event = on_event;

    uint32_t eth = (role == LLP_NODE_HOST) ? ETH_SLOTS : 0u;
    if (role == LLP_NODE_HOST && eth == 0) { cprintf("llp: this board cannot be the host\n"); return -1; }
    uint8_t *srx = take(SRX_BYTES);
    if (!CHAN_STREAMS) g_wire = take(WIRE_BYTES);
    if (role == LLP_NODE_RADIO) g_pending = take(NETIF_FRAME_MAX);
    if (!srx || (!CHAN_STREAMS && !g_wire) || (role == LLP_NODE_RADIO && !g_pending) ||
        !fq_alloc(&g_eth_rx, eth) || !fq_alloc(&g_eth_tx, eth) ||
        !fq_alloc(&g_p9_rx, P9_SLOTS) || !fq_alloc(&g_p9_tx, P9_TX_SLOTS)) {
        cprintf("llp: out of memory (%u pages taken; they stay with the node)\n", (unsigned)g_pages);
        return -1;
    }

    chan_claim();
    if (role == LLP_NODE_RADIO) net_stack_detach();

    uint32_t session = 0;
    while (session == 0) random_bytes(&session, sizeof session);
    llp_serial_rx_init(&g_srx, srx, SRX_BYTES);
    g_role = role;
    llp_link_init(&g_link, &cfg, session, now_ms());

    p9_link_register_background(&g_p9);
    if (task_create_sized("llp", pump, NULL, PUMP_PAGES) < 0) {
        cprintf("llp: could not start the pump task\n");
        return -1;
    }
    cprintf("llp: %s, session %08x, %u KB, waiting for the peer\n",
            role == LLP_NODE_RADIO ? "radio (bridging the network interface)" : "host",
            (unsigned)session, (unsigned)(g_pages * PAGE_SIZE / 1024u));
    return 0;
}

/* --- Status ------------------------------------------------------------------------ */

void llp_node_print_status(void) {
    if (g_role == LLP_NODE_OFF) {
        cprintf("llp: off (`llp host` or `llp radio` starts it)\n");
        return;
    }
    static const char *const states[] = { "DOWN", "UP", "MISMATCH" };
    const llp_link_t *l = &g_link;
    cprintf("llp: %s, %s, session %08x\n", g_role == LLP_NODE_RADIO ? "radio" : "host",
            states[l->state], (unsigned)l->session);
    if (l->peer_session) {
        char mac[18];
        netif_mac_str(l->peer_mac, mac);
        cprintf("  peer: %s, session %08x, LLP %u.%u, max %u, mac %s, wifi %s rssi %d\n",
                l->peer_role == LLP_ROLE_RADIO ? "radio" : "host", (unsigned)l->peer_session,
                l->peer_major, l->peer_minor, l->peer_max_payload, mac,
                l->peer_link ? "up" : "down", l->peer_rssi);
    }
    const llp_stats_t *s = &l->st;
    cprintf("  data: tx %u rx %u, lost %u, stale %u, not-up %u, bad %u, too-big %u\n",
            (unsigned)s->tx_frames, (unsigned)s->rx_frames, (unsigned)s->rx_lost, (unsigned)s->rx_stale,
            (unsigned)s->rx_not_up, (unsigned)s->rx_bad, (unsigned)s->rx_too_big);
    cprintf("  link: tx %u rx %u (hello %u, credit %u, probe %u), held for credit %u, refused %u, ups %u downs %u\n",
            (unsigned)s->tx_link, (unsigned)s->rx_link, (unsigned)s->hellos, (unsigned)s->credits,
            (unsigned)s->probes, (unsigned)s->tx_no_credit, (unsigned)s->tx_refused,
            (unsigned)s->ups, (unsigned)s->downs);
    cprintf("  serial: %u frames, crc %u, runt %u, bad-len %u, overflow %u, channel errors %u\n",
            (unsigned)g_srx.frames, (unsigned)g_srx.crc_err, (unsigned)g_srx.runt,
            (unsigned)g_srx.bad_len, (unsigned)g_srx.overflow, (unsigned)g_chan_errors);
    cprintf("  queues: eth rx %u/%u (full %u), eth tx %u/%u (full %u), p9 rx %u/%u (full %u), p9 tx %u/%u (full %u); %u KB\n",
            (unsigned)fq_used(&g_eth_rx), (unsigned)g_eth_rx.nslots, (unsigned)g_eth_rx.drops,
            (unsigned)fq_used(&g_eth_tx), (unsigned)g_eth_tx.nslots, (unsigned)g_eth_tx.drops,
            (unsigned)fq_used(&g_p9_rx), (unsigned)g_p9_rx.nslots, (unsigned)g_p9_rx.drops,
            (unsigned)fq_used(&g_p9_tx), (unsigned)g_p9_tx.nslots, (unsigned)g_p9_tx.drops,
            (unsigned)(g_pages * PAGE_SIZE / 1024u));
#if defined(CONFIG_BOARD_ESP32C6)
    uint32_t lr, cr, ct;
    usbjtag_llp_counts(&lr, &cr, &ct);
    cprintf("  usb: llp rx overflow %u, console rx overflow %u, console tx overflow %u\n",
            (unsigned)lr, (unsigned)cr, (unsigned)ct);
#endif
    if (g_role == LLP_NODE_RADIO)
        cprintf("  bridge: %s, %u frames to the link, %u to the interface\n",
                g_bridged ? g_bridged->name : "-", (unsigned)g_fwd_to_link, (unsigned)g_fwd_to_if);
}
