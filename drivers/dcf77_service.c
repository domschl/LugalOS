/* The DCF-77 service. See drivers/include/drivers/dcf77_service.h for what it
 * is for and why it is not the state machine the plan originally described. */

#include "drivers/dcf77_service.h"
#include "drivers/dcf77.h"
#include "drivers/edgecap.h"
#include "drivers/gps_pps.h"
#include "drivers/i2c_rtc.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "kernel/timezone.h"
#include "lugalos_config.h"
#include <string.h>

#define DEFAULT_TIMEOUT_S 300u

/* 03:17 local. Off the hour on purpose: the band is quieter there, and every
 * other radio clock in the house is listening on the hour. It also covers the
 * European DST changeover, which happens at 03:00 CEST / 02:00 CET -- though
 * with the clock running on UTC that matters less than it used to. */
#ifndef CONFIG_DCF77_AUTO_HOUR
#define CONFIG_DCF77_AUTO_HOUR 3
#endif
#ifndef CONFIG_DCF77_AUTO_MIN
#define CONFIG_DCF77_AUTO_MIN 17
#endif
#ifndef CONFIG_DCF77_AUTO_ENABLED
#define CONFIG_DCF77_AUTO_ENABLED 1
#endif

/* How stale a decoded time may be and still be committed on the spot.
 *
 * The value itself is exact however old it is -- it is carried forward by the
 * same monotonic counter the clock runs on, so age costs only that counter's
 * drift, which over minutes is nothing. The limit is about *trust*, not
 * arithmetic: a time decoded ten minutes ago was decoded from a signal that
 * may since have gone, and a sync that quietly succeeds off a stale frame is
 * indistinguishable from one that verified the radio is still there. Two
 * minutes is long enough to cover the walk from noticing the clock is wrong
 * to pressing the button. */
#define FRESH_MS 120000u

static dcf77_t     g_dec;
static dcf_state_t g_state;
static uint64_t    g_deadline_ms;

static bool        g_have_radio;
static rtc_time_t  g_radio_utc;
static uint64_t    g_radio_at_ms;
static uint64_t    g_radio_at_us;   /* the same instant, snapped to a real edge */
static bool        g_radio_at_us_ok;

/* P3: the pin's own edges, timestamped in an interrupt rather than by whoever
 * happened to sample it.
 *
 * The decoder is deliberately *not* moved onto these. It stays sample-driven
 * because its quality score's glitch term is sample-based and is the one
 * measurement that varies continuously with antenna orientation (phase 17,
 * D5) -- converting it would invalidate every D5 number as a baseline. What
 * changes is only the *instant* a completed frame is stamped with: the
 * decoder still decides which edge is the minute mark, and this supplies when
 * that edge actually happened.
 *
 * Sixteen entries is about four seconds of a clean signal, far more than the
 * sub-second window a snap searches. A noisy line will overrun it and say so
 * through the drop counter; that costs precision on the mark, not
 * correctness, because the fallback is the millisecond timestamp the decoder
 * already had. */
static edge_t     g_edge_ring[16];
static edgecap_t  g_edges;
static bool       g_edges_ok;

#define MARK_HISTORY 8
static uint64_t   g_mark_hist_us[MARK_HISTORY];
static uint32_t   g_mark_hist_n;

/* P4: the receiver's delay, measured rather than fitted.
 *
 * For each accepted frame whose mark landed on a real edge, the interval from
 * the PPS edge that began the same second to that mark. That interval is the
 * whole of what the radio path costs -- propagation from Mainflingen, the
 * receiver's filters, its AGC and its demodulator -- and it is what
 * CONFIG_DCF77_DELAY_US should eventually hold.
 *
 * Sum and sum-of-squares rather than a stored series: a spread is wanted, not
 * a history, and 64 bits hold it comfortably (a half-second offset squared is
 * 2.5e11, so a thousand samples stay four orders of magnitude clear of
 * overflow). Aggregates are what /proc reports; the per-sample values go out
 * through the P0 log, which is where a distribution or a temperature
 * correlation can actually be built. */
static uint32_t   g_pps_n;
static int64_t    g_pps_sum_us;
static uint64_t   g_pps_sumsq;
static int64_t    g_pps_min_us, g_pps_max_us;
static int64_t    g_pps_last_us;
static bool       g_pps_have;

/* The same comparison done entirely locally, second label included.
 *
 * pps_* above measures phase: how far the mark sits from the pulse that began
 * some second. It cannot tell *which* second, so a decoder that got the minute
 * wrong would still report a contented ~38 ms. NMEA supplies the label, and
 * the pair is a local stratum-0 reference -- so this is the NTP comparison
 * without the network: no WLAN round trip, no server, and microseconds of
 * resolution instead of the several milliseconds an offset over WiFi carries.
 *
 * Kept alongside the NTP figure rather than replacing it. Two references that
 * share no instrument are the only way to catch a fault in either, which is
 * what §P4 asks for; but this one is the better of the two and should be
 * believed first when they disagree. */
static uint32_t   g_gerr_n;
static int64_t    g_gerr_sum_us;
static uint64_t   g_gerr_sumsq;
static int64_t    g_gerr_last_us;
static uint32_t   g_gerr_secbad;

static bool        g_ever_synced;
static rtc_time_t  g_last_sync_utc;
static uint64_t    g_last_sync_ms;
static uint32_t    g_attempts, g_successes;
static bool        g_inited;

static bool        g_auto = (CONFIG_DCF77_AUTO_ENABLED != 0);
static int64_t     g_auto_last_day = -1;   /* civil day the nightly sync fired */
static uint64_t    g_next_auto_check_ms;

/* Carry a decoded instant forward to now and write it everywhere the clock
 * lives. The decoded time was true at the second-mark that ended its frame;
 * the monotonic counter says how long ago that was. */
static void commit(uint64_t now_ms) {
    uint64_t elapsed = (now_ms >= g_radio_at_ms) ? (now_ms - g_radio_at_ms) : 0;
    rtc_time_t utc;
    time_from_epoch(time_to_epoch(&g_radio_utc) + (int64_t)(elapsed / 1000u), &utc);
    utc.ms = (uint16_t)(elapsed % 1000u);

    time_set_utc(&utc);
    i2c_rtc_write_time(&utc);

    g_last_sync_utc = utc;
    g_last_sync_ms  = now_ms;
    g_ever_synced   = true;
    g_successes++;

    char iso[32];
    time_format_iso(&utc, iso, sizeof(iso));
    printk("[DCF77] clock set from the radio: %s UTC\n", iso);
}

void dcf77_service_init(void) {
    dcf77_init();
#if defined(CONFIG_BOARD_RP2350)
    if (!g_edges_ok) {
        g_edges_ok = edgecap_attach(&g_edges, CONFIG_DCF77_OUT_GPIO, g_edge_ring,
                                    (uint16_t)(sizeof(g_edge_ring) / sizeof(g_edge_ring[0]))) == 0;
    }
#endif
    dcf77_power(true);              /* a no-op where PON is not wired */
    dcf77_reset(&g_dec, dcf77_out_pulse_is_high());
    g_state = DCF_IDLE;
    g_deadline_ms = 0;
    g_have_radio = false;
    g_inited = true;
}

void dcf77_service_set_auto(bool on) { g_auto = on; }
bool dcf77_service_auto(void)        { return g_auto; }

/* Checked once a second, not once per sample: this is the only part of the
 * service that reads a clock, and doing it at 1 kHz would put a timezone
 * conversion on the row-scan hot path for no benefit whatsoever. */
static void auto_sync_check(uint64_t now_ms) {
    if (now_ms < g_next_auto_check_ms) return;
    g_next_auto_check_ms = now_ms + 1000u;

    if (!g_auto || g_state == DCF_SYNCING) return;

    rtc_time_t local;
    time_get_local(&local);
    int64_t day = time_to_epoch(&local) / 86400;

    /* Once per calendar day, and keyed on the day rather than on a timer, so
     * a clock that is itself being corrected cannot fire twice or skip. */
    if (local.hour != (unsigned)CONFIG_DCF77_AUTO_HOUR) return;
    if (local.min  != (unsigned)CONFIG_DCF77_AUTO_MIN)  return;
    if (day == g_auto_last_day) return;

    g_auto_last_day = day;
    printk("[DCF77] nightly sync at %02u:%02u local\n",
           (unsigned)local.hour, (unsigned)local.min);
    dcf77_service_request_sync(0);
}

void dcf77_service_feed(uint64_t now_ms) {
    if (!g_inited) dcf77_service_init();

    dcf77_feed(&g_dec, dcf77_raw_level(), now_ms);
    auto_sync_check(now_ms);

    /* Drain the capture, keeping only the pulse *starts* -- the level that
     * means "carrier attenuated" on this module, which is the transition the
     * decoder's marks refer to. The other edge carries the pulse width, a
     * property of the receiver rather than of the second. */
    {
        edge_t e;
        while (edgecap_pop(&g_edges, &e)) {
            if ((e.level != 0) != dcf77_out_pulse_is_high()) continue;
            g_mark_hist_us[g_mark_hist_n % MARK_HISTORY] = e.t_us;
            g_mark_hist_n++;
        }
    }

    rtc_time_t got;
    uint64_t   mark_ms = now_ms;
    if (dcf77_take_time(&g_dec, &got, &mark_ms)) {
        /* Always recorded, whether or not anyone asked for a sync: this is
         * what makes a later request instant, and what /proc/dcf77 reports as
         * "the radio is being heard" independently of whether the clock has
         * been changed. */
        g_radio_utc   = got;
        /* Snap the decoder's millisecond mark to the interrupt-timestamped
         * edge nearest it. The decoder decided *which* edge; this says when it
         * happened, to a microsecond rather than to whenever the frame loop
         * next looked. A search rather than "the newest edge", because the
         * ring may hold later edges on a noisy line.
         *
         * No candidate within half a second means the capture missed it -- a
         * full ring, or no interrupt at all where the attach failed. The
         * millisecond mark is then used unchanged, which is exactly the
         * behaviour before P3 and is why this cannot make anything worse. */
        g_radio_at_us_ok = false;
        {
            uint64_t target_us = mark_ms * 1000ull;
            uint64_t best = 0, best_err = 500000ull;
            uint32_t have = g_mark_hist_n < MARK_HISTORY ? g_mark_hist_n : MARK_HISTORY;
            for (uint32_t i = 0; i < have; i++) {
                uint64_t t = g_mark_hist_us[i];
                uint64_t err = (t > target_us) ? (t - target_us) : (target_us - t);
                if (err < best_err) { best_err = err; best = t; }
            }
            if (best) { g_radio_at_us = best; g_radio_at_us_ok = true; }
        }

        /* P4. Only from an edge-snapped mark: the millisecond fallback is
         * quantised at 25-35 ms by the debounce, which is larger than the
         * whole quantity being measured and would poison the mean rather than
         * merely widen it. No PPS, or no edge, simply means no sample -- the
         * calibration is a thing that happens while the GPS is attached, and
         * the service works exactly as before without it. */
        if (g_radio_at_us_ok) {
            int64_t off;
            if (gps_pps_offset_us(g_radio_at_us, &off)) {
                if (g_pps_n == 0) { g_pps_min_us = g_pps_max_us = off; }
                else {
                    if (off < g_pps_min_us) g_pps_min_us = off;
                    if (off > g_pps_max_us) g_pps_max_us = off;
                }
                g_pps_sum_us += off;
                g_pps_sumsq  += (uint64_t)(off * off);
                g_pps_n++;
                g_pps_last_us = off;
                g_pps_have = true;
            }

            /* The whole claim, against the local reference. */
            int64_t gnow;
            if (gps_epoch_us(g_radio_at_us, &gnow)) {
                int64_t claimed = time_to_epoch(&got) * 1000000LL
                                + (int64_t)got.ms * 1000LL;
                int64_t err = claimed - gnow;
                /* A whole second out is a decode fault, not a delay, and
                 * averaging it in would hide it behind a plausible mean. It
                 * is counted separately and loudly. */
                if (err > 500000LL || err < -500000LL) {
                    g_gerr_secbad++;
                } else {
                    g_gerr_sum_us += err;
                    g_gerr_sumsq  += (uint64_t)(err * err);
                    g_gerr_n++;
                    g_gerr_last_us = err;
                }
            }
        }
        /* P1 (plan/phase24_dcf77_precision_and_ntp_server.md): the decoder's
         * own mark, not this call's `now`.
         *
         * They are not the same instant and the difference is not small. A
         * transition is only confirmed once DEBOUNCE_MS (25 ms) has passed at
         * the new level, and this function learns of it on whichever sample
         * happens to notice -- once per display frame when the panel is
         * scanning. So `now_ms` ran 25-35 ms behind the second it was
         * labelling, and every clock this service ever set was that much
         * slow. The decoder had the correct timestamp throughout; nothing
         * asked it for it.
         *
         * Measured against a GPS-disciplined stratum-1 the night before this
         * changed: -65.5 ms with 5.0 ms of scatter. This is about half of it.
         * The rest is the receiver's own group delay, which is P4's to
         * calibrate and not software's to fix. */
        g_radio_at_ms = mark_ms;
        g_have_radio  = true;

        if (g_state == DCF_SYNCING) {
            commit(now_ms);
            g_state = DCF_DONE;
            g_deadline_ms = 0;
        }
    }

    if (g_state == DCF_SYNCING && g_deadline_ms && now_ms >= g_deadline_ms) {
        /* Nothing is written on failure. A DS3231 holding a slightly-wrong
         * time is categorically better than one holding a garbage time. */
        g_state = DCF_FAILED;
        g_deadline_ms = 0;
        printk("[DCF77] sync timed out; the clock was not changed\n");
    }
}

bool dcf77_service_request_sync(unsigned timeout_s) {
    if (!g_inited) dcf77_service_init();
    uint64_t now = time_get_ms();
    g_attempts++;

    if (g_have_radio && (now - g_radio_at_ms) <= FRESH_MS) {
        commit(now);
        g_state = DCF_DONE;
        g_deadline_ms = 0;
        return true;
    }

    g_state = DCF_SYNCING;
    g_deadline_ms = now + (uint64_t)(timeout_s ? timeout_s : DEFAULT_TIMEOUT_S) * 1000u;
    return false;
}

void dcf77_service_cancel(void) {
    if (g_state == DCF_SYNCING) g_state = DCF_IDLE;
    g_deadline_ms = 0;
}

uint32_t dcf77_service_age_s(void) {
    if (!g_ever_synced) return 0xFFFFFFFFu;
    uint64_t now = time_get_ms();
    return (uint32_t)((now >= g_last_sync_ms) ? (now - g_last_sync_ms) / 1000u : 0);
}

void dcf77_service_status(dcf_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!g_inited) { out->state = DCF_IDLE; return; }

    uint64_t now = time_get_ms();
    out->state = g_state;
    out->timeout_left_s = (g_state == DCF_SYNCING && g_deadline_ms > now)
                            ? (uint32_t)((g_deadline_ms - now) / 1000u) : 0;
    out->have_radio_time = g_have_radio;
    out->radio_utc = g_radio_utc;
    out->radio_at_ms = g_radio_at_ms;
    out->radio_at_us = g_radio_at_us;
    out->radio_at_us_ok = g_radio_at_us_ok;
    out->pps_n       = g_pps_n;
    out->pps_have    = g_pps_have;
    out->pps_last_us = g_pps_last_us;
    out->pps_min_us  = g_pps_min_us;
    out->pps_max_us  = g_pps_max_us;
    if (g_pps_n) {
        int64_t mean = g_pps_sum_us / (int64_t)g_pps_n;
        out->pps_mean_us = mean;
        /* Population variance, integer throughout: E[x^2] - (E[x])^2. The
         * sample count is small and the quantities are microseconds, so
         * nothing here needs floating point. */
        int64_t var = (int64_t)(g_pps_sumsq / g_pps_n) - mean * mean;
        if (var < 0) var = 0;
        uint64_t r = 0, v = (uint64_t)var;
        while ((r + 1ull) * (r + 1ull) <= v) r++;   /* integer sqrt */
        out->pps_sd_us = (uint32_t)r;
    } else {
        out->pps_mean_us = 0;
        out->pps_sd_us = 0;
    }
    out->gerr_n       = g_gerr_n;
    out->gerr_secbad  = g_gerr_secbad;
    out->gerr_last_us = g_gerr_last_us;
    if (g_gerr_n) {
        int64_t m = g_gerr_sum_us / (int64_t)g_gerr_n;
        out->gerr_mean_us = m;
        int64_t var = (int64_t)(g_gerr_sumsq / g_gerr_n) - m * m;
        if (var < 0) var = 0;
        uint64_t r = 0, v = (uint64_t)var;
        while ((r + 1ull) * (r + 1ull) <= v) r++;
        out->gerr_sd_us = (uint32_t)r;
    } else {
        out->gerr_mean_us = 0;
        out->gerr_sd_us = 0;
    }
    out->edges_dropped = edgecap_dropped(&g_edges);
    out->edges_total = edgecap_total(&g_edges);
    out->ever_synced = g_ever_synced;
    out->last_sync_utc = g_last_sync_utc;
    out->last_sync_ms = g_last_sync_ms;
    out->attempts = g_attempts;
    out->successes = g_successes;
    out->pulse = (dcf77_raw_level() == dcf77_out_pulse_is_high());
    dcf77_get_stats(&g_dec, &out->decoder);
}
