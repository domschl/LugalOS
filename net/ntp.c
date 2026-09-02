#include "net/ntp.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/random.h"
#include "drivers/i2c_rtc.h"
#include <string.h>

/* SNTP client. See net/include/net/ntp.h for what this is and is not. */

#define NTP_PORT      123u
#define NTP_PKT_LEN   48u

/* Seconds from 1900-01-01 (NTP's epoch) to 1970-01-01 (everything else's).
 *
 * NTP's 32-bit second count wraps in February 2036, which the protocol calls
 * an era rollover and resolves by requiring both ends to already agree within
 * 68 years. This code assumes era 0 and will read 2036 as 1900. Written down
 * rather than guarded, because the guard is a *policy* -- "assume the era
 * nearest our own clock" -- and this board's clock can be unset, which is
 * exactly the case where that policy has nothing to lean on. Phase 24 owns
 * it if it still matters then. */
#define NTP_EPOCH_OFFSET  2208988800LL

/* The reply is stashed by the callback -- which runs on `netsrv`'s task, not
 * on the caller's -- and picked up by the waiting query below. One query at a
 * time by construction: the port binding is what serialises it, and a second
 * caller cannot bind the same port. */
static struct {
    volatile bool  got;
    uint8_t        pkt[NTP_PKT_LEN];
    uint8_t        from[IPV4_LEN];
    int64_t        t4_us;      /* our clock when the reply arrived */
} g_rx;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Our wall clock as microseconds since 1970 (P2,
 * plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * This used to go through rtc_time_t, which carries milliseconds, so the four
 * timestamps NTP's arithmetic subtracts were each quantised to 1 ms before
 * anything was computed -- against a measured round trip of 6-9 ms on this
 * bench, that is a quantisation comparable to the quantity. The clock keeps
 * microseconds now and this reads them directly. */
static int64_t clock_us(void) {
    return time_epoch_us();
}

/* An NTP 64-bit timestamp (32.32 fixed point) to Unix milliseconds. Zero in
 * both halves is NTP's "this field was never set", which the caller has to
 * distinguish from a real instant -- so it is returned as 0 rather than as
 * the year 1900, and every caller checks. */
static int64_t ntp_to_us(const uint8_t *p) {
    uint32_t sec = be32(p), frac = be32(p + 4);
    if (sec == 0 && frac == 0) return 0;
    return ((int64_t)sec - NTP_EPOCH_OFFSET) * 1000000LL
         + (int64_t)(((uint64_t)frac * 1000000ULL) >> 32);
}

static void us_to_ntp(int64_t us, uint8_t *p) {
    int64_t sec = us / 1000000LL, rem = us % 1000000LL;
    if (rem < 0) { rem += 1000000LL; sec -= 1; }
    uint32_t nsec = (uint32_t)(sec + NTP_EPOCH_OFFSET);
    uint32_t frac = (uint32_t)(((uint64_t)rem << 32) / 1000000ULL);
    p[0] = (uint8_t)(nsec >> 24); p[1] = (uint8_t)(nsec >> 16);
    p[2] = (uint8_t)(nsec >> 8);  p[3] = (uint8_t)nsec;
    p[4] = (uint8_t)(frac >> 24); p[5] = (uint8_t)(frac >> 16);
    p[6] = (uint8_t)(frac >> 8);  p[7] = (uint8_t)frac;
}

static void ntp_recv_cb(void *ctx, const uint8_t src_ip[IPV4_LEN], uint16_t src_port,
                        const uint8_t *data, uint32_t len) {
    (void)ctx;
    /* Timestamped first, before any validation: T4 is "when the reply
     * arrived", and a packet that turns out to be malformed still arrived
     * when it arrived. Doing the checks first would fold their cost into the
     * measurement. */
    int64_t t4 = clock_us();

    if (g_rx.got) return;                      /* already have one; ignore the rest */
    if (src_port != NTP_PORT) return;          /* not from a server's own port */
    if (len < NTP_PKT_LEN) return;

    memcpy(g_rx.pkt, data, NTP_PKT_LEN);
    memcpy(g_rx.from, src_ip, IPV4_LEN);
    g_rx.t4_us = t4;
    g_rx.got = true;
}

const char *ntp_err_str(int rc) {
    switch (rc) {
        case 0:                   return "ok";
        case NTP_ERR_NO_NET:      return "no network address configured";
        case NTP_ERR_BUSY:        return "no free UDP port to ask from";
        case NTP_ERR_SEND:        return "the request could not be sent";
        case NTP_ERR_TIMEOUT:     return "no reply";
        case NTP_ERR_MALFORMED:   return "the reply was not an answer to our question";
        case NTP_ERR_UNSYNCED:    return "the server says its own clock is not set";
        case NTP_ERR_KISS:        return "the server refused us (kiss-o'-death)";
        case NTP_ERR_INTERRUPTED: return "interrupted";
        default:                  return "unknown error";
    }
}

int ntp_query(const uint8_t server[IPV4_LEN], uint32_t timeout_ms, ntp_result_t *out) {
    if (!server || !out) return NTP_ERR_NO_NET;
    if (!net_configured()) return NTP_ERR_NO_NET;
    if (timeout_ms == 0) timeout_ms = 3000u;

    memset(out, 0, sizeof(*out));
    memcpy(out->server, server, IPV4_LEN);

    /* A random source port per query, in the ephemeral range. Not privacy --
     * it is the cheapest half of the two checks that stop an off-path forgery
     * being accepted (the other is the origin timestamp below). An attacker
     * who cannot see our traffic has to guess both. */
    uint16_t sport;
    do {
        uint16_t r;
        random_bytes(&r, sizeof(r));
        sport = (uint16_t)(49152u + (r % 16384u));
    } while (sport == NTP_PORT);

    /* Cleared before the port exists, not after: once bound, a datagram can
     * arrive at any yield point, and wiping the slot afterwards would be a
     * window in which a real reply is thrown away. */
    memset(&g_rx, 0, sizeof(g_rx));
    if (udp_bind(sport, ntp_recv_cb, NULL) != 0) return NTP_ERR_BUSY;

    uint8_t req[NTP_PKT_LEN];
    memset(req, 0, sizeof(req));
    /* LI = 0 (no warning), VN = 4, Mode = 3 (client). Version 4 rather than 3
     * because every server that speaks 3 also answers 4, and a v4 server may
     * legitimately decline v3. */
    req[0] = (0u << 6) | (4u << 3) | 3u;
    req[1] = 0;     /* stratum: unspecified, which is what a client sends */
    req[2] = 6;     /* poll: 2^6 = 64 s, the conventional minimum for a client */
    /* Precision, as a signed power of two seconds. -10 is about a
     * millisecond, which is what kernel/time.c's wall clock actually offers
     * -- claiming the -20 (microseconds) that most clients send would be a
     * lie about this board in a field whose entire purpose is to describe
     * it. A server ignores a client's value; the honesty is the point. */
    req[3] = (uint8_t)(int8_t)-10;

    /* The first datagram to any new peer misses the ARP cache, and this stack
     * does not queue what missed -- net/ip.h says so and says a caller that
     * cares retries. This caller cares: asking a server for the first time
     * since boot is the *normal* case here, not an edge one, and a client
     * that failed on it would look broken exactly when someone is bringing a
     * board up. arp_resolve() has already sent the request by the time
     * udp_send() reports the failure, so the wait below is for the reply to
     * come back, which needs `netsrv` to run.
     *
     * T1 is taken inside the loop, not before it: it means "when this request
     * left", and folding an ARP round trip into it would bias the offset by
     * exactly the time the retry cost. */
    int64_t t1 = 0;
    uint8_t t1_wire[8];
    bool sent = false;
    for (unsigned attempt = 0; attempt < 4u && !sent; attempt++) {
        if (attempt) {
            uint64_t until = time_get_ms() + 150u;
            while (time_get_ms() < until) {
                if (console_interrupt_requested()) { udp_unbind(sport); return NTP_ERR_INTERRUPTED; }
                sched_yield();
            }
        }
        t1 = clock_us();
        us_to_ntp(t1, &req[40]);
        memcpy(t1_wire, &req[40], 8);
        sent = udp_send(server, NTP_PORT, sport, req, sizeof(req)) > 0;
    }
    if (!sent) {
        udp_unbind(sport);
        return NTP_ERR_SEND;
    }

    /* `netsrv` is what will deliver the reply, so waiting means yielding to
     * it -- blocking here would deadlock against the task that has to run.
     * The pacing costs nothing in accuracy: T4 was taken in the callback, on
     * netsrv's own stack, the moment the datagram was parsed. */
    uint64_t deadline = time_get_ms() + timeout_ms;
    while (!g_rx.got) {
        if (time_get_ms() >= deadline) { udp_unbind(sport); return NTP_ERR_TIMEOUT; }
        if (console_interrupt_requested()) { udp_unbind(sport); return NTP_ERR_INTERRUPTED; }
        sched_yield();
    }
    udp_unbind(sport);

    const uint8_t *p = g_rx.pkt;
    uint8_t li   = (uint8_t)(p[0] >> 6);
    uint8_t vn   = (uint8_t)((p[0] >> 3) & 7u);
    uint8_t mode = (uint8_t)(p[0] & 7u);

    /* Mode 4 is "server". Mode 5 is a broadcast, which is a different
     * protocol wearing the same packet and is not an answer to anything. */
    if (mode != 4u || vn < 3u || vn > 4u) return NTP_ERR_MALFORMED;
    if (memcmp(&p[24], t1_wire, 8) != 0)  return NTP_ERR_MALFORMED;

    out->leap    = li;
    out->stratum = p[1];

    if (p[1] == 0) {
        /* Stratum 0 is a kiss-o'-death: the refid is four ASCII characters
         * saying why -- RATE (we asked too often), DENY, RSTR. Reported as
         * text because the text is the entire content of the message. */
        for (unsigned i = 0; i < 4; i++) {
            char c = (char)p[12 + i];
            out->refid[i] = (c >= 32 && c < 127) ? c : '.';
        }
        out->refid[4] = '\0';
        return NTP_ERR_KISS;
    }
    /* LI 3 is "alarm": the server is running but its own clock has never been
     * disciplined. Stratum 16 says the same thing in the other field. Either
     * one means the answer is a number, not a time. */
    if (li == 3u || p[1] >= 16u) return NTP_ERR_UNSYNCED;

    if (p[1] == 1u) {
        for (unsigned i = 0; i < 4; i++) {
            char c = (char)p[12 + i];
            out->refid[i] = (c >= 32 && c < 127) ? c : '\0';
        }
        out->refid[4] = '\0';
    } else {
        /* Stratum 2 and above: the refid is the upstream server's IPv4
         * address, which is what makes a chain visible from here. */
        out->refid_is_ip = true;
        memcpy(out->refip, &p[12], IPV4_LEN);
    }

    int64_t t2 = ntp_to_us(&p[32]);   /* server received our request */
    int64_t t3 = ntp_to_us(&p[40]);   /* server sent this reply */
    if (t3 == 0) return NTP_ERR_MALFORMED;   /* transmit timestamp is mandatory */
    int64_t t4 = g_rx.t4_us;

    /* RFC 4330 §5, and the reason a slow server is not an inaccurate one:
     * the server's own think time (t3 - t2) is subtracted out of the delay
     * and cancels out of the offset entirely. What does *not* cancel is
     * asymmetry -- a path whose two directions take different times biases
     * the offset by half the difference, and nothing in the protocol can see
     * that. Over WiFi that is the dominant term. */
    out->offset_us = ((t2 - t1) + (t3 - t4)) / 2;
    out->delay_us  = (t4 - t1) - (t3 - t2);
    if (out->delay_us < 0) out->delay_us = 0;   /* only reachable if our own clock stepped mid-query */
    return 0;
}

int ntp_sync(const uint8_t server[IPV4_LEN], uint32_t timeout_ms, ntp_result_t *out) {
    ntp_result_t local;
    if (!out) out = &local;

    int rc = ntp_query(server, timeout_ms, out);
    if (rc != 0) return rc;

    /* The offset is a correction to a clock that is still running, so it
     * stays valid however long the arithmetic above took -- reading the clock
     * again here and adding to *that* is what makes this true, rather than
     * carrying t4 forward. */
    time_set_epoch_us(clock_us() + out->offset_us);

    rtc_time_t tm;
    time_get_utc(&tm);
    /* The DS3231 too, exactly as drivers/dcf77_service.c's commit() does: the
     * battery-backed chip is what carries the time across a power cut, and a
     * clock set from the network that reverts on the next boot has only
     * half-worked. A no-op on a board with no RTC. */
    i2c_rtc_write_time(&tm);
    return 0;
}

/* An interval in microseconds, as text, at a scale that suits its size.
 *
 * `long` is 32 bits on RV32 and RP2350, and the first sync of a board that has
 * never been told the time is an offset of *decades* -- 841,582,395,586 ms on
 * the bench, which a `%ld` truncates to -231,194,430 and prints with a
 * confident minus sign. That was found on hardware, printing a wrong number
 * beside a clock it had just set correctly; `kernel/printk.c` supports one `l`
 * and no `%lld`, and teaching it 64-bit varargs to serve one caller is the
 * wrong trade. So the value is split into fields that each fit in 32 bits.
 *
 * The scale follows the magnitude, which is the other half of the job: since
 * P2 the argument carries microseconds, and a sync against a running clock is
 * now worth reading to three decimal places of a millisecond, while a quarter
 * of a century is not worth reading in milliseconds at all. */
static void fmt_interval(int64_t us, char *buf, uint32_t cap) {
    if (cap < 2) { if (cap) buf[0] = '\0'; return; }
    const char *sign = us < 0 ? "-" : "+";
    uint64_t u = (uint64_t)(us < 0 ? -us : us);

    if (u < 10000000ULL) {                       /* under 10 s: milliseconds */
        ksnprintf(buf, cap, "%s%lu.%03u ms",
                  sign, (unsigned long)(u / 1000ULL), (unsigned)(u % 1000ULL));
        return;
    }

    uint64_t secs = u / 1000000ULL;
    uint32_t frac = (uint32_t)((u % 1000000ULL) / 1000ULL);   /* whole ms */
    if (secs < 86400ULL) {
        ksnprintf(buf, cap, "%s%lu.%03u s", sign, (unsigned long)secs, frac);
    } else {
        uint64_t days = secs / 86400ULL;
        uint32_t rem  = (uint32_t)(secs % 86400ULL);
        ksnprintf(buf, cap, "%s%lu d %02u:%02u:%02u.%03u", sign,
                  (unsigned long)days, rem / 3600u, (rem % 3600u) / 60u,
                  rem % 60u, frac);
    }
}

void ntp_print_result(const ntp_result_t *r, bool applied) {
    if (!r) return;
    char refbuf[24];
    if (r->refid_is_ip) {
        cprintf("ntp: %u.%u.%u.%u stratum %u (via %u.%u.%u.%u)\n",
                r->server[0], r->server[1], r->server[2], r->server[3],
                (unsigned)r->stratum,
                r->refip[0], r->refip[1], r->refip[2], r->refip[3]);
    } else {
        unsigned n = 0;
        for (unsigned i = 0; i < 4 && r->refid[i]; i++) refbuf[n++] = r->refid[i];
        refbuf[n] = '\0';
        cprintf("ntp: %u.%u.%u.%u stratum %u (%s)\n",
                r->server[0], r->server[1], r->server[2], r->server[3],
                (unsigned)r->stratum, n ? refbuf : "no refid");
    }

    char ivbuf[48];
    fmt_interval(r->offset_us, ivbuf, sizeof(ivbuf));
    cprintf("  offset     : %s   (what was added to our clock)\n", ivbuf);
    fmt_interval(r->delay_us, ivbuf, sizeof(ivbuf));
    cprintf("  round trip : %s\n", ivbuf + 1);   /* a round trip is never negative */
    if (r->leap == 1u || r->leap == 2u)
        cprintf("  leap       : a leap second is announced for the end of this month\n");

    if (applied) {
        rtc_time_t now;
        char iso[32];
        time_get_utc(&now);
        time_format_iso(&now, iso, sizeof(iso));
        cprintf("  clock set  : %s.%03u UTC\n", iso, (unsigned)now.ms);
    } else {
        cprintf("  clock      : not changed\n");
    }
}
