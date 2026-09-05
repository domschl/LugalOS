#include "net/mqtt.h"
#include "net/tcp.h"
#include "net/ip.h"
#include "kernel/identity.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/palloc.h"
#include <string.h>

/* See net/include/net/mqtt.h for what this speaks and what it deliberately
 * does not. This file is the wire: byte counting, one varint, and a small
 * state machine that is mostly "is the connection still there".
 *
 * Everything here runs over Q0's byte stream, which means two rules that are
 * not obvious from the protocol:
 *
 *   * **A write may be partially accepted, or refused outright**, because the
 *     stream takes one buffer-load at a time and `netsrv` is what drains it.
 *     So every send goes through write_all(), which loops and yields. A send
 *     that spins without yielding waits forever for a drain only it is
 *     preventing.
 *   * **Reads arrive in arbitrary pieces.** A fixed header can be split
 *     across two segments, and a broker is entitled to do that. The parser is
 *     therefore incremental and never assumes a packet arrives whole.
 */

/* --- Packet types (the high nibble of byte 0) --- */
#define MQTT_CONNECT      0x10
#define MQTT_CONNACK      0x20
#define MQTT_PUBLISH      0x30
#define MQTT_PUBACK       0x40
#define MQTT_SUBSCRIBE    0x80
#define MQTT_SUBACK       0x90
#define MQTT_UNSUBSCRIBE  0xA0
#define MQTT_UNSUBACK     0xB0
#define MQTT_PINGREQ      0xC0
#define MQTT_PINGRESP     0xD0
#define MQTT_DISCONNECT   0xE0

#define MQTT_PROTOCOL_LEVEL 4      /* 3.1.1 */

/* CONNECT flag bits. */
#define CF_CLEAN_SESSION  0x02
#define CF_WILL           0x04
#define CF_WILL_RETAIN    0x20
#define CF_PASSWORD       0x40
#define CF_USERNAME       0x80

#define VARINT_MAX 268435455u

/* --- The varint ---
 *
 * Seven bits per byte, high bit as continuation. The only arithmetic in the
 * protocol and the only place a wrong implementation desynchronises a stream
 * silently instead of failing, so it is pure and tested at every boundary. */

uint32_t mqtt_varint_encode(uint32_t value, uint8_t *out) {
    if (!out || value > VARINT_MAX) return 0;
    uint32_t n = 0;
    do {
        uint8_t byte = (uint8_t)(value % 128u);
        value /= 128u;
        if (value) byte |= 0x80u;
        out[n++] = byte;
    } while (value);
    return n;
}

int mqtt_varint_decode(const uint8_t *buf, uint32_t len, uint32_t *value) {
    if (!buf || !value) return -1;
    uint32_t v = 0, mult = 1;
    for (uint32_t i = 0; i < 4u; i++) {
        if (i >= len) return 0;                    /* incomplete; ask again */
        uint8_t byte = buf[i];
        v += (uint32_t)(byte & 0x7fu) * mult;
        if (!(byte & 0x80u)) { *value = v; return (int)(i + 1u); }
        mult *= 128u;
    }
    return -1;                                     /* a fifth continuation byte */
}

/* --- Client state --- */

static struct {
    mqtt_state_t   state;
    tcp_stream_t   stream;
    mqtt_config_t  cfg;
    char           client_id[64];
    char           broker_str[16];
    uint16_t       keepalive_s;

    uint8_t       *rx;             /* MQTT_RX_MAX, from ensure_bufs() */
    uint8_t       *tx;             /* MQTT_TX_MAX, immediately after it */
    uint32_t       rx_len;
    uint32_t       skip;           /* bytes of an oversize packet still to discard */

    uint64_t       last_tx_ms;
    uint64_t       ping_sent_ms;   /* 0 = no ping outstanding */
    bool           connack_seen;
    uint8_t        connack_rc;

    mqtt_counters_t ctr;
    bool           connected_once;
} g;

/* One page, taken the first time anything actually dials a broker and never
 * freed -- net/tcp.c's ensure_bufs() rule, unchanged and for the same reason:
 * a board that never connects to a broker pays nothing, and the chess and
 * clock personas never do. Keeping these as statics cost every persona 1 KB
 * of the RP2350's 512 KB whether or not it had a broker, which `sizecheck`
 * duly refused. */
static int ensure_bufs(void) {
    if (g.rx) return 0;
    uint8_t *page = (uint8_t *)palloc_pages(1);
    if (!page) {
        printk("[MQTT] No memory for the packet buffers.\n");
        return -1;
    }
    g.rx = page;
    g.tx = page + MQTT_RX_MAX;
    return 0;
}

mqtt_state_t mqtt_state(void) { return g.state; }
const mqtt_counters_t *mqtt_counters(void) { return &g.ctr; }
uint8_t mqtt_last_connack(void) { return g.connack_rc; }

const char *mqtt_state_str(void) {
    switch (g.state) {
        case MQTT_CLOSED:     return "CLOSED";
        case MQTT_CONNECTING: return "CONNECTING";
        case MQTT_CONNECTED:  return "CONNECTED";
    }
    return "?";
}

const char *mqtt_err_str(int rc) {
    switch (rc) {
        case 0:                   return "ok";
        case MQTT_ERR_NO_NET:     return "no network address configured";
        case MQTT_ERR_TCP:        return "the broker refused the connection, or is unreachable";
        case MQTT_ERR_TIMEOUT:    return "the broker did not answer in time";
        case MQTT_ERR_REFUSED:    return "the broker rejected the CONNECT";
        case MQTT_ERR_PROTO:      return "the broker sent something that is not MQTT 3.1.1";
        case MQTT_ERR_INTERRUPTED:return "interrupted";
        case MQTT_ERR_TOOBIG:     return "the topic and payload do not fit in one packet";
        case MQTT_ERR_STATE:      return "not connected";
        case MQTT_ERR_BADARG:     return "bad argument";
    }
    return "unknown error";
}

/* The five CONNACK return codes, in words. A broker that says no is the most
 * common thing to debug here, and "rc=4" is not a diagnosis. */
static const char *connack_str(uint8_t rc) {
    switch (rc) {
        case 0: return "accepted";
        case 1: return "unacceptable protocol version";
        case 2: return "client id rejected";
        case 3: return "server unavailable";
        case 4: return "bad username or password";
        case 5: return "not authorized";
    }
    return "unknown return code";
}

/* --- Sending ---
 *
 * write_all() is where the stream's partial-write contract is absorbed, so
 * nothing above it has to think about it. Yields between attempts, checks for
 * Ctrl-C, and gives up on a deadline rather than spinning forever against a
 * peer that has stopped reading. */
static int write_all(const uint8_t *buf, uint32_t len, uint32_t timeout_ms) {
    uint32_t sent = 0;
    uint64_t deadline = time_get_ms() + timeout_ms;
    while (sent < len) {
        int n = tcp_stream_write(&g.stream, buf + sent, len - sent);
        if (n < 0) return MQTT_ERR_TCP;
        if (n > 0) {
            sent += (uint32_t)n;
            continue;
        }
        if (console_interrupt_requested()) return MQTT_ERR_INTERRUPTED;
        if (time_get_ms() >= deadline) return MQTT_ERR_TIMEOUT;
        sched_yield();
    }
    g.last_tx_ms = time_get_ms();
    return 0;
}

/* A length-prefixed UTF-8 field, which is how MQTT spells every string. */
static uint32_t put_str(uint8_t *out, uint32_t off, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) : 0;
    out[off++] = (uint8_t)(n >> 8);
    out[off++] = (uint8_t)n;
    if (n) memcpy(out + off, s, n);
    return off + n;
}

static uint32_t str_field_len(const char *s) {
    return 2u + (s ? (uint32_t)strlen(s) : 0u);
}

static bool have(const char *s) { return s && s[0]; }

/* Builds a packet's fixed header ahead of a body already at
 * g.tx[5..], and returns the offset the complete packet starts at.
 *
 * Building the body first and the header second is what avoids a second
 * buffer: the header's own length depends on the body's length through the
 * varint, so five bytes are reserved and the header is written to end exactly
 * where the body begins. */
static uint32_t frame_packet(uint8_t type_and_flags, uint32_t body_len, uint32_t *out_len) {
    uint8_t vi[4];
    uint32_t vn = mqtt_varint_encode(body_len, vi);
    uint32_t start = 5u - (1u + vn);
    g.tx[start] = type_and_flags;
    memcpy(g.tx + start + 1u, vi, vn);
    *out_len = 1u + vn + body_len;
    return start;
}

/* --- Receiving --- */

static void handle_publish(const uint8_t *pkt, uint32_t len, uint8_t flags);

/* One complete packet, header included. */
static int dispatch(const uint8_t *pkt, uint32_t hdr_len, uint32_t body_len) {
    uint8_t type = pkt[0] & 0xf0u;
    const uint8_t *body = pkt + hdr_len;

    switch (type) {
        case MQTT_CONNACK:
            if (body_len < 2u) return MQTT_ERR_PROTO;
            g.connack_rc = body[1];
            g.connack_seen = true;
            return 0;
        case MQTT_PINGRESP:
            g.ping_sent_ms = 0;
            return 0;
        case MQTT_PUBLISH:
            handle_publish(body, body_len, pkt[0] & 0x0fu);
            g.ctr.received++;
            return 0;
        case MQTT_SUBACK:
        case MQTT_UNSUBACK:
        case MQTT_PUBACK:
            /* Q3/Q8 give these meaning. Accepted and ignored until then,
             * which is better than closing a healthy connection over a packet
             * we asked for and have not yet learned to read. */
            return 0;
        default:
            /* Anything else is a broker speaking a protocol we did not agree
             * to -- including the QoS 2 handshake, which we never initiate. */
            return MQTT_ERR_PROTO;
    }
}

/* Q1 has no subscriptions, so nothing can arrive here yet; Q3 gives this a
 * callback. Kept as its own function so Q3 is a body, not a restructure. */
static void handle_publish(const uint8_t *pkt, uint32_t len, uint8_t flags) {
    (void)pkt; (void)len; (void)flags;
}

/* Feeds one chunk of freshly-arrived bytes through the incremental parser.
 * Returns 0, or MQTT_ERR_PROTO when the framing itself is wrong -- which
 * closes the connection, because there is no resynchronising a stream whose
 * framing is broken and pretending otherwise is how a parser starts on
 * whatever follows. */
static int feed(const uint8_t *data, uint32_t len) {
    while (len) {
        /* Draining an oversize packet we already decided not to keep. */
        if (g.skip) {
            uint32_t n = (g.skip < len) ? g.skip : len;
            g.skip -= n;
            data += n;
            len -= n;
            continue;
        }

        uint32_t room = MQTT_RX_MAX - g.rx_len;
        uint32_t n = (len < room) ? len : room;
        memcpy(g.rx + g.rx_len, data, n);
        g.rx_len += n;
        data += n;
        len -= n;

        /* Consume as many complete packets as the buffer now holds. */
        for (;;) {
            if (g.rx_len < 2u) break;
            uint32_t body_len = 0;
            int vn = mqtt_varint_decode(g.rx + 1u, g.rx_len - 1u, &body_len);
            if (vn < 0) return MQTT_ERR_PROTO;
            if (vn == 0) break;                       /* varint not complete yet */
            uint32_t hdr_len = 1u + (uint32_t)vn;
            uint32_t total = hdr_len + body_len;

            if (total > MQTT_RX_MAX) {
                /* §3.5: too big to hold, so its bytes are *counted and
                 * discarded* rather than dropped. Dropping without consuming
                 * would leave the stream desynchronised at a packet boundary
                 * forever. */
                uint32_t have_body = g.rx_len - hdr_len;
                g.skip = body_len - ((have_body < body_len) ? have_body : body_len);
                g.rx_len = 0;
                g.ctr.oversize++;
                break;
            }
            if (g.rx_len < total) break;              /* wait for the rest */

            int rc = dispatch(g.rx, hdr_len, body_len);
            g.rx_len -= total;
            if (g.rx_len) memmove(g.rx, g.rx + total, g.rx_len);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

static void drop_connection(void) {
    tcp_stream_close(&g.stream);
    g.state = MQTT_CLOSED;
    g.rx_len = 0;
    g.skip = 0;
    g.ping_sent_ms = 0;
}

void mqtt_service(void) {
    if (g.state == MQTT_CLOSED) return;

    if (tcp_stream_ready(&g.stream) < 0) {
        drop_connection();
        return;
    }

    uint8_t buf[256];
    for (;;) {
        int n = tcp_stream_read(&g.stream, buf, sizeof(buf));
        if (n < 0) { drop_connection(); return; }
        if (n == 0) break;
        int rc = feed(buf, (uint32_t)n);
        if (rc != 0) {
            g.ctr.proto_errors++;
            printk("[MQTT] %s -- closing the connection.\n", mqtt_err_str(rc));
            drop_connection();
            return;
        }
    }

    /* A broker that closed on us. Reported here rather than waited for: the
     * owner's next publish would otherwise be the thing that discovers it. */
    if (tcp_stream_peer_closed(&g.stream)) {
        drop_connection();
        return;
    }

    if (g.state != MQTT_CONNECTED) return;

    uint64_t now = time_get_ms();
    uint32_t ka_ms = (uint32_t)g.keepalive_s * 1000u;

    /* An unanswered ping means the connection is gone whatever TCP thinks --
     * a half-open connection through a router that has forgotten us looks
     * exactly like a quiet one, and only the missing PINGRESP tells them
     * apart. */
    if (g.ping_sent_ms && (now - g.ping_sent_ms) > ka_ms) {
        printk("[MQTT] No PINGRESP within the keepalive -- the connection is dead.\n");
        drop_connection();
        return;
    }

    /* Three quarters of the interval, so a slow round trip does not race the
     * broker's own 1.5x timeout. */
    if (!g.ping_sent_ms && (now - g.last_tx_ms) >= (ka_ms - ka_ms / 4u)) {
        uint8_t ping[2] = { MQTT_PINGREQ, 0x00 };
        if (write_all(ping, sizeof(ping), 2000u) == 0) {
            g.ping_sent_ms = now;
            g.ctr.pings++;
        }
    }
}

int mqtt_connect(const mqtt_config_t *cfg, uint32_t timeout_ms) {
    if (!cfg) return MQTT_ERR_BADARG;
    if (!net_configured()) return MQTT_ERR_NO_NET;

    if (g.state != MQTT_CLOSED) mqtt_disconnect();
    if (ensure_bufs() != 0) return MQTT_ERR_NO_NET;

    g.cfg = *cfg;
    g.keepalive_s = cfg->keepalive_s ? cfg->keepalive_s : (uint16_t)MQTT_DEFAULT_KEEPALIVE;
    uint16_t port = cfg->port ? cfg->port : (uint16_t)MQTT_DEFAULT_PORT;

    /* The client id defaults to the node's own name, which the identity
     * record already makes per-board -- see the header on why sharing one is
     * a disconnect loop rather than a merge. Copied rather than pointed at:
     * the caller's config may be a stack temporary. */
    const char *id = have(cfg->client_id) ? cfg->client_id : node_name();
    strncpy(g.client_id, id ? id : "lugalos", sizeof(g.client_id) - 1u);
    g.client_id[sizeof(g.client_id) - 1u] = '\0';
    ksnprintf(g.broker_str, sizeof(g.broker_str), "%u.%u.%u.%u",
              cfg->broker[0], cfg->broker[1], cfg->broker[2], cfg->broker[3]);

    /* 3.1.1: a password without a username is a protocol error, and a broker
     * answers it with a reset or a CONNACK nobody can interpret. Refused here
     * with a reason instead. */
    if (have(cfg->password) && !have(cfg->username)) return MQTT_ERR_BADARG;

    g.rx_len = 0;
    g.skip = 0;
    g.connack_seen = false;
    g.connack_rc = 0;
    g.ping_sent_ms = 0;

    if (tcp_stream_open(&g.stream, cfg->broker, port) != 0) return MQTT_ERR_TCP;
    g.state = MQTT_CONNECTING;

    uint64_t deadline = time_get_ms() + (timeout_ms ? timeout_ms : 5000u);
    int ready;
    while ((ready = tcp_stream_ready(&g.stream)) == 0) {
        if (console_interrupt_requested()) { drop_connection(); return MQTT_ERR_INTERRUPTED; }
        if (time_get_ms() >= deadline) { drop_connection(); return MQTT_ERR_TIMEOUT; }
        sched_yield();
    }
    if (ready < 0) { drop_connection(); return MQTT_ERR_TCP; }

    /* --- CONNECT --- */
    uint8_t flags = CF_CLEAN_SESSION;
    if (have(cfg->will_topic)) {
        flags |= CF_WILL;                       /* QoS 0, so no will-QoS bits */
        if (cfg->will_retain) flags |= CF_WILL_RETAIN;
    }
    if (have(cfg->username)) flags |= CF_USERNAME;
    if (have(cfg->password)) flags |= CF_PASSWORD;

    uint32_t body = 10u + str_field_len(g.client_id);
    if (flags & CF_WILL) body += str_field_len(cfg->will_topic) + str_field_len(cfg->will_payload);
    if (flags & CF_USERNAME) body += str_field_len(cfg->username);
    if (flags & CF_PASSWORD) body += str_field_len(cfg->password);
    if (5u + body > MQTT_TX_MAX) { drop_connection(); return MQTT_ERR_TOOBIG; }

    uint32_t o = 5u;
    o = put_str(g.tx, o, "MQTT");
    g.tx[o++] = MQTT_PROTOCOL_LEVEL;
    g.tx[o++] = flags;
    g.tx[o++] = (uint8_t)(g.keepalive_s >> 8);
    g.tx[o++] = (uint8_t)g.keepalive_s;
    o = put_str(g.tx, o, g.client_id);
    if (flags & CF_WILL) {
        o = put_str(g.tx, o, cfg->will_topic);
        o = put_str(g.tx, o, cfg->will_payload);
    }
    if (flags & CF_USERNAME) o = put_str(g.tx, o, cfg->username);
    if (flags & CF_PASSWORD) o = put_str(g.tx, o, cfg->password);

    uint32_t pkt_len = 0;
    uint32_t start = frame_packet(MQTT_CONNECT, o - 5u, &pkt_len);
    int rc = write_all(g.tx + start, pkt_len, 5000u);
    if (rc != 0) { drop_connection(); return rc; }

    /* --- CONNACK --- */
    while (!g.connack_seen) {
        if (console_interrupt_requested()) { drop_connection(); return MQTT_ERR_INTERRUPTED; }
        if (time_get_ms() >= deadline) { drop_connection(); return MQTT_ERR_TIMEOUT; }
        mqtt_service();
        if (g.state == MQTT_CLOSED) return MQTT_ERR_TCP;
        sched_yield();
    }
    if (g.connack_rc != 0) {
        printk("[MQTT] The broker refused the connection: %s (%u).\n",
               connack_str(g.connack_rc), g.connack_rc);
        drop_connection();
        return MQTT_ERR_REFUSED;
    }

    g.state = MQTT_CONNECTED;
    g.ctr.connected_at_ms = time_get_ms();
    if (g.connected_once) g.ctr.reconnects++;
    g.connected_once = true;
    return 0;
}

int mqtt_publish(const char *topic, const void *payload, uint32_t len, bool retain) {
    if (!topic || !topic[0]) return MQTT_ERR_BADARG;
    if (g.state != MQTT_CONNECTED) return MQTT_ERR_STATE;
    if (strlen(topic) > MQTT_TOPIC_MAX) return MQTT_ERR_TOOBIG;

    uint32_t body = str_field_len(topic) + len;   /* QoS 0: no packet id */
    if (5u + body > MQTT_TX_MAX) return MQTT_ERR_TOOBIG;

    uint32_t o = put_str(g.tx, 5u, topic);
    if (len) memcpy(g.tx + o, payload, len);
    o += len;

    uint32_t pkt_len = 0;
    uint32_t start = frame_packet((uint8_t)(MQTT_PUBLISH | (retain ? 0x01u : 0x00u)),
                                  o - 5u, &pkt_len);
    int rc = write_all(g.tx + start, pkt_len, 5000u);
    if (rc != 0) {
        if (rc == MQTT_ERR_TCP) drop_connection();
        return rc;
    }
    g.ctr.published++;
    return 0;
}

void mqtt_disconnect(void) {
    if (g.state == MQTT_CONNECTED) {
        /* A clean DISCONNECT tells the broker not to publish the will: the
         * node meant to go. That is the whole difference between this and
         * pulling the power, and it is what makes the will meaningful. */
        uint8_t pkt[2] = { MQTT_DISCONNECT, 0x00 };
        write_all(pkt, sizeof(pkt), 1000u);
    }
    drop_connection();
}

void mqtt_print_status(void) {
    if (g.state == MQTT_CLOSED && !g.connected_once) {
        cprintf("mqtt: not connected. `mqtt connect <ip>[:port]` to dial one.\n");
        return;
    }
    cprintf("mqtt: broker %s:%u, client-id \"%s\"\n",
            g.broker_str, g.cfg.port ? g.cfg.port : (uint16_t)MQTT_DEFAULT_PORT,
            g.client_id);
    if (g.state == MQTT_CONNECTED) {
        uint64_t up = (time_get_ms() - g.ctr.connected_at_ms) / 1000u;
        cprintf("      state %s (up %lum%lus), keepalive %us\n",
                mqtt_state_str(), (unsigned long)(up / 60u), (unsigned long)(up % 60u),
                g.keepalive_s);
    } else {
        cprintf("      state %s\n", mqtt_state_str());
        if (g.connack_rc) cprintf("      last refusal: %s (%u)\n",
                                  connack_str(g.connack_rc), g.connack_rc);
    }
    cprintf("      published %lu, received %lu, pings %lu, reconnects %lu\n",
            (unsigned long)g.ctr.published, (unsigned long)g.ctr.received,
            (unsigned long)g.ctr.pings, (unsigned long)g.ctr.reconnects);
    if (g.ctr.oversize || g.ctr.proto_errors)
        cprintf("      oversize dropped %lu, protocol errors %lu\n",
                (unsigned long)g.ctr.oversize, (unsigned long)g.ctr.proto_errors);
    if (have(g.cfg.username))
        cprintf("      authenticating as \"%s\"%s -- in the clear, on this LAN\n",
                g.cfg.username, have(g.cfg.password) ? " with a password" : "");
}

/* --- The boundary vector ---
 *
 * Needs no network, no broker and no hardware, so it runs in CI on both QEMU
 * targets. The values are the varint's own boundaries plus the two malformed
 * shapes, which is the whole of what can go wrong in it. */
uint32_t mqtt_selftest(bool report) {
    static const uint32_t cases[] = {
        0u, 1u, 127u, 128u, 16383u, 16384u, 2097151u, 2097152u, VARINT_MAX
    };
    static const uint32_t widths[] = { 1u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u };
    uint32_t failed = 0;

    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[4];
        uint32_t n = mqtt_varint_encode(cases[i], buf);
        uint32_t back = 0;
        int m = mqtt_varint_decode(buf, n, &back);
        bool ok = (n == widths[i]) && (m == (int)n) && (back == cases[i]);
        if (!ok) {
            failed++;
            if (report) cprintf("  varint %lu: encoded %lu bytes (want %lu), "
                                "decoded %d -> %lu\n",
                                (unsigned long)cases[i], (unsigned long)n,
                                (unsigned long)widths[i], m, (unsigned long)back);
        }
    }

    /* Above the maximum: refused rather than truncated to four bytes. */
    uint8_t buf[4];
    if (mqtt_varint_encode(VARINT_MAX + 1u, buf) != 0) {
        failed++;
        if (report) cprintf("  varint: encoded a value above the protocol maximum\n");
    }

    /* A fifth continuation byte is malformed, not merely long. */
    static const uint8_t bad[] = { 0xff, 0xff, 0xff, 0xff, 0x7f };
    uint32_t v = 0;
    if (mqtt_varint_decode(bad, sizeof(bad), &v) != -1) {
        failed++;
        if (report) cprintf("  varint: accepted a fifth continuation byte\n");
    }

    /* A truncated varint is "ask again", not an error -- the case a split
     * fixed header presents, and the one an eager parser gets wrong. */
    static const uint8_t partial[] = { 0x80 };
    if (mqtt_varint_decode(partial, sizeof(partial), &v) != 0) {
        failed++;
        if (report) cprintf("  varint: a truncated varint must report incomplete\n");
    }

    if (report) cprintf("mqtt selftest: %lu case%s failed\n",
                        (unsigned long)failed, failed == 1u ? "" : "s");
    return failed;
}
