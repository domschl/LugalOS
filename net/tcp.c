#include "net/tcp.h"
#include "net/net_internal.h"
#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

/* --- Wire constants --- */
#define TCP_HDR_MIN   20
#define TCP_FIN       0x01
#define TCP_SYN       0x02
#define TCP_RST       0x04
#define TCP_PSH       0x08
#define TCP_ACK       0x10

#define TCP_OPT_END   0
#define TCP_OPT_NOP   1
#define TCP_OPT_MSS   2

/* Our MSS: one full frame minus the IPv4 and TCP headers. Advertised in the
 * SYN-ACK; a peer that offers none is assumed to want the RFC 1122 default. */
#define TCP_OUR_MSS      (NETIF_MTU - IPV4_HDR_LEN - TCP_HDR_MIN)
#define TCP_DEFAULT_MSS  536u

/* Retransmission. No RTT estimator: this is a LAN, the round trip is
 * sub-millisecond, and an estimator's value is in avoiding spurious
 * retransmissions on a path whose delay varies -- which is not this one. A
 * fixed initial timeout with exponential backoff has the property that
 * matters (a lost segment is resent, and a dead peer is given up on) with a
 * tenth of the state. */
#define TCP_RTO_INITIAL_MS  300u
#define TCP_RTO_MAX_MS      4800u
#define TCP_MAX_RETRIES     6

/* Long enough that a retransmitted FIN from the peer still finds the
 * connection, short enough that two slots are not tied up by a conversation
 * that ended. Not the RFC's 2*MSL, which on a LAN would be pure waste. */
#define TCP_TIME_WAIT_MS    2000u

#define TCP_MAX_CONNS 2

typedef enum {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED, TCP_ESTABLISHED,
    TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT, TCP_CLOSING,
    TCP_LAST_ACK, TCP_TIME_WAIT
} tcp_state_t;

static const char *state_name(tcp_state_t s) {
    switch (s) {
        case TCP_CLOSED:       return "CLOSED";
        case TCP_LISTEN:       return "LISTEN";
        case TCP_SYN_SENT:     return "SYN_SENT";
        case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
        case TCP_ESTABLISHED:  return "ESTABLISHED";
        case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
        case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
        case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_CLOSING:      return "CLOSING";
        case TCP_LAST_ACK:     return "LAST_ACK";
        case TCP_TIME_WAIT:    return "TIME_WAIT";
    }
    return "?";
}

typedef struct {
    bool in_use;
    tcp_state_t state;
    uint8_t peer_ip[IPV4_LEN];
    uint16_t local_port, peer_port;

    uint32_t snd_una;      /* oldest unacknowledged byte */
    uint32_t snd_nxt;      /* next byte to send */
    uint32_t snd_wnd;      /* what the peer will accept */
    uint32_t rcv_nxt;      /* next byte we expect */
    uint32_t peer_mss;

    /* Receive: in-order bytes waiting for the 9P server to take a frame.
     * Sized at one msize, because that is the largest thing the layer above
     * ever asks for in one piece. */
    uint8_t *rx;
    uint32_t rx_len;

    /* Send: one complete 9P reply, retained until every byte of it is
     * acknowledged, which is what makes retransmission a matter of rewinding
     * rather than of remembering segments. */
    uint8_t *tx;
    uint32_t tx_len;       /* bytes held */
    uint32_t tx_seq;       /* sequence number of tx[0] */

    uint64_t rto_at_ms;    /* 0 = no timer running */
    uint32_t rto_ms;
    uint32_t retries;
    uint64_t time_wait_at_ms;

    bool fin_sent;
    bool need_ack;         /* something changed that the peer should hear about */

    /* R3b: we opened this one, so we are the 9P *client* on it. tcp_service()
     * must not offer it to the server -- on this wire the peer answers us,
     * exactly the distinction kernel/board.c makes for the UART1 downlink by
     * withholding DEV_F_BACKGROUND_9P. */
    bool is_client;

    /* Bumped every time the slot is reused. The link carries its own epoch in
     * `ctx`, so a mount left holding a link whose connection has since died
     * and been replaced fails cleanly instead of silently attaching itself to
     * a stranger's session -- the one aliasing hazard a fixed slot table
     * has. */
    uint32_t epoch;

    p9_link_t link;
} tcp_conn_t;

static tcp_conn_t g_conns[TCP_MAX_CONNS];
static uint8_t *g_bufs;                /* one palloc run, carved per connection */
static uint16_t g_listen_port;
static bool g_listening;
static uint32_t g_accepted_total, g_reset_total;
/* Not a secure ISN. A LAN-local server behind a pre-shared-key auth gate
 * (phase 18 §1's threat model) does not defend against an off-path attacker
 * guessing sequence numbers; saying so is better than implying otherwise
 * with a hash nobody audited. */
static uint32_t g_iss_counter = 0x1000;
static uint32_t g_ephemeral;

static int ensure_bufs(void);

/* Wrap-safe sequence comparison -- the reason every one of these is a
 * subtraction cast to signed rather than a plain <. */
static inline bool seq_lt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) <  0; }
static inline bool seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static inline bool seq_gt(uint32_t a, uint32_t b)  { return (int32_t)(a - b) >  0; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* The TCP checksum covers the same pseudo-header UDP's does. */
static uint16_t tcp_checksum(const uint8_t *src, const uint8_t *dst,
                             const uint8_t *seg, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < IPV4_LEN; i += 2) sum += rd16(src + i);
    for (uint32_t i = 0; i < IPV4_LEN; i += 2) sum += rd16(dst + i);
    sum += IP_PROTO_TCP;
    sum += len;
    for (uint32_t i = 0; i + 1 < len; i += 2) sum += rd16(seg + i);
    if (len & 1u) sum += (uint32_t)seg[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)(~sum & 0xffffu);
}

/* How much we are willing to receive right now: whatever is free in the
 * receive buffer.
 *
 * §2 said "a fixed receive window of one MSS". Advertising the buffer's real
 * free space instead is not a scope increase -- it is the same buffer and the
 * same code -- and a one-MSS window on a buffer that holds a whole msize
 * would cost a round trip per segment for nothing. A full buffer advertises
 * zero, which is correct and handled: tcp_service() sends a window update as
 * soon as the 9P server takes the frame. */
static uint32_t rcv_window(const tcp_conn_t *c) {
    return P9_MAX_MSIZE - c->rx_len;
}

static int tcp_emit(tcp_conn_t *c, uint8_t flags, uint32_t seq,
                    const uint8_t *data, uint32_t data_len, bool with_mss) {
    uint8_t *seg = net_tx_payload() + IPV4_HDR_LEN;
    uint32_t hdr = TCP_HDR_MIN + (with_mss ? 4u : 0u);

    wr16(seg + 0, c->local_port);
    wr16(seg + 2, c->peer_port);
    wr32(seg + 4, seq);
    wr32(seg + 8, c->rcv_nxt);
    seg[12] = (uint8_t)((hdr / 4u) << 4);
    seg[13] = flags;
    wr16(seg + 14, (uint16_t)(rcv_window(c) > 0xffffu ? 0xffffu : rcv_window(c)));
    seg[16] = 0; seg[17] = 0;
    seg[18] = 0; seg[19] = 0;              /* no urgent pointer, ever */

    if (with_mss) {
        seg[20] = TCP_OPT_MSS; seg[21] = 4;
        wr16(seg + 22, (uint16_t)TCP_OUR_MSS);
    }
    if (data_len) memcpy(seg + hdr, data, data_len);

    uint32_t total = hdr + data_len;
    uint16_t ck = tcp_checksum(net_state()->ip, c->peer_ip, seg, total);
    wr16(seg + 16, ck);

    c->need_ack = false;
    return ip_send(IP_PROTO_TCP, c->peer_ip, total);
}

/* A reset for a segment that belongs to no connection. Built without a
 * tcp_conn_t because by definition there is not one -- which is exactly the
 * case a server must handle without allocating anything. */
static void tcp_reset_stray(const uint8_t peer_ip[IPV4_LEN], const uint8_t *seg,
                            uint32_t seg_len, uint32_t data_len) {
    if (seg[13] & TCP_RST) return;         /* never answer a reset with a reset */
    (void)seg_len;

    uint8_t *out = net_tx_payload() + IPV4_HDR_LEN;
    uint32_t their_seq = rd32(seg + 4);
    uint32_t their_ack = rd32(seg + 8);
    bool they_acked = (seg[13] & TCP_ACK) != 0;

    wr16(out + 0, rd16(seg + 2));          /* our port is their destination */
    wr16(out + 2, rd16(seg + 0));
    if (they_acked) {
        wr32(out + 4, their_ack);
        wr32(out + 8, 0);
        out[13] = TCP_RST;
    } else {
        wr32(out + 4, 0);
        wr32(out + 8, their_seq + data_len + ((seg[13] & (TCP_SYN | TCP_FIN)) ? 1u : 0u));
        out[13] = TCP_RST | TCP_ACK;
    }
    out[12] = (TCP_HDR_MIN / 4u) << 4;
    wr16(out + 14, 0);
    out[16] = 0; out[17] = 0;
    out[18] = 0; out[19] = 0;

    uint16_t ck = tcp_checksum(net_state()->ip, peer_ip, out, TCP_HDR_MIN);
    wr16(out + 16, ck);
    ip_send(IP_PROTO_TCP, peer_ip, TCP_HDR_MIN);
    g_reset_total++;
}

static void conn_release(tcp_conn_t *c) {
    if (c->state != TCP_CLOSED) p9_link_unregister_background(&c->link);
    c->in_use = false;
    c->state = TCP_CLOSED;
    c->epoch++;            /* every link handed out for this slot is now stale */
    c->rx_len = 0;
    c->tx_len = 0;
    c->rto_at_ms = 0;
    c->fin_sent = false;
    c->need_ack = false;
}

static void conn_abort(tcp_conn_t *c) {
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT ||
        c->state == TCP_SYN_RECEIVED || c->state == TCP_FIN_WAIT_1 ||
        c->state == TCP_FIN_WAIT_2) {
        tcp_emit(c, TCP_RST, c->snd_nxt, NULL, 0, false);
        g_reset_total++;
    }
    conn_release(c);
}

/* --- p9_link_t glue: an accepted connection *is* a link ---
 *
 * This is what makes phase 18 a sunk cost of days rather than weeks. The auth
 * gate, `auth_required`, the background service, `p9auth` and the entire host
 * side connect to a byte stream, and TCP is one. */

static tcp_conn_t *link_conn(p9_link_t *link) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &g_conns[i];
        if (&c->link != link) continue;
        /* The epoch check is what makes a stale link safe to hold. */
        if ((uint32_t)(uintptr_t)link->ctx != c->epoch) return NULL;
        return c;
    }
    return NULL;
}

/* 9P frames are length-prefixed, so the stream needs no further framing --
 * the same reasoning drivers/virtio_console.c's link uses. */
static uint32_t framed_len(const tcp_conn_t *c) {
    if (c->rx_len < 4) return 0;
    return (uint32_t)c->rx[0] | ((uint32_t)c->rx[1] << 8) |
           ((uint32_t)c->rx[2] << 16) | ((uint32_t)c->rx[3] << 24);
}

static int link_poll(p9_link_t *link) {
    tcp_conn_t *c = link_conn(link);
    if (!c || !c->in_use) return -1;
    uint32_t declared = framed_len(c);
    if (declared == 0) return 0;
    if (declared < 7 || declared > P9_MAX_MSIZE) return -1;   /* corrupt stream */
    return (c->rx_len >= declared) ? 1 : 0;
}

static int link_recv_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    tcp_conn_t *c = link_conn(link);
    if (!c || !c->in_use) return -1;
    uint32_t declared = framed_len(c);
    if (declared < 7 || declared > max_len || c->rx_len < declared) return -1;

    memcpy(buf, c->rx, declared);
    c->rx_len -= declared;
    if (c->rx_len) memmove(c->rx, c->rx + declared, c->rx_len);
    /* The window just reopened. Saying so promptly is what keeps a peer that
     * filled the buffer from waiting on its own persist timer. */
    c->need_ack = true;
    return (int)declared;
}

static int link_send_frame(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    tcp_conn_t *c = link_conn(link);
    if (!c || !c->in_use || len == 0 || len > P9_MAX_MSIZE) return -1;
    /* Never blocks. tcp_service() only offers a connection to the 9P server
     * when its send buffer is already empty, so this cannot be reached with
     * one outstanding -- and if it somehow is, refusing is right: blocking
     * here would be blocking the only task that can drain it. */
    if (c->tx_len) return -1;

    memcpy(c->tx, buf, len);
    c->tx_len = len;
    c->tx_seq = c->snd_nxt;
    return (int)len;
}

static void conn_init_link(tcp_conn_t *c) {
    c->link.name = c->is_client ? "tcpout" : "tcpnet";
    c->link.poll = link_poll;
    c->link.send_frame = link_send_frame;
    c->link.recv_frame = link_recv_frame;
    c->link.ctx = (void *)(uintptr_t)c->epoch;
    /* A network link, so the gate applies (phase 18 N2). This is the whole
     * reason `auth_required` lives on the link rather than in the server. */
    c->link.auth_required = true;
}

/* --- Transmit --- */

/* Sends whatever the window allows from the send buffer, plus a FIN when the
 * buffer has drained and one is due. */
static void tcp_transmit(tcp_conn_t *c) {
    uint32_t in_flight = c->snd_nxt - c->snd_una;
    uint32_t held = c->tx_len;
    uint32_t offset = c->snd_nxt - c->tx_seq;

    while (held > offset) {
        uint32_t usable = (c->snd_wnd > in_flight) ? c->snd_wnd - in_flight : 0;
        if (usable == 0) break;
        uint32_t chunk = held - offset;
        if (chunk > c->peer_mss) chunk = c->peer_mss;
        if (chunk > usable) chunk = usable;
        if (chunk == 0) break;

        tcp_emit(c, TCP_ACK | TCP_PSH, c->snd_nxt, c->tx + offset, chunk, false);
        c->snd_nxt += chunk;
        in_flight += chunk;
        offset += chunk;
        if (c->rto_at_ms == 0) {
            c->rto_ms = TCP_RTO_INITIAL_MS;
            c->rto_at_ms = time_get_ms() + c->rto_ms;
            c->retries = 0;
        }
    }

    if (c->need_ack) tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0, false);
}

static void tcp_retransmit(tcp_conn_t *c) {
    uint32_t held = c->tx_len;
    uint32_t offset = c->snd_una - c->tx_seq;
    if (held > offset) {
        uint32_t chunk = held - offset;
        if (chunk > c->peer_mss) chunk = c->peer_mss;
        tcp_emit(c, TCP_ACK | TCP_PSH, c->snd_una, c->tx + offset, chunk, false);
    } else if (c->fin_sent) {
        tcp_emit(c, TCP_ACK | TCP_FIN, c->snd_una, NULL, 0, false);
    } else if (c->state == TCP_SYN_RECEIVED) {
        tcp_emit(c, TCP_SYN | TCP_ACK, c->snd_una, NULL, 0, true);
    } else if (c->state == TCP_SYN_SENT) {
        tcp_emit(c, TCP_SYN, c->snd_una, NULL, 0, true);
    }
    c->rto_ms = (c->rto_ms * 2u > TCP_RTO_MAX_MS) ? TCP_RTO_MAX_MS : c->rto_ms * 2u;
    c->rto_at_ms = time_get_ms() + c->rto_ms;
    c->retries++;
}

static void send_fin(tcp_conn_t *c) {
    c->fin_sent = true;
    tcp_emit(c, TCP_ACK | TCP_FIN, c->snd_nxt, NULL, 0, false);
    c->snd_nxt++;
    if (c->rto_at_ms == 0) {
        c->rto_ms = TCP_RTO_INITIAL_MS;
        c->rto_at_ms = time_get_ms() + c->rto_ms;
        c->retries = 0;
    }
}

/* --- Receive --- */

static tcp_conn_t *find_conn(const uint8_t src[IPV4_LEN], uint16_t sport, uint16_t dport) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &g_conns[i];
        if (c->in_use && c->peer_port == sport && c->local_port == dport &&
            memcmp(c->peer_ip, src, IPV4_LEN) == 0) return c;
    }
    return NULL;
}

static tcp_conn_t *free_conn(void) {
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        if (!g_conns[i].in_use) return &g_conns[i];
    }
    return NULL;
}

static uint32_t parse_mss(const uint8_t *seg, uint32_t hdr_len) {
    uint32_t i = TCP_HDR_MIN;
    while (i + 1 < hdr_len) {
        uint8_t kind = seg[i];
        if (kind == TCP_OPT_END) break;
        if (kind == TCP_OPT_NOP) { i++; continue; }
        uint8_t olen = seg[i + 1];
        if (olen < 2 || i + olen > hdr_len) break;      /* malformed: stop, do not guess */
        if (kind == TCP_OPT_MSS && olen == 4) return rd16(seg + i + 2);
        i += olen;
    }
    return 0;
}

static void accept_syn(const uint8_t src[IPV4_LEN], const uint8_t *seg,
                       uint16_t sport, uint16_t dport, uint32_t hdr_len) {
    tcp_conn_t *c = free_conn();
    if (!c) {
        /* Both slots busy. A reset is the honest answer -- better than a
         * silent drop, which looks to the client like a black hole. */
        tcp_reset_stray(src, seg, hdr_len, 0);
        return;
    }
    memset(c, 0, sizeof(*c));
    c->in_use = true;
    c->state = TCP_SYN_RECEIVED;
    memcpy(c->peer_ip, src, IPV4_LEN);
    c->peer_port = sport;
    c->local_port = dport;
    c->rx = g_bufs + (uint32_t)(c - g_conns) * (P9_MAX_MSIZE * 2u);
    c->tx = c->rx + P9_MAX_MSIZE;

    uint32_t mss = parse_mss(seg, hdr_len);
    c->peer_mss = mss ? mss : TCP_DEFAULT_MSS;
    if (c->peer_mss > TCP_OUR_MSS) c->peer_mss = TCP_OUR_MSS;

    c->rcv_nxt = rd32(seg + 4) + 1;              /* the SYN itself consumes one */
    c->snd_una = g_iss_counter;
    c->snd_nxt = g_iss_counter;
    g_iss_counter += 0x10000u + (uint32_t)time_get_ms();
    c->snd_wnd = rd16(seg + 14);

    conn_init_link(c);
    tcp_emit(c, TCP_SYN | TCP_ACK, c->snd_nxt, NULL, 0, true);
    c->snd_nxt++;                                 /* our SYN consumes one too */
    c->rto_ms = TCP_RTO_INITIAL_MS;
    c->rto_at_ms = time_get_ms() + c->rto_ms;
    c->retries = 0;
}

/* R3b: the active open.
 *
 * Half the state machine was unreachable without it -- SYN_SENT never
 * entered, and FIN_WAIT_1/FIN_WAIT_2/CLOSING/TIME_WAIT only reachable if we
 * are the side that closes, which nothing was. It is also what lets one node
 * mount another's namespace over IP rather than over a cable, which is phase
 * 5's distributed story finally running on a network.
 *
 * Returns a link that is not yet usable: the caller polls tcp_link_ready()
 * until the handshake completes. Non-blocking on purpose -- the task that
 * would have to wait is often the one whose pump completes it. */
p9_link_t *tcp_connect(const uint8_t ip[IPV4_LEN], uint16_t port) {
    if (!net_configured() || !ip || port == 0) return NULL;
    if (ensure_bufs() != 0) return NULL;

    tcp_conn_t *c = free_conn();
    if (!c) return NULL;

    uint32_t epoch = c->epoch;
    memset(c, 0, sizeof(*c));
    c->epoch = epoch;
    c->in_use = true;
    c->is_client = true;
    c->state = TCP_SYN_SENT;
    memcpy(c->peer_ip, ip, IPV4_LEN);
    c->peer_port = port;
    /* An ephemeral source port from the IANA dynamic range, stepped per
     * connection so a reconnect after a reset does not land on the four-tuple
     * the peer may still be holding in TIME_WAIT. */
    c->local_port = (uint16_t)(49152u + (g_ephemeral++ & 0x3fffu));
    c->rx = g_bufs + (uint32_t)(c - g_conns) * (P9_MAX_MSIZE * 2u);
    c->tx = c->rx + P9_MAX_MSIZE;
    c->peer_mss = TCP_DEFAULT_MSS;
    c->snd_una = g_iss_counter;
    c->snd_nxt = g_iss_counter;
    g_iss_counter += 0x10000u + (uint32_t)time_get_ms();
    c->snd_wnd = TCP_OUR_MSS;      /* until the peer tells us otherwise */

    conn_init_link(c);
    tcp_emit(c, TCP_SYN, c->snd_nxt, NULL, 0, true);
    c->snd_nxt++;
    c->rto_ms = TCP_RTO_INITIAL_MS;
    c->rto_at_ms = time_get_ms() + c->rto_ms;
    c->retries = 0;
    return &c->link;
}

/* 1 established, 0 still handshaking, -1 refused or gone. */
int tcp_link_ready(p9_link_t *link) {
    tcp_conn_t *c = link_conn(link);
    if (!c || !c->in_use) return -1;
    if (c->state == TCP_ESTABLISHED) return 1;
    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED) return 0;
    return -1;
}

/* Our own close: the graceful one, which is the path FIN_WAIT_1 exists for. */
void tcp_close(p9_link_t *link) {
    tcp_conn_t *c = link_conn(link);
    if (!c || !c->in_use) return;
    if (c->state == TCP_ESTABLISHED) {
        send_fin(c);
        c->state = TCP_FIN_WAIT_1;
    } else if (c->state == TCP_CLOSE_WAIT) {
        send_fin(c);
        c->state = TCP_LAST_ACK;
    } else {
        conn_abort(c);
    }
}

void tcp_input(const uint8_t src[IPV4_LEN], const uint8_t *ip_hdr,
               const uint8_t *seg, uint32_t len) {
    net_state_t *st = net_mutable_state();
    (void)ip_hdr;
    if (len < TCP_HDR_MIN) { st->drop_short++; return; }

    uint32_t hdr_len = (uint32_t)(seg[12] >> 4) * 4u;
    if (hdr_len < TCP_HDR_MIN || hdr_len > len) { st->drop_short++; return; }

    if (tcp_checksum(src, st->ip, seg, len) != 0) { st->drop_checksum++; return; }

    uint16_t sport = rd16(seg + 0);
    uint16_t dport = rd16(seg + 2);
    uint32_t seq = rd32(seg + 4);
    uint32_t ack = rd32(seg + 8);
    uint8_t flags = seg[13];
    uint32_t wnd = rd16(seg + 14);
    const uint8_t *data = seg + hdr_len;
    uint32_t data_len = len - hdr_len;

    tcp_conn_t *c = find_conn(src, sport, dport);
    if (!c) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK) && g_listening && dport == g_listen_port) {
            accept_syn(src, seg, sport, dport, hdr_len);
        } else {
            tcp_reset_stray(src, seg, len, data_len);
        }
        return;
    }

    if (flags & TCP_RST) {
        /* An in-window reset kills the connection. Out of window it is
         * ignored -- accepting one on faith is the blind-reset attack, and
         * the check costs a comparison. */
        if (seq == c->rcv_nxt || (seq_leq(c->rcv_nxt, seq) &&
                                  seq_lt(seq, c->rcv_nxt + rcv_window(c)))) {
            conn_release(c);
        }
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        /* The only segment that means anything here is the SYN-ACK for the
         * SYN we sent. Anything else is either a stale duplicate or an
         * attempt to hijack, and both deserve the same silence. */
        if (!(flags & TCP_SYN)) return;
        if (!(flags & TCP_ACK)) {
            /* Simultaneous open: both sides dialled. Legal, and not
             * implemented -- a client and a server cannot both be dialling
             * in this system, since only tcp_connect() sends a bare SYN and
             * only a listener answers one. Counted, not guessed at. */
            st->drop_proto++;
            return;
        }
        if (ack != c->snd_nxt) {
            /* Acknowledging a SYN we did not send. */
            conn_abort(c);
            return;
        }
        c->snd_una = ack;
        c->rcv_nxt = seq + 1;
        c->snd_wnd = wnd;
        uint32_t mss = parse_mss(seg, hdr_len);
        c->peer_mss = mss ? mss : TCP_DEFAULT_MSS;
        if (c->peer_mss > TCP_OUR_MSS) c->peer_mss = TCP_OUR_MSS;
        c->state = TCP_ESTABLISHED;
        c->rto_at_ms = 0;
        c->retries = 0;
        tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0, false);
        return;
    }

    if (flags & TCP_ACK) {
        if (seq_gt(ack, c->snd_nxt)) {
            /* Acknowledging something we never sent. */
            tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0, false);
            return;
        }
        if (seq_gt(ack, c->snd_una)) {
            c->snd_una = ack;
            c->retries = 0;
            /* Drop the acknowledged prefix of the send buffer. */
            uint32_t acked = (c->tx_len && seq_gt(ack, c->tx_seq))
                             ? (ack - c->tx_seq) : 0;
            if (acked >= c->tx_len) {
                c->tx_len = 0;
            } else if (acked) {
                memmove(c->tx, c->tx + acked, c->tx_len - acked);
                c->tx_len -= acked;
                c->tx_seq += acked;
            }
            c->rto_at_ms = (c->snd_una == c->snd_nxt) ? 0
                                                      : time_get_ms() + TCP_RTO_INITIAL_MS;
            if (c->rto_at_ms) c->rto_ms = TCP_RTO_INITIAL_MS;
        }
    }
    c->snd_wnd = wnd;

    switch (c->state) {
        case TCP_SYN_RECEIVED:
            if (!(flags & TCP_ACK)) return;
            if (seq_lt(c->snd_una, c->snd_nxt)) return;   /* our SYN is still unacked */
            c->state = TCP_ESTABLISHED;
            g_accepted_total++;
            /* Server side only: a link we dialled is answered *by* the peer,
             * so registering it for inbound service would have the background
             * pump racing our own client for its replies. */
            p9_link_register_background(&c->link);
            break;
        case TCP_FIN_WAIT_1:
            if (c->fin_sent && c->snd_una == c->snd_nxt) c->state = TCP_FIN_WAIT_2;
            break;
        case TCP_LAST_ACK:
            if (c->snd_una == c->snd_nxt) { conn_release(c); return; }
            break;
        case TCP_CLOSING:
            if (c->snd_una == c->snd_nxt) {
                c->state = TCP_TIME_WAIT;
                c->time_wait_at_ms = time_get_ms() + TCP_TIME_WAIT_MS;
            }
            break;
        default:
            break;
    }

    /* Data. In order only: anything else is dropped, and answered with a
     * duplicate ACK so the peer retransmits now rather than after its RTO.
     * §2 records why there is no reassembly queue. */
    if (data_len) {
        if (seq != c->rcv_nxt) {
            st->drop_proto++;                  /* counted; see /proc/net */
            tcp_emit(c, TCP_ACK, c->snd_nxt, NULL, 0, false);
            return;
        }
        uint32_t space = rcv_window(c);
        if (data_len > space) {
            /* More than we advertised. Take what fits and acknowledge only
             * that -- truncating silently would desynchronise the stream. */
            data_len = space;
        }
        if (data_len) {
            memcpy(c->rx + c->rx_len, data, data_len);
            c->rx_len += data_len;
            c->rcv_nxt += data_len;
        }
        c->need_ack = true;
    }

    if ((flags & TCP_FIN) && seq + data_len == c->rcv_nxt) {
        c->rcv_nxt++;                          /* the FIN occupies one */
        c->need_ack = true;
        switch (c->state) {
            case TCP_ESTABLISHED:
                c->state = TCP_CLOSE_WAIT;
                break;
            case TCP_FIN_WAIT_1:
                c->state = TCP_CLOSING;
                break;
            case TCP_FIN_WAIT_2:
                c->state = TCP_TIME_WAIT;
                c->time_wait_at_ms = time_get_ms() + TCP_TIME_WAIT_MS;
                break;
            default:
                break;
        }
    }

    tcp_transmit(c);
}

/* --- The pump's one call --- */

void tcp_service(void) {
    uint64_t now = time_get_ms();

    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_t *c = &g_conns[i];
        if (!c->in_use) continue;

        if (c->state == TCP_TIME_WAIT) {
            if ((int64_t)(now - c->time_wait_at_ms) >= 0) conn_release(c);
            continue;
        }

        if (c->rto_at_ms && (int64_t)(now - c->rto_at_ms) >= 0) {
            if (c->retries >= TCP_MAX_RETRIES) {
                /* Six attempts over roughly ten seconds. A peer that has said
                 * nothing in that time is gone, and holding one of two slots
                 * for it costs the next client its connection. */
                conn_abort(c);
                continue;
            }
            tcp_retransmit(c);
        }

        /* Service 9P only when the send buffer is empty, which is what lets
         * link_send_frame() be non-blocking: the reply always has somewhere
         * to go. */
        if (c->state == TCP_ESTABLISHED && c->tx_len == 0 && !c->is_client) {
            p9_link_service(&c->link);
        }

        /* A peer that has closed and whose reply has drained gets our FIN. */
        if (c->state == TCP_CLOSE_WAIT && c->tx_len == 0 && !c->fin_sent && !c->is_client) {
            send_fin(c);
            c->state = TCP_LAST_ACK;
        }

        tcp_transmit(c);
    }
}

/* --- Setup and reporting --- */

/* One palloc run for every connection's pair of buffers, taken the first
 * time anything wants a connection at all -- R2's rule, unchanged: a board
 * that neither listens nor dials pays nothing. */
static int ensure_bufs(void) {
    if (g_bufs) return 0;
    uint32_t bytes = TCP_MAX_CONNS * P9_MAX_MSIZE * 2u;
    uint32_t pages = (bytes + 4095u) / 4096u;
    g_bufs = (uint8_t *)palloc_pages(pages);
    if (!g_bufs) {
        printk("[TCP] No memory for connection buffers (%u pages).\n", pages);
        return -1;
    }
    return 0;
}

int tcp_listen(uint16_t port) {
    if (port == 0) return -1;
    if (ensure_bufs() != 0) return -1;
    g_listen_port = port;
    g_listening = true;
    return 0;
}

void tcp_unlisten(void) { g_listening = false; }

bool tcp_listening(uint16_t *port_out) {
    if (port_out && g_listening) *port_out = g_listen_port;
    return g_listening;
}

uint32_t tcp_conn_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++) if (g_conns[i].in_use) n++;
    return n;
}

uint32_t tcp_accepted_total(void) { return g_accepted_total; }
uint32_t tcp_reset_total(void) { return g_reset_total; }

void tcp_conn_str(uint32_t index, char *out, uint32_t max) {
    if (!out || max == 0) return;
    out[0] = '\0';
    if (index >= TCP_MAX_CONNS || !g_conns[index].in_use) return;
    const tcp_conn_t *c = &g_conns[index];
    ksnprintf(out, max, "%u.%u.%u.%u:%u -> :%u %s rx %lu tx %lu\n",
              c->peer_ip[0], c->peer_ip[1], c->peer_ip[2], c->peer_ip[3],
              c->peer_port, c->local_port, state_name(c->state),
              (unsigned long)c->rx_len, (unsigned long)c->tx_len);
}
