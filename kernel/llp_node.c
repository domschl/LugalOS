/* An LLP node: the binding between kernel/llp.c and this kernel (protocol in doc/dev/protocols/llp.md). kernel/include/kernel/llp_node.h says
 * what it does; this file is how. */

#include "kernel/llp_node.h"
#include "lugalos_config.h"
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
#include "fs/vfs.h"
#include "kernel/identity.h"
#include "kernel/palloc.h"
#include <string.h>

/* --- The channel ----------------------------------------------------------------
 * What a board has to provide. Two kinds:
 *
 * * A byte stream, carrying LLP's serial binding (SLIP + CRC-32): QEMU's
 *   virtio-console, taken off the 9P pump; the C6-Zero's one USB port, shared
 *   with the console (drivers/usbjtag_esp32c6.c).
 * * Packets, carrying LLP's SDIO binding (kernel/include/kernel/llp_sdio.h):
 *   frames packed back to back, 4-byte aligned, no CRC, flow control in the
 *   hardware. The P4 as the SDIO host, the C6-MINI beside it as the slave. */

/* A serial channel's rate on a UART (llp_node_set_uart_baud()); 0: the board's default wire. */
static uint32_t g_uart_baud;   /* single task: set at boot, before LLP starts */

#if defined(CONFIG_BOARD_ESP32C6)
#include "kernel/radio_c6.h"
/* The station's signal, for STATE (dBm; -127 while not associated). */
static int radio_rssi(void) { return radio_c6_rssi(); }
#else
static int radio_rssi(void) { return 0; }
#endif

#if defined(CONFIG_BOARD_ESP32C6) && defined(CONFIG_SDIO_SLAVE)
#include "drivers/sdio_slave_esp32c6.h"
#include "kernel/llp_sdio.h"
#define CHAN_PACKET  1
static bool chan_present(void) { return true; }
static int  pkt_open(void) { return sdio_slave_start(); }
static bool pkt_poll(void) { return sdio_slave_poll(); }
static int  pkt_recv(uint8_t *buf, uint32_t cap) { return sdio_slave_recv(buf, cap); }
static bool pkt_can_send(uint32_t len) { (void)len; return sdio_slave_send_ready(); }
static int  pkt_send(const uint8_t *buf, uint32_t len) { return sdio_slave_send(buf, len); }
static void pkt_set_link(uint8_t up) { sdio_slave_set_link(up, 0); }
static void pkt_report(void) { sdio_slave_report(); }
/* The send buffers are the driver's: the only memory its DMA may read. */
static uint8_t *pkt_tx_buf(unsigned i) { return sdio_slave_tx_buf(i); }
/* The slave owns a sent packet until the host has read it: two buffers, one
 * out and one filling. */
#define PKT_DOUBLE   1
#define ETH_SLOTS    0u
#define P9_SLOTS     2u
#define P9_TX_SLOTS  1u
#define PUMP_PAGES   1u
#elif defined(CONFIG_BOARD_ESP32C6)
#include "drivers/usbjtag_esp32c6.h"
#define CHAN_PACKET  0
static int  chan_read(uint8_t *buf, uint32_t max) { return usbjtag_llp_read(buf, max); }
static bool chan_present(void) { return true; }
/* The USB port, shared with the console; or, with a rate set, UART0 (the
 * header's TX/RX) alone -- a C6 that is another board's radio over wires. */
static void chan_claim(void) {
    if (g_uart_baud && usbjtag_llp_use_uart(g_uart_baud) != 0)
        cprintf("llp: UART0 cannot run at %u baud; staying on the USB\n", (unsigned)g_uart_baud);
    usbjtag_llp_arm(true);
}
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
#elif defined(CONFIG_BOARD_ESP32P4)
#include "drivers/sdio_esp32p4.h"
#include "kernel/llp_sdio.h"
#define CHAN_PACKET  1
static bool chan_present(void) { return true; }
/* The C6 may not be up yet: the pump keeps polling until it is. */
static int  pkt_open(void) { (void)sdio_llp_open(); return 0; }
static bool pkt_poll(void) { return sdio_llp_poll() == 1; }
static int  pkt_recv(uint8_t *buf, uint32_t cap) { return sdio_llp_readable() ? sdio_llp_read(buf, cap) : 0; }
static bool pkt_can_send(uint32_t len) { return sdio_llp_room(len); }
static int  pkt_send(const uint8_t *buf, uint32_t len) { return sdio_llp_write(buf, len); }
static void pkt_set_link(uint8_t up) { (void)up; }
static void pkt_report(void) { sdio_llp_report(); }
static uint8_t *pkt_tx_buf(unsigned i) { (void)i; return NULL; }   /* the node's own */
/* The pump waits for the C6's card interrupt (sdio_llp_set_waker()). */
#define PUMP_WAITS   1
static bool chan_up(void) { return sdio_llp_up(); }
static bool chan_wake_pending(void) { return sdio_llp_irq_pending(); }
static void chan_set_waker(int pid) { sdio_llp_set_waker(pid); }
/* The write is done when sdio_llp_write() returns. */
#define PKT_DOUBLE   0
#define ETH_SLOTS    8u
#define P9_SLOTS     4u
#define P9_TX_SLOTS  4u
#define PUMP_PAGES   2u
#elif defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LLP_UART_BAUD)
#include "drivers/uart_raw.h"
#include "drivers/lcdterm.h"
#include <string.h>
#define CHAN_PACKET  0
static uint8_t *take(uint32_t bytes);
static int  chan_read(uint8_t *buf, uint32_t max) { return uart_raw_read(buf, max); }
static int  chan_write(const uint8_t *buf, uint32_t len) { return uart_raw_write(buf, len); }
static bool chan_present(void) { return true; }
/* UART0 (header H7) becomes LLP's: the panel console only tees to USB here,
 * so its claim on the wire is let go -- unless it tees to UART0, which keeps
 * it. Received bytes are buffered in an interrupt (drivers/uart_raw.h). */
#define RAW_RX_BYTES 8192u
static void chan_claim(void) {
    const char *owner = dev_wire_owner("llp");
    if (owner && strcmp(owner, "lcd") == 0 && lcdterm_tee() != LCD_TEE_UART) dev_release("lcd");
    if (dev_claim("llp") != 0) {
        cprintf("llp: UART0 is held by %s\n", dev_wire_owner("llp"));
        return;
    }
    uint32_t baud = g_uart_baud ? g_uart_baud : (uint32_t)CONFIG_LLP_UART_BAUD;
    uint8_t *ring = take(RAW_RX_BYTES);
    if (!ring || uart_raw_open(ring, RAW_RX_BYTES, baud) != 0) cprintf("llp: UART0 did not open at %u baud\n", (unsigned)baud);
}
static void chan_service(void) {}
/* The pump waits for the UART's receive interrupt (uart_raw_set_waker()). */
#define PUMP_WAITS   1
static bool chan_up(void) { return true; }
static bool chan_wake_pending(void) { return uart_raw_pending(); }
static void chan_set_waker(int pid) { uart_raw_set_waker(pid); }
#define CHAN_STREAMS 0
/* The host role only, on the RP2350's memory: half the QEMU host's Ethernet
 * queues; 9P frames are its 2 KB msize. */
#define ETH_SLOTS    4u
#define P9_SLOTS     4u
#define P9_TX_SLOTS  4u
#define PUMP_PAGES   2u
#else
#include "drivers/virtio_console.h"
#define CHAN_PACKET  0
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

#ifndef PUMP_WAITS
#define PUMP_WAITS   0
#endif
/* A pump that waits (PUMP_WAITS) is woken by its channel's interrupt and by
 * tx_put(); g_tx_seq tells it whether a frame was queued during its round. */
/* guarded by: g_tx_lock for g_tx_seq (tx_put()); the pump reads it. The pid is
 * set once, at start. */
static volatile int      g_pump_pid = -1;
static volatile uint32_t g_tx_seq;

/* --- Frame queues ----------------------------------------------------------------
 * Fixed slots, one producer side and one consumer side each. The receive
 * queues are filled by the pump alone; the transmit queues have several
 * producers (the stack's task, the DHCP client, 9P server and clients), so
 * their put is under a spinlock, and the pump alone takes from them.
 *
 * guarded by: that protocol. Receive: the pump is the one producer, and one
 * consumer per role (the stack's wlan_recv(); the 9P link's one user, the /c6
 * mount on a host or the server on a radio). Transmit: g_tx_lock for the
 * puts, the pump the one consumer. llp_node_start() allocates them before the
 * pump exists (it refuses a second start). */

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

/* Consumer side only: everything queued is dropped. */
static void fq_flush(fq_t *q) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->tail = q->head;
}

/* The slots come from pages when the node starts, sized for its role (a
 * radio has no Ethernet queues): nothing is spent on a board that never runs
 * LLP. The length arrays are small enough to keep. */
/* guarded by: the frame-queue protocol above. */
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
    if (ok) g_tx_seq++;
    spin_unlock_irqrestore(&g_tx_lock, f);
    if (ok && PUMP_WAITS && g_pump_pid >= 0) task_unblock(g_pump_pid);
    return ok;
}

/* --- State ----------------------------------------------------------------------- */

/* single task: the pump. llp_node_start() sets them up before creating it;
 * everything else reaches them through the callbacks the link calls from the
 * pump (on_event, on_send). */
static llp_node_role_t g_role;
static llp_link_t      g_link;
static volatile bool   g_ev_up, g_ev_down;

#if CHAN_PACKET
/* Packets out: frames are appended to the one filling, and it goes when the
 * next frame would not fit, or at the end of the pump's round. Packets in: one
 * at a time, its frames handed on as far as the queues take them; the rest
 * waits, and so does the channel (its flow control is the slave's buffers).
 * single task: the pump (llp_node_start() allocates them first). */
static uint8_t        *g_txp[2];
static unsigned        g_txp_cur;
static uint32_t        g_txp_len;
static uint8_t        *g_rxp;
static uint32_t        g_rxp_len, g_rxp_off;
static uint32_t        g_pkt_bad, g_chan_resets;
#else
static llp_serial_rx_t g_srx;
static uint8_t        *g_wire;
static uint32_t        g_chan_errors;
#define SRX_BYTES  (LLP_HDR_LEN + LLP_MAX_PAYLOAD + LLP_CRC_LEN)
#define WIRE_BYTES LLP_SERIAL_WIRE_MAX(LLP_MAX_PAYLOAD)
#endif

/* Radio role: the interface the link stands in for (NULL until the radio has
 * one), and a frame read from it that is waiting to go. In loop mode the
 * frames the host sends come back to it instead, through the same slot. */
static netif_t        *g_bridged;              /* guarded by: g_own_lock (below) */
static uint8_t        *g_pending;              /* single task: the pump; one frame */
static uint32_t        g_pending_len;
static int             g_sent_link = -1;      /* last STATE link value sent */
static int             g_sent_rssi;           /* ... its rssi, and when */
static uint32_t        g_sent_state_at;
static uint32_t        g_fwd_to_link, g_fwd_to_if;
static volatile bool   g_loop;
static uint32_t        g_looped;

/* Host role: wlan0, registered on the first handshake (it needs the radio's
 * MAC). `llp bench` frames are recognised by their EtherType and counted
 * here instead of going to the stack. */
static bool            g_wlan_registered;
#define BENCH_ETHERTYPE 0x88B5u               /* IEEE 802 local experimental */
/* accepted: counters the pump increments and `llp bench|soak` resets before a
 * run; a lost increment is a miscount by one. */
static volatile uint32_t g_bench_rx, g_bench_bad;

static uint32_t now_ms(void) { return (uint32_t)time_get_ms(); }

/* --- Packets (SDIO) ------------------------------------------------------------ */

#if CHAN_PACKET
static bool pkt_flush(void) {
    if (!g_txp_len) return true;
    if (!pkt_can_send(g_txp_len) || pkt_send(g_txp[g_txp_cur], g_txp_len) != 0) return false;
    if (PKT_DOUBLE) g_txp_cur ^= 1u;
    g_txp_len = 0;
    return true;
}

/* Room for a frame of `bytes` (aligned) in the packet filling, sending it
 * first if that is what it takes. */
static bool pkt_room(uint32_t bytes) {
    return g_txp_len + bytes <= LLP_SDIO_PKT_MAX || pkt_flush();
}

/* The channel started over: whatever was half-built or half-read is gone. */
static void pkt_restarted(void) {
    g_txp_len = 0;
    g_rxp_len = g_rxp_off = 0;
    g_chan_resets++;
}
#endif

/* Can a data frame of `len` payload bytes be handed to the link now? */
static bool tx_room(uint32_t len) {
#if CHAN_PACKET
    return pkt_room(LLP_SDIO_ALIGN(LLP_HDR_LEN + len));
#else
    (void)len;
    return true;
#endif
}

/* --- LLP callbacks (all run on the pump task) ------------------------------------ */

static int on_send(void *ctx, const uint8_t hdr[LLP_HDR_LEN], const uint8_t *payload, uint16_t len) {
    (void)ctx;
#if CHAN_PACKET
    uint32_t bytes = LLP_SDIO_ALIGN(LLP_HDR_LEN + (uint32_t)len);
    if (!pkt_room(bytes)) return -1;
    uint8_t *d = g_txp[g_txp_cur] + g_txp_len;
    memcpy(d, hdr, LLP_HDR_LEN);
    if (len) memcpy(d + LLP_HDR_LEN, payload, len);
    memset(d + LLP_HDR_LEN + len, 0, bytes - LLP_HDR_LEN - len);
    g_txp_len += bytes;
#elif CHAN_STREAMS
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

/* single task: the pump. */
static netif_t g_wlan = {
    .name = "wlan0", .poll = wlan_poll, .send_frame = wlan_send,
    .recv_frame = wlan_recv, .link_up = wlan_link_up,
};

/* --- The 9P link over LLP ---------------------------------------------------------
 * A cable, not a network: no authentication (fs/p9_link.h, auth_required). */

/* Frames received from a peer session that has since ended, up to this mark,
 * are dropped unread. Set by the pump (the queue's producer), acted on here
 * (its consumer): only the consumer moves the tail. */
/* single task: the pump (see above). */
static volatile uint32_t g_p9_rx_stale;

static void p9_drop_stale(void) {
    while ((int32_t)(g_p9_rx_stale - g_p9_rx.tail) > 0 && fq_used(&g_p9_rx)) fq_drop(&g_p9_rx);
}

static int p9_poll(p9_link_t *link) {
    (void)link;
    p9_drop_stale();
    return fq_used(&g_p9_rx) ? 1 : 0;
}

static int p9_recv(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    p9_drop_stale();
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

static bool p9_up(p9_link_t *link) { (void)link; return g_link.state == LLP_UP; }

/* single task: the pump moves it (generation, session); c6attach only reads
 * it and uses its ops. */
static p9_link_t g_p9 = {
    .name = "llp", .poll = p9_poll, .send_frame = p9_send, .recv_frame = p9_recv,
    .auth_required = false, .up = p9_up,
};

/* The peer session the 9P side belongs to. A peer that was only cut off comes
 * back with its session, and its fids with it; a new session is a peer that
 * restarted, and nothing sent to the old one means anything to it. */
static uint32_t g_p9_session;

/* Called by the pump on every handshake, before it sends anything. True when
 * the peer is a new one. */
static bool p9_new_peer(void) {
    if (g_link.peer_session == g_p9_session) return false;
    g_p9_session = g_link.peer_session;
    g_p9.generation++;
    g_p9_rx_stale = g_p9_rx.head;
    uintptr_t f = spin_lock_irqsave(&g_tx_lock);
    fq_flush(&g_p9_tx);
    spin_unlock_irqrestore(&g_tx_lock, f);
    return true;
}

p9_link_t *llp_node_p9_link(void) { return g_role != LLP_NODE_OFF ? &g_p9 : NULL; }

bool llp_node_link_up(void) { return g_role != LLP_NODE_OFF && g_link.state == LLP_UP; }

/* --- The pump --------------------------------------------------------------------- */

static bool is_bench(const llp_frame_t *f) {
    return f->len >= 14u && ((uint32_t)f->payload[12] << 8 | f->payload[13]) == BENCH_ETHERTYPE;
}

#if CHAN_PACKET
/* Can a data frame of `type` be handed on now? Asked before llp_link_input()
 * takes it, so a frame that cannot go stays in the channel. */
static bool rx_room(uint8_t type) {
    if (type == LLP_T_P9) return fq_free(&g_p9_rx) > 0;
    if (type != LLP_T_ETH) return true;
    if (g_role == LLP_NODE_RADIO) return !g_loop || g_pending_len == 0;
    return fq_free(&g_eth_rx) > 0;
}
#endif

static void deliver(const llp_frame_t *f) {
    if (f->type == LLP_T_P9) {
        (void)fq_put(&g_p9_rx, f->payload, f->len);
    } else if (g_role == LLP_NODE_RADIO) {
        if (g_loop) {
            if (f->len <= NETIF_FRAME_MAX) {
                memcpy(g_pending, f->payload, f->len);
                g_pending_len = f->len;
                g_looped++;
            }
        } else if (g_bridged && netif_send(g_bridged, f->payload, f->len) >= 0) {
            g_fwd_to_if++;
        }
    } else if (is_bench(f)) {
        bool ok = true;
        for (uint32_t i = 14; i < f->len; i++) if (f->payload[i] != (uint8_t)i) { ok = false; break; }
        if (ok) g_bench_rx++; else g_bench_bad++;
    } else {
        (void)fq_put(&g_eth_rx, f->payload, f->len);
    }
}

#if CHAN_PACKET
static void read_channel(uint32_t now) {
    for (int round = 0; round < 8; round++) {
        if (g_rxp_off >= g_rxp_len) {
            int n = pkt_recv(g_rxp, LLP_SDIO_PKT_MAX);
            if (n <= 0) { if (n < 0) g_pkt_bad++; return; }
            g_rxp_len = (uint32_t)n;
            g_rxp_off = 0;
        }
        while (g_rxp_off + LLP_HDR_LEN <= g_rxp_len) {
            const uint8_t *p = g_rxp + g_rxp_off;
            uint32_t flen = LLP_HDR_LEN + ((uint32_t)p[2] | ((uint32_t)p[3] << 8));
            if (p[0] == 0) { g_rxp_off = g_rxp_len; break; }          /* zero fill: the end */
            if (g_rxp_off + flen > g_rxp_len) { g_pkt_bad++; g_rxp_off = g_rxp_len; break; }
            if (!rx_room(p[0])) return;
            llp_frame_t f, data;
            if (llp_frame_parse(p, flen, &f) == 0 && llp_link_input(&g_link, &f, now, &data))
                deliver(&data);
            g_rxp_off += LLP_SDIO_ALIGN(flen);
        }
        g_rxp_off = g_rxp_len;
    }
}
#else
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
#endif

/* Queued frames out, 9P before Ethernet (doc/llp.md §5: a binding's
 * priority), as far as credit and the channel allow. */
static void drain(fq_t *q, uint8_t type, uint32_t now) {
    uint32_t n;
    const uint8_t *p;
    while ((p = fq_peek(q, &n)) != NULL) {
        if (!tx_room(n)) return;
        int rc = llp_link_send(&g_link, type, p, (uint16_t)n, now);
        if (rc == -2 || rc == -1) return;           /* no credit / not up: keep it */
        fq_drop(q);                                 /* sent, or unsendable (-3/-4): gone */
    }
}

/* Who reads the radio's interface: this node's stack, or the bridge -- never
 * both, since two readers corrupt its receive ring. Decided under g_own_lock
 * by whichever comes second: the radio registering its interface
 * (llp_node_offer_netif()), or the link coming up (radio_adopt()). */
static spinlock_t g_own_lock = SPINLOCK_INIT;
static netif_t   *g_stack_nif;     /* guarded by: g_own_lock (llp_node_start() before the pump) */

/* A host is there: the interface is bridged from now on. A radio with no host
 * stays a node of its own: it is a host's HELLO that makes it a network card. */
static void radio_adopt(void) {
    if (g_bridged || g_link.state != LLP_UP) return;
    netif_t *nif = NULL;
    uintptr_t f = spin_lock_irqsave(&g_own_lock);
    if (g_stack_nif) {
        net_stack_detach();
        nif = g_bridged = g_stack_nif;
    }
    spin_unlock_irqrestore(&g_own_lock, f);
    if (nif) printk("[LLP] bridging %s\n", nif->name);
}

bool llp_node_offer_netif(netif_t *nif) {
    bool taken = false;
    uintptr_t f = spin_lock_irqsave(&g_own_lock);
    if (g_role == LLP_NODE_RADIO && g_link.state == LLP_UP && !g_bridged) {
        g_bridged = nif;
        taken = true;
    } else {
        g_stack_nif = nif;
    }
    spin_unlock_irqrestore(&g_own_lock, f);
    if (taken) {
        printk("[LLP] bridging %s\n", nif->name);
    } else {
        net_stack_attach(nif);
    }
    return taken;
}

static void radio_forward(uint32_t now) {
    for (int k = 0; k < 8; k++) {
        if (!g_pending_len) {
            if (g_loop || !g_bridged || netif_poll(g_bridged) != 1) return;
            int n = netif_recv(g_bridged, g_pending, NETIF_FRAME_MAX);
            if (n <= 0) continue;
            g_pending_len = (uint32_t)n;
        }
        if (g_link.state != LLP_UP) { g_pending_len = 0; continue; }   /* nobody to give it to */
        if (!tx_room(g_pending_len)) return;
        if (llp_link_send(&g_link, LLP_T_ETH, g_pending, (uint16_t)g_pending_len, now) == -2) return;
        g_fwd_to_link++;
        g_pending_len = 0;
    }
}

/* Host role, after a handshake with a new radio, and every few seconds while
 * /c6 is not mounted and the link is up: the radio's namespace at /c6,
 * afresh, and -- when this node's identity record holds WLAN credentials and
 * the radio is not associated -- the network for it to join, written to its
 * /dev/wlan. The credentials live
 * here, and the radio holds them in RAM only: its image carries a binary blob,
 * and a key it never stores is one it cannot leak. Its own task: the mount
 * talks 9P over this link, which only the pump moves. */
/* guarded by: a hand-off. The pump sets g_c6_attaching and starts c6attach;
 * until the task clears it, only the task writes g_c6_mounted and
 * g_c6_fail_said. A new peer meanwhile: the pump clears g_c6_mounted, and
 * the task's generation check keeps it clear. */
static volatile bool g_c6_mounted, g_c6_attaching;
static uint32_t g_c6_attach_at;                /* when the last attempt started */
static bool g_c6_fail_said;                    /* a failure is said once per radio */
#define C6_ATTACH_RETRY_MS 5000u
/* accepted: a word-sized flag; set by whoever stored new credentials, cleared by the pump. */
static volatile bool g_creds_changed;

void llp_node_credentials_changed(void) { g_creds_changed = true; }

static void c6_attach_task(void *arg) {
    (void)arg;
    uint32_t gen = g_p9.generation;
    (void)vfs_unmount("c6");
    bool ok = vfs_mount_remote("c6", &g_p9) == 0;
    /* A radio that restarted during the attach makes it one of the old peer:
     * the pump tries again. */
    g_c6_mounted = ok && gen == g_p9.generation;
    g_c6_attaching = false;
    if (!g_c6_mounted) {
        if (!ok && !g_c6_fail_said) {
            printk("[LLP] could not mount the radio at /c6 (trying again every %u s)\n",
                   (unsigned)(C6_ATTACH_RETRY_MS / 1000u));
            g_c6_fail_said = true;
        }
        return;
    }
    printk("[LLP] the radio's namespace is at /c6\n");
    char ssid[NODE_WLAN_SSID_MAX + 1];
    uint8_t psk[NODE_WLAN_PSK_LEN];
    if (g_link.peer_link || !node_wlan_ssid(ssid, sizeof ssid) || !node_wlan_psk(psk)) {
        memset(psk, 0, sizeof psk);
        return;
    }
    static const char digits[] = "0123456789abcdef";
    char cmd[8 + NODE_WLAN_SSID_MAX + 2 * NODE_WLAN_PSK_LEN];
    uint32_t n = 0;
    for (const char *p = "join "; *p; p++) cmd[n++] = *p;
    for (const char *p = ssid; *p; p++) cmd[n++] = *p;
    cmd[n++] = ' ';
    for (uint32_t i = 0; i < NODE_WLAN_PSK_LEN; i++) {
        cmd[n++] = digits[psk[i] >> 4];
        cmd[n++] = digits[psk[i] & 15];
    }
    cmd[n++] = '\n';
    memset(psk, 0, sizeof psk);
    int fd = vfs_open("/c6/dev/wlan", VFS_O_WRITE);
    int rc = fd >= 0 ? vfs_pwrite(fd, cmd, n, 0) : -1;
    if (fd >= 0) vfs_close(fd);
    memset(cmd, 0, sizeof cmd);
    if (rc == (int)n) printk("[LLP] the radio was given \"%s\" to join\n", ssid);
    else printk("[LLP] the radio refused the network (already started, or no radio)\n");
}

static void handle_events(uint32_t now) {
    if (g_ev_down) {
        g_ev_down = false;
        printk("[LLP] link down\n");
        g_sent_link = -1;
    }
    if (g_ev_up) {
        g_ev_up = false;
        bool new_peer = p9_new_peer();
        char mac[18];
        netif_mac_str(g_link.peer_mac, mac);
        printk("[LLP] link up: peer %s, LLP %u.%u, mac %s\n",
               g_link.peer_role == LLP_ROLE_RADIO ? "radio" : "host", g_link.peer_major, g_link.peer_minor, mac);
        if (g_role == LLP_NODE_HOST) {
            memcpy(g_wlan.mac, g_link.peer_mac, NETIF_MAC_LEN);
            if (!g_wlan_registered) {
#if defined(CONFIG_NET_PREFER_WIRED)
                /* The stack stays on a wire that has a carrier; net/stack.c moves
                 * it here when there is none. */
                const netif_t *cur = net_state()->nif;
                bool take = !cur || !netif_link_up((netif_t *)cur);
#else
                bool take = true;
#endif
                if (netif_register(&g_wlan) == 0 && (!take || net_stack_attach(&g_wlan) == 0)) {
                    g_wlan_registered = true;
                    net_task_start();
                    dhcp_start();
                    printk("[LLP] wlan0 registered, mac %s\n", mac);
                } else {
                    printk("[LLP] could not register wlan0\n");
                }
            }
            if (new_peer) { g_c6_mounted = false; g_c6_attach_at = 0; g_c6_fail_said = false; }
        }
    }
    /* New credentials on this host reach the radio the way they do at boot: attach again, which hands them
     * over unless the radio has joined a network already (it has to restart to change networks). */
    if (g_creds_changed && g_role == LLP_NODE_HOST && g_link.state == LLP_UP && g_c6_mounted && !g_c6_attaching) {
        g_creds_changed = false;
        if (g_link.peer_link) printk("[LLP] new Wi-Fi credentials: the radio is joined already; restart it to change networks\n");
        else g_c6_mounted = false;
    }
    if (g_role == LLP_NODE_HOST && g_link.state == LLP_UP && !g_c6_mounted && !g_c6_attaching &&
        (g_c6_attach_at == 0 || now - g_c6_attach_at >= C6_ATTACH_RETRY_MS)) {
        g_c6_attaching = true;
        g_c6_attach_at = now ? now : 1u;
        /* 16 KB: the 9P attach under vfs_mount_remote() goes 9.1 KB deep
         * (measured), past the default 8 KB -- and an overflow here lands
         * silently in the stack below. */
        if (task_create_sized("c6attach", c6_attach_task, NULL, 4) < 0) {
            g_c6_attaching = false;
            printk("[LLP] could not start c6attach\n");
        }
    }
    /* The radio's link state, sent when it changes and after every handshake;
     * its signal too, when that moved by 3 dB or more (at most every 5 s). */
    if (g_role == LLP_NODE_RADIO && g_link.state == LLP_UP) {
        int up = (g_bridged && netif_link_up(g_bridged)) ? 1 : 0;
        int rssi = up ? radio_rssi() : 0;
        int drift = rssi - g_sent_rssi;
        if (up != g_sent_link || ((drift >= 3 || drift <= -3) && now - g_sent_state_at >= 5000u)) {
            llp_link_send_state(&g_link, (uint8_t)up, (int8_t)rssi, now);
#if CHAN_PACKET
            pkt_set_link((uint8_t)up);
#endif
            g_sent_link = up;
            g_sent_rssi = rssi;
            g_sent_state_at = now;
        }
    }
}

/* Anything moved this round? A pump that waits (PUMP_WAITS) does so only
 * when nothing did. */
static uint32_t activity(void) {
    const llp_stats_t *s = &g_link.st;
    return s->tx_frames + s->rx_frames + s->tx_link + s->rx_link;
}

static void pump(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t before = activity();
        uint32_t seq = g_tx_seq;
#if CHAN_PACKET
        if (pkt_poll()) pkt_restarted();
#endif
        read_channel(now);
        handle_events(now);
        if (g_role == LLP_NODE_RADIO) {
            radio_adopt();
            radio_forward(now);
        }
        drain(&g_p9_tx, LLP_T_P9, now);
        drain(&g_eth_tx, LLP_T_ETH, now);
        llp_link_tick(&g_link, now);
#if CHAN_PACKET
        (void)pkt_flush();
#else
        chan_service();
#endif
#if PUMP_WAITS
        /* Nothing moved: what could move waits on the peer -- bytes, a
         * packet, credits, receive buffers -- and the channel's interrupt
         * says when that changes; a new frame here comes with tx_put()'s
         * wake. 20 ms bounds LLP's timers and a peer that changed without a
         * word (a C6 that restarted, one that raises no interrupt). Polling
         * every millisecond instead cost a core shared with a panel and a
         * shell measurably. A channel that is down is probed every 50 ms. */
        if (activity() == before) {
            if (!chan_up()) { task_sleep_ms(50u); continue; }
            task_prepare_block();
            if (!chan_wake_pending() && g_tx_seq == seq) task_block_until_ms(time_get_ms() + 20u);
            task_wait_done();
            continue;
        }
#else
        (void)before;
        (void)seq;
#endif
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
        /* The interface is bridged once the link is up (radio_adopt()); the
         * host learns its MAC now. */
        netif_t *nif = netif_default();
        if (nif) {
            g_stack_nif = nif;         /* attached at boot (kernel/main.c) */
            memcpy(cfg.mac, nif->mac, NETIF_MAC_LEN);
#if defined(CONFIG_BOARD_ESP32C6)
        } else {
            /* No interface yet (no credentials, or not joined): the station
             * MAC is the chip's own, so the host can be told it now. */
            memcpy(cfg.mac, node_mac(), NETIF_MAC_LEN);
#endif
        }
#if !defined(CONFIG_BOARD_ESP32C6)
        if (!nif) { cprintf("llp: no network interface to bridge\n"); return -1; }
#endif
        cfg.role = LLP_ROLE_RADIO;
    } else {
        cfg.role = LLP_ROLE_HOST;
    }
    cfg.max_payload = LLP_MAX_PAYLOAD;
#if CHAN_PACKET
    /* The channel's own flow control (llp.md §5.1): no FLOW, no credits. */
    cfg.rx_window = 0;
#else
    /* The window LLP may advertise: the radio only queues 9P (Ethernet goes
     * straight to its interface), the host queues both. */
    cfg.rx_window = (uint8_t)(role == LLP_NODE_RADIO ? P9_SLOTS : (ETH_SLOTS < P9_SLOTS ? ETH_SLOTS : P9_SLOTS));
#endif
    cfg.send = on_send;
    cfg.rx_free = on_rx_free;
    cfg.event = on_event;

    uint32_t eth = (role == LLP_NODE_HOST) ? ETH_SLOTS : 0u;
    if (role == LLP_NODE_HOST && eth == 0) { cprintf("llp: this board cannot be the host\n"); return -1; }
    bool ok = true;
#if CHAN_PACKET
    if (pkt_open() != 0) { cprintf("llp: the channel did not start\n"); return -1; }
    g_txp[0] = pkt_tx_buf(0);
    g_txp[1] = pkt_tx_buf(1);
    if (!g_txp[0]) {
        uint8_t *tx = take((PKT_DOUBLE ? 2u : 1u) * LLP_SDIO_PKT_MAX);
        if (tx) { g_txp[0] = tx; g_txp[1] = tx + LLP_SDIO_PKT_MAX; }
    }
    g_rxp = take(LLP_SDIO_PKT_MAX);
    ok = g_txp[0] && g_rxp;
#else
    uint8_t *srx = take(SRX_BYTES);
    if (!CHAN_STREAMS) g_wire = take(WIRE_BYTES);
    ok = srx && (CHAN_STREAMS || g_wire);
#endif
    if (role == LLP_NODE_RADIO) g_pending = take(NETIF_FRAME_MAX);
    if (!ok || (role == LLP_NODE_RADIO && !g_pending) ||
        !fq_alloc(&g_eth_rx, eth) || !fq_alloc(&g_eth_tx, eth) ||
        !fq_alloc(&g_p9_rx, P9_SLOTS) || !fq_alloc(&g_p9_tx, P9_TX_SLOTS)) {
        cprintf("llp: out of memory (%u pages taken; they stay with the node)\n", (unsigned)g_pages);
        return -1;
    }

#if !CHAN_PACKET
    chan_claim();
#endif

    uint32_t session = 0;
    while (session == 0) random_bytes(&session, sizeof session);
#if !CHAN_PACKET
    llp_serial_rx_init(&g_srx, srx, SRX_BYTES);
#endif
    g_role = role;
    llp_link_init(&g_link, &cfg, session, now_ms());

    p9_link_register_background(&g_p9);
    int pump_pid = task_create_sized("llp", pump, NULL, PUMP_PAGES);
    if (pump_pid < 0) {
        cprintf("llp: could not start the pump task\n");
        return -1;
    }
#if PUMP_WAITS
    g_pump_pid = pump_pid;
    chan_set_waker(pump_pid);
#endif
    cprintf("llp: %s, session %08x, %u KB, waiting for the peer\n",
            role == LLP_NODE_RADIO ? "radio (bridges its network interface once a host is up)" : "host",
            (unsigned)session, (unsigned)(g_pages * PAGE_SIZE / 1024u));
    return 0;
}

void llp_node_set_uart_baud(uint32_t baud) { g_uart_baud = baud; }

#if defined(CONFIG_BOARD_ESP32C6) && !defined(CONFIG_SDIO_SLAVE)
/* A C6-Zero with no USB host at boot is another board's radio over its UART0
 * (the RP2350 terminal's H7): nothing reads its console, and its header's TX/RX
 * lead to that board. With a host on the USB it is a node of its own, LLP left
 * to `llp radio` as before. The look waits for the host to configure the port.
 * The rate is the board file's, and the other board's must match it. */
#if defined(CONFIG_LLP_UART_BAUD)
#define UART_RADIO_BAUD ((uint32_t)CONFIG_LLP_UART_BAUD)
#else
#define UART_RADIO_BAUD 115200u
#endif
static void uart_radio_task(void *arg) {
    (void)arg;
    task_sleep_ms(2000);
    if (usbjtag_host_present()) return;
    llp_node_set_uart_baud(UART_RADIO_BAUD);
    (void)llp_node_start(LLP_NODE_RADIO);
}
#endif

void llp_node_autostart(void) {
#if defined(CONFIG_BOARD_ESP32C6) && defined(CONFIG_SDIO_SLAVE)
    /* The C6 beside a P4: the channel is dedicated, so the radio role is
     * armed at boot and waits for a host. */
    (void)llp_node_start(LLP_NODE_RADIO);
#elif defined(CONFIG_BOARD_ESP32C6)
    if (task_create_sized("llpuart", uart_radio_task, NULL, 1) < 0) printk("[LLP] could not start the UART look\n");
#elif defined(CONFIG_BOARD_RP2350) && defined(CONFIG_LLP_UART_BAUD)
    /* A board whose UART0 header leads to its radio (the terminal's H7): the
     * wire is LLP's from boot. Left to the console, whatever the radio sends --
     * its boot log, LLP's frames -- arrives as keystrokes. */
    (void)llp_node_start(LLP_NODE_HOST);
#elif defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_SDIO_CLK_GPIO) && \
      (!defined(CONFIG_EMAC_BASE) || defined(CONFIG_NET_PREFER_WIRED))
    /* A P4 whose network is its C6 (the LCD-7B), or that has a wire as well and
     * a rule for which one the stack serves (the NANO, net/stack.c). */
    (void)llp_node_start(LLP_NODE_HOST);
#endif
}

/* --- Loop mode and the bench -------------------------------------------------------- */

void llp_node_set_loop(bool on) {
    g_loop = on;
    cprintf("llp: loop %s -- Ethernet frames from the host %s\n", on ? "on" : "off",
            on ? "go back to it" : "go to the radio");
}

void llp_node_bench(uint32_t count, uint32_t size) {
    if (g_role != LLP_NODE_HOST || g_link.state != LLP_UP) {
        cprintf("llp bench: needs `llp host` with the link up, and `llp loop on` on the radio\n");
        return;
    }
    if (size < 64u) size = 64u;
    if (size > NETIF_FRAME_MAX) size = NETIF_FRAME_MAX;
    /* accepted: a diagnostic command's state; two terminals running it at once mix
     * their output. */
    static uint8_t frame[NETIF_FRAME_MAX];
    memset(frame, 0xFF, 6);
    memcpy(frame + 6, g_wlan.mac, 6);
    frame[12] = (uint8_t)(BENCH_ETHERTYPE >> 8);
    frame[13] = (uint8_t)BENCH_ETHERTYPE;
    for (uint32_t i = 14; i < size; i++) frame[i] = (uint8_t)i;
    g_bench_rx = 0;
    g_bench_bad = 0;
    uint64_t t0 = time_get_us();
    uint32_t sent = 0, last_rx = 0;
    uint32_t idle_since = now_ms();
    while (g_bench_rx + g_bench_bad < count) {
        if (sent < count && tx_put(&g_eth_tx, frame, size)) sent++;
        else sched_yield();
        uint32_t got = g_bench_rx + g_bench_bad;
        if (got != last_rx) { last_rx = got; idle_since = now_ms(); }
        else if (now_ms() - idle_since > 2000u) break;
    }
    uint64_t us = time_get_us() - t0;
    if (!us) us = 1;
    uint64_t bytes = (uint64_t)g_bench_rx * size;
    cprintf("llp bench: %u frames of %u B sent, %u back intact, %u damaged, %u lost, in %u ms: %u KB/s each way\n",
            (unsigned)sent, (unsigned)size, (unsigned)g_bench_rx, (unsigned)g_bench_bad,
            (unsigned)(sent - g_bench_rx - g_bench_bad), (unsigned)(us / 1000u),
            (unsigned)(bytes * 1000000u / 1024u / us));
}

/* The soak: the bench as a task, for minutes, with the console free meanwhile
 * (to reset the radio or the channel under it). It sends only while the link
 * is up with the peer it started with: a radio that restarted has left loop
 * mode, and its bridge would put the frames -- broadcasts -- on the air. */
/* accepted: `llp soak` tests and sets the flag from a shell, the soak task
 * clears it when done; two shells starting one at the same instant could both
 * start. */
static uint32_t g_soak_s, g_soak_size;
static volatile bool g_soak_running;

static void soak_task(void *arg) {
    (void)arg;
    static uint8_t frame[NETIF_FRAME_MAX];
    uint32_t size = g_soak_size;
    memset(frame, 0xFF, 6);
    memcpy(frame + 6, g_wlan.mac, 6);
    frame[12] = (uint8_t)(BENCH_ETHERTYPE >> 8);
    frame[13] = (uint8_t)BENCH_ETHERTYPE;
    for (uint32_t i = 14; i < size; i++) frame[i] = (uint8_t)i;
    g_bench_rx = 0;
    g_bench_bad = 0;
    uint32_t peer = g_link.peer_session, sent = 0, downs = 0;
    bool was_up = true;
    uint32_t t0 = now_ms(), next_report = t0 + 60000u;
    const char *why = "done";
    while (now_ms() - t0 < g_soak_s * 1000u) {
        bool up = g_link.state == LLP_UP;
        if (up && g_link.peer_session != peer) { why = "the radio restarted (new session): stopped"; break; }
        if (!up) {
            if (was_up) { downs++; printk("[LLP] soak: link down at %u s, waiting\n", (unsigned)((now_ms() - t0) / 1000u)); }
            was_up = false;
            task_sleep_ms(10);
            continue;
        }
        if (!was_up) printk("[LLP] soak: link up again at %u s\n", (unsigned)((now_ms() - t0) / 1000u));
        was_up = true;
        if (tx_put(&g_eth_tx, frame, size)) sent++;   /* a full queue paces it */
        else sched_yield();
        if ((int32_t)(now_ms() - next_report) >= 0) {
            next_report += 60000u;
            uint32_t ms = now_ms() - t0;
            printk("[LLP] soak %u s: %u sent, %u back intact, %u damaged, %u KB/s each way\n",
                   (unsigned)(ms / 1000u), (unsigned)sent, (unsigned)g_bench_rx, (unsigned)g_bench_bad,
                   (unsigned)((uint64_t)g_bench_rx * size / (ms ? ms : 1u) * 1000u / 1024u));
        }
    }
    task_sleep_ms(500);                         /* the last frames back */
    uint32_t ms = now_ms() - t0;
    printk("[LLP] soak %s after %u s: %u frames of %u B sent, %u back intact, %u damaged, %u lost, %u link drops, %u KB/s each way\n",
           why, (unsigned)(ms / 1000u), (unsigned)sent, (unsigned)size, (unsigned)g_bench_rx, (unsigned)g_bench_bad,
           (unsigned)(sent - g_bench_rx - g_bench_bad), (unsigned)downs,
           (unsigned)((uint64_t)g_bench_rx * size / (ms ? ms : 1u) * 1000u / 1024u));
    g_soak_running = false;
}

void llp_node_soak(uint32_t seconds, uint32_t size) {
    if (g_role != LLP_NODE_HOST || g_link.state != LLP_UP) {
        cprintf("llp soak: needs `llp host` with the link up, and `llp loop on` on the radio\n");
        return;
    }
    if (g_soak_running) { cprintf("llp soak: one is running\n"); return; }
    if (size < 64u) size = 64u;
    if (size > NETIF_FRAME_MAX) size = NETIF_FRAME_MAX;
    g_soak_s = seconds;
    g_soak_size = size;
    g_soak_running = true;
    if (task_create("llpsoak", soak_task, NULL) < 0) {
        g_soak_running = false;
        cprintf("llp soak: could not start the task\n");
        return;
    }
    cprintf("llp soak: %u s of %u B frames; progress every minute in the kernel log\n",
            (unsigned)seconds, (unsigned)size);
}

/* --- Status ------------------------------------------------------------------------ */

uint32_t llp_node_proc_render(char *buf, uint32_t cap) {
    static const char *const states[] = { "down", "up", "mismatch" };
    const llp_link_t *l = &g_link;
    uint32_t n = 0;
#define PUT(...) do { if (n < cap) n += (uint32_t)ksnprintf(buf + n, cap - n, __VA_ARGS__); } while (0)
    PUT("role=%s\n", g_role == LLP_NODE_OFF ? "off" : g_role == LLP_NODE_RADIO ? "radio" : "host");
    if (g_role == LLP_NODE_OFF) return n < cap ? n : cap;
    PUT("state=%s\nsession=%08x\n", states[l->state], (unsigned)l->session);
    if (l->peer_session) {
        char mac[18];
        netif_mac_str(l->peer_mac, mac);
        PUT("peer_role=%s\npeer_session=%08x\npeer_version=%u.%u\npeer_max_payload=%u\npeer_mac=%s\n",
            l->peer_role == LLP_ROLE_RADIO ? "radio" : "host", (unsigned)l->peer_session,
            l->peer_major, l->peer_minor, l->peer_max_payload, mac);
        if (l->peer_role == LLP_ROLE_RADIO)
            PUT("peer_wifi=%s\npeer_rssi=%d\n", l->peer_link ? "up" : "down", l->peer_rssi);
    }
    const llp_stats_t *st = &l->st;
    PUT("tx_frames=%u\nrx_frames=%u\nrx_lost=%u\nrx_stale=%u\nrx_not_up=%u\nrx_bad=%u\nrx_too_big=%u\n",
        (unsigned)st->tx_frames, (unsigned)st->rx_frames, (unsigned)st->rx_lost, (unsigned)st->rx_stale,
        (unsigned)st->rx_not_up, (unsigned)st->rx_bad, (unsigned)st->rx_too_big);
    PUT("tx_link=%u\nrx_link=%u\nhellos=%u\ncredits=%u\nprobes=%u\nheld_for_credit=%u\nrefused=%u\nups=%u\ndowns=%u\n",
        (unsigned)st->tx_link, (unsigned)st->rx_link, (unsigned)st->hellos, (unsigned)st->credits,
        (unsigned)st->probes, (unsigned)st->tx_no_credit, (unsigned)st->tx_refused,
        (unsigned)st->ups, (unsigned)st->downs);
    PUT("p9_generation=%u\n", (unsigned)g_p9.generation);
#if CHAN_PACKET
    PUT("pkt_bad=%u\nchannel_restarts=%u\n", (unsigned)g_pkt_bad, (unsigned)g_chan_resets);
#else
    PUT("serial_frames=%u\nserial_crc=%u\nserial_runt=%u\nserial_overflow=%u\n",
        (unsigned)g_srx.frames, (unsigned)g_srx.crc_err, (unsigned)g_srx.runt, (unsigned)g_srx.overflow);
#endif
#if defined(CONFIG_BOARD_ESP32C6) && !CHAN_PACKET
    {
        uint32_t lr, cr, ct;
        usbjtag_llp_counts(&lr, &cr, &ct);
        PUT("wire=%s\nwire_rx_overflow=%u\n", usbjtag_llp_on_uart() ? "uart0" : "usb", (unsigned)lr);
    }
#elif defined(CONFIG_BOARD_RP2350)
    {
        uint32_t rd, ro;
        uart_raw_counts(&rd, &ro);
        PUT("wire=uart0\nuart_baud=%u\nuart_rx_dropped=%u\nuart_rx_overruns=%u\n",
            (unsigned)(g_uart_baud ? g_uart_baud : (uint32_t)CONFIG_LLP_UART_BAUD), (unsigned)rd, (unsigned)ro);
    }
#endif
    PUT("queue_eth_rx=%u/%u\nqueue_eth_tx=%u/%u\nqueue_p9_rx=%u/%u\nqueue_p9_tx=%u/%u\n",
        (unsigned)fq_used(&g_eth_rx), (unsigned)g_eth_rx.nslots, (unsigned)fq_used(&g_eth_tx), (unsigned)g_eth_tx.nslots,
        (unsigned)fq_used(&g_p9_rx), (unsigned)g_p9_rx.nslots, (unsigned)fq_used(&g_p9_tx), (unsigned)g_p9_tx.nslots);
    PUT("queue_full=%u\nmemory_kb=%u\n",
        (unsigned)(g_eth_rx.drops + g_eth_tx.drops + g_p9_rx.drops + g_p9_tx.drops),
        (unsigned)(g_pages * PAGE_SIZE / 1024u));
    if (g_role == LLP_NODE_HOST)
        PUT("radio_mounted=%s\n", g_c6_mounted ? "/c6" : "no");
    else
        PUT("bridge=%s\nto_link=%u\nto_interface=%u\nloop=%s\n", g_bridged ? g_bridged->name : "none",
            (unsigned)g_fwd_to_link, (unsigned)g_fwd_to_if, g_loop ? "on" : "off");
#undef PUT
    return n < cap ? n : cap;
}

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
#if CHAN_PACKET
    cprintf("  packets: bad %u, channel restarts %u\n", (unsigned)g_pkt_bad, (unsigned)g_chan_resets);
    pkt_report();
#else
    cprintf("  serial: %u frames, crc %u, runt %u, bad-len %u, overflow %u, channel errors %u\n",
            (unsigned)g_srx.frames, (unsigned)g_srx.crc_err, (unsigned)g_srx.runt,
            (unsigned)g_srx.bad_len, (unsigned)g_srx.overflow, (unsigned)g_chan_errors);
#endif
    cprintf("  queues: eth rx %u/%u (full %u), eth tx %u/%u (full %u), p9 rx %u/%u (full %u), p9 tx %u/%u (full %u); %u KB\n",
            (unsigned)fq_used(&g_eth_rx), (unsigned)g_eth_rx.nslots, (unsigned)g_eth_rx.drops,
            (unsigned)fq_used(&g_eth_tx), (unsigned)g_eth_tx.nslots, (unsigned)g_eth_tx.drops,
            (unsigned)fq_used(&g_p9_rx), (unsigned)g_p9_rx.nslots, (unsigned)g_p9_rx.drops,
            (unsigned)fq_used(&g_p9_tx), (unsigned)g_p9_tx.nslots, (unsigned)g_p9_tx.drops,
            (unsigned)(g_pages * PAGE_SIZE / 1024u));
#if defined(CONFIG_BOARD_ESP32C6) && !CHAN_PACKET
    uint32_t lr, cr, ct;
    usbjtag_llp_counts(&lr, &cr, &ct);
    cprintf("  %s: llp rx overflow %u, console rx overflow %u, console tx overflow %u\n",
            usbjtag_llp_on_uart() ? "uart0" : "usb", (unsigned)lr, (unsigned)cr, (unsigned)ct);
#elif defined(CONFIG_BOARD_RP2350)
    uint32_t rd, ro;
    uart_raw_counts(&rd, &ro);
    cprintf("  uart0: %u baud, rx ring full %u, rx overrun %u\n",
            (unsigned)(g_uart_baud ? g_uart_baud : (uint32_t)CONFIG_LLP_UART_BAUD), (unsigned)rd, (unsigned)ro);
#endif
    if (g_role == LLP_NODE_RADIO)
        cprintf("  bridge: %s, %u frames to the link, %u to the interface; loop %s, %u looped\n",
                g_bridged ? g_bridged->name : "-", (unsigned)g_fwd_to_link, (unsigned)g_fwd_to_if,
                g_loop ? "on" : "off", (unsigned)g_looped);
}
