/* LLP, the LugalOS link protocol (49.1, plan/phase49_c6_network_nodes.md).
 * doc/dev/protocols/llp.md is the specification; kernel/include/kernel/llp.h says what this
 * file is and is not. Every rule below names the section of doc/dev/protocols/llp.md it
 * implements, and host/llp/src/llp/link.py is the same state machine in
 * Python -- tests/host/llp_vectors.txt is how the two are kept equal. */

#include "kernel/llp.h"
#include "kernel/crc32.h"
#include "drivers/slip.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Wrap-safe "at least `ms` since `t`". */
static bool elapsed(uint32_t now, uint32_t t, uint32_t ms) { return (uint32_t)(now - t) >= ms; }

/* --- Framing (§3) ------------------------------------------------------------ */

void llp_hdr_put(uint8_t hdr[LLP_HDR_LEN], uint8_t type, uint8_t seq, uint16_t len) {
    hdr[0] = type;
    hdr[1] = seq;
    put16(&hdr[2], len);
}

int llp_frame_parse(const uint8_t *buf, uint32_t n, llp_frame_t *out) {
    if (n < LLP_HDR_LEN) return -1;
    uint16_t len = get16(&buf[2]);
    if ((uint32_t)len != n - LLP_HDR_LEN) return -2;
    out->type = buf[0];
    out->seq = buf[1];
    out->len = len;
    out->payload = &buf[LLP_HDR_LEN];
    return 0;
}

/* --- Serial binding (§7.1) ----------------------------------------------------- */

int llp_serial_encode(const uint8_t hdr[LLP_HDR_LEN], const uint8_t *payload, uint16_t len,
                      uint8_t *dst, uint32_t cap) {
    if (cap < 2) return -1;
    uint32_t crc = crc32_update(0xFFFFFFFFu, hdr, LLP_HDR_LEN);
    crc = ~crc32_update(crc, payload, len);
    uint8_t crc_le[LLP_CRC_LEN];
    put32(crc_le, crc);

    uint32_t out = 0;
    dst[out++] = SLIP_END;
    if (slip_append(dst, &out, cap, hdr, LLP_HDR_LEN) != 0) return -1;
    if (slip_append(dst, &out, cap, payload, len) != 0) return -1;
    if (slip_append(dst, &out, cap, crc_le, LLP_CRC_LEN) != 0) return -1;
    if (out + 1 > cap) return -1;
    dst[out++] = SLIP_END;
    return (int)out;
}

static void emit_escaped(const uint8_t *p, uint32_t n, llp_put_fn put, void *ctx) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == SLIP_END) { put(ctx, SLIP_ESC); put(ctx, SLIP_ESC_END); }
        else if (p[i] == SLIP_ESC) { put(ctx, SLIP_ESC); put(ctx, SLIP_ESC_ESC); }
        else put(ctx, p[i]);
    }
}

void llp_serial_emit(const uint8_t hdr[LLP_HDR_LEN], const uint8_t *payload, uint16_t len,
                     llp_put_fn put, void *ctx) {
    uint32_t crc = crc32_update(0xFFFFFFFFu, hdr, LLP_HDR_LEN);
    crc = ~crc32_update(crc, payload, len);
    uint8_t crc_le[LLP_CRC_LEN];
    put32(crc_le, crc);
    put(ctx, SLIP_END);
    emit_escaped(hdr, LLP_HDR_LEN, put, ctx);
    emit_escaped(payload, len, put, ctx);
    emit_escaped(crc_le, LLP_CRC_LEN, put, ctx);
    put(ctx, SLIP_END);
}

void llp_serial_rx_init(llp_serial_rx_t *rx, uint8_t *buf, uint32_t cap) {
    memset(rx, 0, sizeof(*rx));
    rx->buf = buf;
    rx->cap = cap;
}

int llp_serial_feed(llp_serial_rx_t *rx, uint8_t c, llp_frame_t *out) {
    bool was_discarding = (rx->len == SLIP_DISCARDING);
    if (!slip_feed(c, rx->buf, rx->cap, &rx->len, &rx->esc)) {
        if (!was_discarding && rx->len == SLIP_DISCARDING) rx->overflow++;
        return 0;
    }
    uint32_t n = rx->len;
    rx->len = 0;
    if (n < LLP_HDR_LEN + LLP_CRC_LEN) { rx->runt++; return 0; }

    uint32_t body = n - LLP_CRC_LEN;
    if (crc32_compute(rx->buf, body) != get32(&rx->buf[body])) { rx->crc_err++; return 0; }
    if (llp_frame_parse(rx->buf, body, out) != 0) { rx->bad_len++; return 0; }
    rx->frames++;
    return 1;
}

/* --- The link (§4-§6) ----------------------------------------------------------- */

static uint8_t my_major(const llp_link_t *l) { return l->cfg.major ? l->cfg.major : LLP_VERSION_MAJOR; }
static uint8_t my_minor(const llp_link_t *l) { return l->cfg.major ? l->cfg.minor : LLP_VERSION_MINOR; }

static void event(llp_link_t *l, llp_event_t ev) {
    if (l->cfg.event) l->cfg.event(l->cfg.ctx, ev, l);
}

static int emit(llp_link_t *l, uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
                uint32_t now) {
    uint8_t hdr[LLP_HDR_LEN];
    llp_hdr_put(hdr, type, seq, len);
    if (l->cfg.send(l->cfg.ctx, hdr, payload, len) != 0) {
        l->st.tx_refused++;
        return -1;
    }
    l->last_tx = now;
    if (type == LLP_T_LINK) l->st.tx_link++;
    return 0;
}

/* The receive window we can offer now (§5.2): what the binding can hold,
 * never more than we configured. 0 when the channel does flow control. */
static uint8_t rx_window_now(const llp_link_t *l) {
    if (!l->cfg.rx_window) return 0;
    uint32_t free = l->cfg.rx_free(l->cfg.ctx);
    return (uint8_t)(free < l->cfg.rx_window ? free : l->cfg.rx_window);
}

static void send_hello(llp_link_t *l, uint32_t now) {
    uint8_t p[LLP_HELLO_LEN];
    uint8_t window = rx_window_now(l);
    p[0] = LLP_OP_HELLO;
    p[1] = my_major(l);
    p[2] = my_minor(l);
    p[3] = l->cfg.role;
    p[4] = (uint8_t)((l->state == LLP_UP ? LLP_HELLO_F_UP : 0) | (l->cfg.rx_window ? LLP_HELLO_F_FLOW : 0));
    put32(&p[5], l->session);
    put32(&p[9], l->peer_session);
    put16(&p[13], l->cfg.max_payload);
    p[15] = window;
    memcpy(&p[16], l->cfg.mac, 6);
    l->adv_limit = (uint8_t)(l->rx_next + window);
    l->last_hello = now;
    l->st.hellos++;
    (void)emit(l, LLP_T_LINK, 0, p, LLP_HELLO_LEN, now);
}

static void send_credit(llp_link_t *l, uint8_t window, uint32_t now) {
    uint8_t p[LLP_CREDIT_LEN] = { LLP_OP_CREDIT, l->rx_next, window };
    l->adv_limit = (uint8_t)(l->rx_next + window);
    l->st.credits++;
    (void)emit(l, LLP_T_LINK, 0, p, LLP_CREDIT_LEN, now);
}

/* §5.2: advertise when the window's far edge has moved by a quarter of the
 * window (at least one frame) -- or always, when `force` (a PROBE answered, a
 * keepalive). A link without credits sends CREDIT only as the keepalive. */
static void maybe_credit(llp_link_t *l, bool force, uint32_t now) {
    if (!l->cfg.rx_window) {
        if (force) send_credit(l, 0, now);
        return;
    }
    uint8_t window = rx_window_now(l);
    uint8_t moved = (uint8_t)((uint8_t)(l->rx_next + window) - l->adv_limit);
    uint8_t thresh = (uint8_t)(l->cfg.rx_window / 4u ? l->cfg.rx_window / 4u : 1u);
    if (force || (moved >= thresh && moved < 128u)) send_credit(l, window, now);
}

bool llp_link_can_send(const llp_link_t *l) {
    if (l->state != LLP_UP) return false;
    if (!(l->peer_flags & LLP_HELLO_F_FLOW)) return true;
    return (uint8_t)(l->tx_seq - l->tx_ack) < l->tx_window;
}

void llp_link_init(llp_link_t *l, const llp_config_t *cfg, uint32_t session, uint32_t now) {
    memset(l, 0, sizeof(*l));
    l->cfg = *cfg;
    if (l->cfg.rx_window > LLP_MAX_WINDOW) l->cfg.rx_window = LLP_MAX_WINDOW;
    if (l->cfg.max_payload > LLP_MAX_PAYLOAD) l->cfg.max_payload = LLP_MAX_PAYLOAD;
    l->state = LLP_DOWN;
    l->session = session;
    l->last_rx = now;
    l->last_tx = now;
    send_hello(l, now);
}

/* §4: the handshake. */
static void on_hello(llp_link_t *l, const uint8_t *p, uint16_t len, uint32_t now) {
    if (len < LLP_HELLO_LEN) { l->st.rx_bad++; return; }
    uint32_t session = get32(&p[5]);
    uint32_t seen = get32(&p[9]);
    if (session == 0) { l->st.rx_bad++; return; }

    if (session != l->peer_session) {
        /* A peer we have not seen, or one that restarted: everything about the
         * old session is gone, in both directions (§4.3). */
        bool was_up = (l->state == LLP_UP);
        l->peer_session = session;
        l->peer_major = p[1];
        l->peer_minor = p[2];
        l->peer_role = p[3];
        l->peer_flags = p[4];
        l->peer_max_payload = get16(&p[13]);
        memcpy(l->peer_mac, &p[16], 6);
        l->peer_link = 0;
        l->peer_rssi = 0;
        l->tx_seq = 0;
        l->tx_ack = 0;
        l->tx_window = (uint8_t)(p[15] < LLP_MAX_WINDOW ? p[15] : LLP_MAX_WINDOW);
        l->blocked = false;
        l->rx_next = 0;
        if (was_up) {
            l->state = LLP_DOWN;
            l->st.downs++;
            event(l, LLP_EV_DOWN);
        }
        if (l->peer_major != my_major(l) || l->peer_max_payload < LLP_MIN_PAYLOAD) {
            /* §4.4: answer once, so the peer reports it too, then stay quiet
             * until it restarts. */
            l->state = LLP_MISMATCH;
            event(l, LLP_EV_MISMATCH);
            send_hello(l, now);
            return;
        }
        if (l->state == LLP_MISMATCH) l->state = LLP_DOWN;
    } else {
        if (l->state == LLP_MISMATCH) return;
        l->peer_flags = p[4];
    }

    if (seen == l->session) {
        if (l->state != LLP_UP) {
            l->state = LLP_UP;
            l->last_rx = now;
            l->st.ups++;
            event(l, LLP_EV_UP);
        }
        if (!(p[4] & LLP_HELLO_F_UP)) send_hello(l, now);
    } else {
        send_hello(l, now);
    }
}

/* §5.1: the peer's receive window. */
static void on_credit(llp_link_t *l, const uint8_t *p, uint16_t len) {
    if (len < LLP_CREDIT_LEN) { l->st.rx_bad++; return; }
    if (l->state != LLP_UP) return;
    uint8_t ack = p[1];
    if ((uint8_t)(ack - l->tx_ack) > (uint8_t)(l->tx_seq - l->tx_ack)) {
        l->st.rx_bad++;                 /* acknowledges frames never sent */
        return;
    }
    l->tx_ack = ack;
    l->tx_window = (uint8_t)(p[2] < LLP_MAX_WINDOW ? p[2] : LLP_MAX_WINDOW);
    if (llp_link_can_send(l)) l->blocked = false;
}

/* §5.3: everything before `next` was sent; what has not arrived never will. */
static void on_probe(llp_link_t *l, const uint8_t *p, uint16_t len, uint32_t now) {
    if (len < LLP_PROBE_LEN) { l->st.rx_bad++; return; }
    if (l->state != LLP_UP) return;
    uint8_t gap = (uint8_t)(p[1] - l->rx_next);
    if (gap < 128u) {
        l->st.rx_lost += gap;
        l->rx_next = p[1];
    }
    maybe_credit(l, true, now);
}

static void on_state(llp_link_t *l, const uint8_t *p, uint16_t len) {
    if (len < LLP_STATE_LEN) { l->st.rx_bad++; return; }
    if (l->state != LLP_UP) return;
    l->peer_link = p[1];
    l->peer_rssi = (int8_t)p[2];
    event(l, LLP_EV_PEER_STATE);
}

int llp_link_input(llp_link_t *l, const llp_frame_t *f, uint32_t now, llp_frame_t *out) {
    l->last_rx = now;

    if (f->type == LLP_T_LINK) {
        l->st.rx_link++;
        if (f->len < 1) { l->st.rx_bad++; return 0; }
        switch (f->payload[0]) {
            case LLP_OP_HELLO:  on_hello(l, f->payload, f->len, now); break;
            case LLP_OP_CREDIT: on_credit(l, f->payload, f->len); break;
            case LLP_OP_PROBE:  on_probe(l, f->payload, f->len, now); break;
            case LLP_OP_STATE:  on_state(l, f->payload, f->len); break;
            default:            l->st.rx_bad++; break;
        }
        return 0;
    }
    if (f->type != LLP_T_ETH && f->type != LLP_T_P9) { l->st.rx_bad++; return 0; }
    if (l->state != LLP_UP) { l->st.rx_not_up++; return 0; }
    if (f->len > l->cfg.max_payload) { l->st.rx_too_big++; return 0; }

    /* §5.3: sequence. A gap is frames lost on the wire; behind is a stale one. */
    uint8_t gap = (uint8_t)(f->seq - l->rx_next);
    if (gap >= 128u) { l->st.rx_stale++; return 0; }
    l->st.rx_lost += gap;
    l->rx_next = (uint8_t)(f->seq + 1u);

    /* §3.2: the payload's shape. */
    if (f->type == LLP_T_ETH && f->len < 14u) { l->st.rx_bad++; return 0; }
    if (f->type == LLP_T_P9 && (f->len < 7u || get32(f->payload) != f->len)) { l->st.rx_bad++; return 0; }

    l->st.rx_frames++;
    *out = *f;
    return 1;
}

int llp_link_send(llp_link_t *l, uint8_t type, const uint8_t *payload, uint16_t len, uint32_t now) {
    if (l->state != LLP_UP) return -1;
    if ((type != LLP_T_ETH && type != LLP_T_P9) || len > l->peer_max_payload) return -3;
    if (!llp_link_can_send(l)) {
        if (!l->blocked) {          /* counted once per episode, not per retry */
            l->blocked = true;
            l->last_probe = now;
            l->st.tx_no_credit++;
        }
        return -2;
    }
    if (emit(l, type, l->tx_seq, payload, len, now) != 0) return -4;
    l->tx_seq++;
    l->st.tx_frames++;
    return 0;
}

void llp_link_send_state(llp_link_t *l, uint8_t link_up, int8_t rssi, uint32_t now) {
    if (l->state != LLP_UP) return;
    uint8_t p[LLP_STATE_LEN] = { LLP_OP_STATE, link_up, (uint8_t)rssi, 0, 0 };
    (void)emit(l, LLP_T_LINK, 0, p, LLP_STATE_LEN, now);
}

/* §6: the timers. */
void llp_link_tick(llp_link_t *l, uint32_t now) {
    if (l->state == LLP_DOWN) {
        if (elapsed(now, l->last_hello, LLP_HELLO_RETRY_MS)) send_hello(l, now);
        return;
    }
    if (l->state != LLP_UP) return;     /* MISMATCH: wait for the peer to restart */

    if (elapsed(now, l->last_rx, LLP_DEAD_MS)) {
        l->state = LLP_DOWN;
        l->st.downs++;
        event(l, LLP_EV_DOWN);
        send_hello(l, now);
        return;
    }
    maybe_credit(l, false, now);
    if (l->blocked) {
        if (llp_link_can_send(l)) {
            l->blocked = false;
        } else if (elapsed(now, l->last_probe, LLP_PROBE_MS)) {
            uint8_t p[LLP_PROBE_LEN] = { LLP_OP_PROBE, l->tx_seq };
            l->last_probe = now;
            l->st.probes++;
            (void)emit(l, LLP_T_LINK, 0, p, LLP_PROBE_LEN, now);
        }
    }
    if (elapsed(now, l->last_tx, LLP_KEEPALIVE_MS)) maybe_credit(l, true, now);
}
