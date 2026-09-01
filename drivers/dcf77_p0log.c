/* See drivers/include/drivers/dcf77_p0log.h for what this measures and why
 * it must never set a clock. */

#include "drivers/dcf77_p0log.h"
#include "drivers/dcf77_service.h"
#include "net/ntp.h"
#include "net/ip.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

#ifndef CONFIG_DCF77_P0_NTP
#define CONFIG_DCF77_P0_NTP "192.168.178.23"
#endif
#ifndef CONFIG_DCF77_P0_PERIOD_S
#define CONFIG_DCF77_P0_PERIOD_S 60
#endif
#ifndef CONFIG_DCF77_P0_PORT
#define CONFIG_DCF77_P0_PORT 5959
#endif

/* Twelve is what fits. The report has to land inside a VFS handle's 896-byte
 * proc_buf (fs/vfs_server.c), the header takes about 300 of it, and a sample
 * line is about 45 -- so this is the buffer's size expressed in samples
 * rather than a number anyone chose. A collector polling every five minutes
 * against a one-a-minute cadence has more than twice the headroom it needs,
 * and one that falls behind loses raw detail only: the accumulators below are
 * never rolled off. */
#define P0_RING 12

typedef struct {
    uint32_t seq;
    uint64_t mono_ms;        /* the free-running clock, which is the x axis */
    int32_t  ntp_off_ms;     /* what the reference says our clock is out by */
    uint16_t ntp_rtt_ms;
    uint8_t  ntp_stratum;
    bool     have_dcf;
    int32_t  dcf_err_ms;     /* the radio's claim minus the reference's truth */
    uint8_t  quality;        /* the decoder's mean score, tenths */
    /* The decoder's longest unbroken run of aligned seconds, capped at 255.
     *
     * It answers exactly one question -- "is reception good enough for a
     * frame to form at all", where the bar is 59 consecutive seconds -- and
     * it answers it well: on the first good run it climbed 45, 104, 163, 222,
     * one whole frame's worth per minute with not a bad second between. Then
     * it pins at 255 and says nothing further, because the underlying counter
     * is a running maximum that never resets.
     *
     * Left as it is on purpose. The field has done its job by the time it
     * saturates -- from then on the dcf error column is the signal -- and
     * changing the wire format means fetching a board back from wherever its
     * reception is, which is a poor trade for a bring-up indicator. **When
     * P1 reflashes this board anyway, make it frames_accepted instead**: a
     * count that stays meaningful for the whole run. */
    uint8_t  clean_run;
} p0_sample_t;

static struct {
    bool     started;
    bool     stepped_once;   /* the single deliberate ntp_sync() at startup */
    uint32_t seq;
    uint64_t t0_ms;          /* when the run began, so x stays small */

    p0_sample_t ring[P0_RING];
    uint32_t    n_ring;      /* how many slots are filled, up to P0_RING */

    /* --- accumulators, never rolled off --- */
    /* The DCF error: count, sum and sum of squares give mean and standard
     * deviation without keeping the samples. Milliseconds squared, summed
     * over a day of once-a-minute samples, is comfortably inside int64. */
    uint32_t dcf_n;
    int64_t  dcf_sum, dcf_sumsq;
    int32_t  dcf_min, dcf_max;

    /* The crystal: a least-squares fit of the reference's reported offset
     * against elapsed local seconds. The slope is the fractional frequency
     * error, and 1e6 times it is ppm. Accumulated in int64 because sum_xx
     * over a day reaches 1e13 -- which is also why none of these is ever
     * printed with %ld (see net/ntp.c's fmt_interval note; `long` is 32 bits
     * here). */
    uint32_t ppm_n;
    int64_t  sum_x, sum_y, sum_xx, sum_xy;

    uint32_t ntp_fail;
    int      last_rc;

    /* Bring-up diagnostics. The first version of this had none, and when it
     * sat at samples=0 on a board across the room the log could not say
     * whether the network was down, the reference unreachable, or the task
     * never scheduled -- which is the "a single dropped total cannot become a
     * diagnosis" lesson (net/ip.h) arriving in a new place. An instrument
     * that cannot explain its own silence is not finished. */
    uint32_t init_tries;
    int      init_rc;
    uint32_t loops;
    bool     saw_net;
} g;

static uint8_t g_server[IPV4_LEN];
static bool    g_server_ok;

static void p0_sleep_ms(uint32_t ms) {
    uint64_t end = time_get_ms() + ms;
    while (time_get_ms() < end) sched_yield();
}

/* Our wall clock as epoch milliseconds. */
static int64_t wall_ms(void) {
    rtc_time_t tm;
    time_get_utc(&tm);
    return time_to_epoch(&tm) * 1000LL + (int64_t)tm.ms;
}

/* One sample, broadcast to the segment as text.
 *
 * The board can also be *read* at /proc/dcf77log, and on the bench that is
 * the better channel -- but 9P over TCP requires authentication (phase 18's
 * gate, by design) and this board has no persistent device key, while its
 * console is owned by the clock application. So a board carried to wherever
 * the reception is has no inbound channel at all, and the only way to hear
 * from it is to let it speak.
 *
 * Broadcast rather than unicast to a collector: the address of whoever is
 * listening is one more thing that would have to survive being carried across
 * a house and power-cycled, and net/ipv4.c already sends a 255.255.255.255
 * destination to the broadcast MAC without consulting ARP -- so this needs no
 * configuration and no resolution. Fire and forget: a lost datagram is a
 * missing sample, never a wrong one, and the accumulators in the report are
 * never rolled off, so the two numbers P0 exists for survive any amount of
 * loss. */
static void broadcast_sample(const p0_sample_t *s) {
    static const uint8_t all[IPV4_LEN] = { 255, 255, 255, 255 };
    char line[128];
    int n;
    if (s->have_dcf) {
        n = ksnprintf(line, sizeof(line), "p0 %s %lu %lu %ld %u %u %ld %u %u\n",
                      CONFIG_NODE_PERSONA,
                      (unsigned long)s->seq,
                      (unsigned long)((s->mono_ms - g.t0_ms) / 1000u),
                      (long)s->ntp_off_ms, s->ntp_rtt_ms, s->ntp_stratum,
                      (long)s->dcf_err_ms, s->quality, s->clean_run);
    } else {
        n = ksnprintf(line, sizeof(line), "p0 %s %lu %lu %ld %u %u - %u %u\n",
                      CONFIG_NODE_PERSONA,
                      (unsigned long)s->seq,
                      (unsigned long)((s->mono_ms - g.t0_ms) / 1000u),
                      (long)s->ntp_off_ms, s->ntp_rtt_ms, s->ntp_stratum,
                      s->quality, s->clean_run);
    }
    if (n > 0) {
        udp_send(all, CONFIG_DCF77_P0_PORT, CONFIG_DCF77_P0_PORT,
                 (const uint8_t *)line, (uint32_t)n);
    }
}

static void accumulate(const p0_sample_t *s) {
    if (g.n_ring < P0_RING) {
        g.ring[g.n_ring++] = *s;
    } else {
        memmove(&g.ring[0], &g.ring[1], sizeof(g.ring[0]) * (P0_RING - 1));
        g.ring[P0_RING - 1] = *s;
    }

    /* x in seconds since the run began, not since boot: a smaller x keeps
     * sum_xx an order of magnitude away from anything interesting, and the
     * slope is unaffected by where the origin sits. */
    int64_t x = (int64_t)((s->mono_ms - g.t0_ms) / 1000u);
    int64_t y = (int64_t)s->ntp_off_ms;
    g.ppm_n++;
    g.sum_x += x; g.sum_y += y; g.sum_xx += x * x; g.sum_xy += x * y;

    if (s->have_dcf) {
        int64_t e = (int64_t)s->dcf_err_ms;
        if (g.dcf_n == 0) { g.dcf_min = s->dcf_err_ms; g.dcf_max = s->dcf_err_ms; }
        if (s->dcf_err_ms < g.dcf_min) g.dcf_min = s->dcf_err_ms;
        if (s->dcf_err_ms > g.dcf_max) g.dcf_max = s->dcf_err_ms;
        g.dcf_n++;
        g.dcf_sum += e;
        g.dcf_sumsq += e * e;
    }
}

static void p0_body(void *arg) {
    (void)arg;

    /* TASK_PRIO_NORMAL, and the alternative was tried first and measured.
     *
     * TASK_PRIO_BACKGROUND is what drivers/cyw43_rp2350.c's supervisor drops
     * to once it is only watching, and copying that here looked obviously
     * right: this task does one small query a minute and otherwise measures
     * wall time in a yield loop, which is the same shape. On this persona it
     * got **zero** slices. The task ran as far as its first printk() -- which
     * can yield, because kernel/printk.c blocks on UART backpressure -- and
     * was never scheduled again: the startup line appeared in /proc/kmsg at
     * t=0.63 s while the loop counter was still 0 seventy seconds later.
     *
     * That is phase 19's R5 lesson arriving from the other side. R5 recorded
     * that starting `wifiup` at BACKGROUND was wrong because "the clock app is
     * a continuous loop, so a strictly lower task gets only the slices it
     * leaves and the 231 KB upload never finished at all". The slices it
     * leaves are not few here; they are none. wifiup gets away with dropping
     * to BACKGROUND *after* joining only because nothing then depends on it
     * running promptly.
     *
     * So: NORMAL, and the cost is one more yield-spin sharing the CPU
     * round-robin. That is a real cost and this is a real instrument -- while
     * CONFIG_DCF77_P0_LOG is 1 the board is a measuring device rather than an
     * appliance, and a little more display flicker is the price of the
     * measurement. It goes back to 0 when P0 concludes. */

    if (!ipv4_parse(CONFIG_DCF77_P0_NTP, g_server)) {
        printk("[P0] CONFIG_DCF77_P0_NTP (\"%s\") is not a dotted quad; not logging\n",
               CONFIG_DCF77_P0_NTP);
        return;
    }
    g_server_ok = true;

    /* The clock must stop being written before the first sample is taken. */
    dcf77_service_set_auto(false);
    printk("[P0] measuring against %s; DCF auto-sync off, clock frozen after "
           "one initial step\n", CONFIG_DCF77_P0_NTP);

    uint64_t last_mark_ms = 0;
    bool     have_last_mark = false;

    for (;;) {
        ntp_result_t r;

        g.loops++;
        if (net_configured()) g.saw_net = true;

        if (!g.stepped_once) {
            /* Once, and only once. Everything after this reads the clock and
             * never moves it. Retried until the network is actually up --
             * this task starts long before the radio has joined. */
            g.init_tries++;
            int irc = ntp_sync(g_server, 3000u, &r);
            g.init_rc = irc;
            if (irc == 0) {
                g.stepped_once = true;
                g.t0_ms = time_get_ms();
                printk("[P0] initial step applied; the clock is frozen from here\n");
            } else {
                /* The first few only: a board waiting out a radio join must
                 * not fill /proc/kmsg with one line per retry, but a board
                 * that will never succeed has to say so somewhere a human
                 * reads. */
                if (g.init_tries <= 3u || (g.init_tries % 60u) == 0u) {
                    printk("[P0] initial sync %lu failed: %s (net %s)\n",
                           (unsigned long)g.init_tries, ntp_err_str(irc),
                           net_configured() ? "configured" : "unconfigured");
                }
                p0_sleep_ms(5000);
                continue;
            }
        }

        int rc = ntp_query(g_server, 3000u, &r);
        if (rc != 0) {
            g.ntp_fail++; g.last_rc = rc;
            p0_sleep_ms((uint32_t)CONFIG_DCF77_P0_PERIOD_S * 1000u);
            continue;
        }
        g.last_rc = 0;

        uint64_t now_ms = time_get_ms();
        p0_sample_t s;
        memset(&s, 0, sizeof(s));
        s.seq = ++g.seq;
        s.mono_ms = now_ms;
        /* The offset fits in 32 bits because the startup step put the clock
         * near the truth and nothing has moved it since; clamped rather than
         * wrapped anyway, because a silently plausible wrong number is the
         * failure mode this phase has already met once. */
        int64_t off = r.offset_ms;
        if (off >  2147483647LL) off =  2147483647LL;
        if (off < -2147483648LL) off = -2147483648LL;
        s.ntp_off_ms  = (int32_t)off;
        s.ntp_rtt_ms  = (uint16_t)(r.delay_ms > 65535 ? 65535 : r.delay_ms);
        s.ntp_stratum = r.stratum;

        dcf_status_t d;
        dcf77_service_status(&d);
        s.quality = (uint8_t)(d.decoder.quality_total
            ? (d.decoder.quality_sum * 10u / d.decoder.quality_total) : 0u);
        s.clean_run = (uint8_t)(d.decoder.clean_run_max > 255u ? 255u
                                                              : d.decoder.clean_run_max);

        /* A frame counts once. dcf77_service.c keeps the most recent accepted
         * one indefinitely, so without this every poll would re-log the same
         * frame and the standard deviation would be computed over copies. */
        if (d.have_radio_time && (!have_last_mark || d.radio_at_ms != last_mark_ms)) {
            last_mark_ms = d.radio_at_ms;
            have_last_mark = true;

            /* What our clock read at the instant of the radio's mark, carried
             * back with the monotonic counter -- valid precisely because
             * nothing steps the wall clock during a run. */
            int64_t wall_at_mark = wall_ms() - (int64_t)(now_ms - d.radio_at_ms);
            int64_t truth_at_mark = wall_at_mark + (int64_t)s.ntp_off_ms;
            int64_t claimed = time_to_epoch(&d.radio_utc) * 1000LL + (int64_t)d.radio_utc.ms;

            int64_t err = claimed - truth_at_mark;
            if (err >  2147483647LL) err =  2147483647LL;
            if (err < -2147483648LL) err = -2147483648LL;
            s.have_dcf = true;
            s.dcf_err_ms = (int32_t)err;
        }

        accumulate(&s);
        broadcast_sample(&s);
        p0_sleep_ms((uint32_t)CONFIG_DCF77_P0_PERIOD_S * 1000u);
    }
}

int dcf77_p0log_start(void) {
    if (g.started) return -1;
    memset(&g, 0, sizeof(g));
    g.started = true;
    g.t0_ms = time_get_ms();
    /* Three pages, matching `netsrv` and `wifiup`: ntp_query() puts a 48-byte
     * packet and a result struct on this stack, but idstore_read() behind
     * node_* calls does not run here, so this is headroom rather than need. */
    int pid = task_create_sized("p0log", p0_body, NULL, 3);
    if (pid < 0) printk("[P0] could not start the measurement task\n");
    return pid;
}

bool dcf77_p0log_active(void) { return g.started; }

/* Integer square root, for the standard deviation. Newton's method on
 * integers: no floating point exists in this kernel, and the answer is
 * wanted in whole milliseconds anyway. */
static uint32_t isqrt64(uint64_t v) {
    if (v == 0) return 0;
    uint64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (uint32_t)x;
}

uint32_t dcf77_p0log_render(char *buf, uint32_t cap) {
    if (!buf || cap == 0) return 0;
    uint32_t u = 0;

    u += (uint32_t)ksnprintf(buf + u, cap - u,
        "server=%s\nactive=%s\nstepped=%s\nsamples=%lu\nntp_fail=%lu\nlast_rc=%d\n"
        "loops=%lu\nnet=%s\ninit_tries=%lu\ninit_rc=%d\ninit_why=%s\nudp_port=%u\n",
        CONFIG_DCF77_P0_NTP,
        g.started ? "yes" : "no",
        g.stepped_once ? "yes" : "no",
        (unsigned long)g.seq, (unsigned long)g.ntp_fail, g.last_rc,
        (unsigned long)g.loops,
        net_configured() ? "configured" : "unconfigured",
        (unsigned long)g.init_tries, g.init_rc,
        g.stepped_once ? "done" : ntp_err_str(g.init_rc),
        (unsigned)CONFIG_DCF77_P0_PORT);

    uint64_t span_s = (g.seq && time_get_ms() > g.t0_ms)
                        ? (time_get_ms() - g.t0_ms) / 1000u : 0;
    u += (uint32_t)ksnprintf(buf + u, cap - u, "span_s=%lu\n", (unsigned long)span_s);

    /* ppm = 1e6 * slope, slope = (n*Sxy - Sx*Sy) / (n*Sxx - Sx*Sx), with y in
     * milliseconds and x in seconds -- so the raw slope is ms/s, and ppm is
     * 1000x it. Reported in tenths, because a board is not going to be
     * characterised to better than 0.1 ppm by an hour of samples and printing
     * more digits than that would be a claim rather than a measurement. */
    if (g.ppm_n >= 3) {
        int64_t n = (int64_t)g.ppm_n;
        int64_t den = n * g.sum_xx - g.sum_x * g.sum_x;
        int64_t num = n * g.sum_xy - g.sum_x * g.sum_y;
        if (den != 0) {
            int64_t ppm_tenths = (num * 10000LL) / den;   /* (ms/s)*1000*10 */
            long whole = (long)(ppm_tenths / 10);
            long frac  = (long)(ppm_tenths % 10); if (frac < 0) frac = -frac;
            u += (uint32_t)ksnprintf(buf + u, cap - u,
                "ppm=%ld.%ld\nppm_n=%lu\n", whole, frac, (unsigned long)g.ppm_n);
        }
    } else {
        u += (uint32_t)ksnprintf(buf + u, cap - u, "ppm=pending\nppm_n=%lu\n",
                                 (unsigned long)g.ppm_n);
    }

    if (g.dcf_n) {
        int64_t mean = g.dcf_sum / (int64_t)g.dcf_n;
        /* Population variance from the running sums. Exact in integers up to
         * the point where the mean is truncated, which costs well under a
         * millisecond on a quantity measured in tens. */
        int64_t var = g.dcf_sumsq / (int64_t)g.dcf_n - mean * mean;
        if (var < 0) var = 0;
        u += (uint32_t)ksnprintf(buf + u, cap - u,
            "dcf_n=%lu\ndcf_mean_ms=%ld\ndcf_sd_ms=%lu\ndcf_min_ms=%ld\ndcf_max_ms=%ld\n",
            (unsigned long)g.dcf_n, (long)mean,
            (unsigned long)isqrt64((uint64_t)var),
            (long)g.dcf_min, (long)g.dcf_max);
    } else {
        u += (uint32_t)ksnprintf(buf + u, cap - u, "dcf_n=0\n");
    }

    u += (uint32_t)ksnprintf(buf + u, cap - u, "--\n");
    for (uint32_t i = 0; i < g.n_ring && u < cap - 64; i++) {
        const p0_sample_t *s = &g.ring[i];
        if (s->have_dcf) {
            u += (uint32_t)ksnprintf(buf + u, cap - u,
                "%lu %lu %ld %u %u %ld %u %u\n",
                (unsigned long)s->seq, (unsigned long)((s->mono_ms - g.t0_ms) / 1000u),
                (long)s->ntp_off_ms, s->ntp_rtt_ms, s->ntp_stratum,
                (long)s->dcf_err_ms, s->quality, s->clean_run);
        } else {
            u += (uint32_t)ksnprintf(buf + u, cap - u,
                "%lu %lu %ld %u %u - %u %u\n",
                (unsigned long)s->seq, (unsigned long)((s->mono_ms - g.t0_ms) / 1000u),
                (long)s->ntp_off_ms, s->ntp_rtt_ms, s->ntp_stratum,
                s->quality, s->clean_run);
        }
    }
    return u;
}
