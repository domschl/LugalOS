/*
 * The DCF-77 service: the receiver as a background time source rather than a
 * diagnostic (D3/D4, plan/phase17_clock_ui_and_dcf77.md).
 *
 * Everything DCF-77 in this tree until now has been something you *run* --
 * (dcf-raw), (dcf-listen), (dcf-sync) -- each owning the console and blocking
 * for minutes. This is the other half: a decoder fed from whatever loop is
 * already running at ~1 kHz (the clock's row scan), holding its state between
 * calls, so the signal is always being watched and a sync is a request rather
 * than an expedition.
 *
 * The plan's D4 wrote this as a state machine that powers the receiver up,
 * warms it, acquires, decodes, verifies, commits. With PON unwired
 * (CONFIG_DCF77_PON_PRESENT=0) the first three of those are gone: the module
 * is always on, so the decoder may as well always be listening. What is left
 * is smaller and strictly better -- the signal-quality display has live data
 * the moment you open it, and a sync that is requested while reception has
 * been good completes instantly instead of waiting two more minutes for
 * frames it has already seen.
 *
 * A time is never written unless somebody asked. Listening always, committing
 * only on request, is the distinction that keeps a radio from quietly
 * changing a clock that was deliberately set by hand.
 */

#ifndef DRIVERS_DCF77_SERVICE_H
#define DRIVERS_DCF77_SERVICE_H

#include "drivers/dcf77_decode.h"
#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DCF_IDLE = 0,     /* listening; nothing will be written */
    DCF_SYNCING,      /* a sync was asked for; the next good time commits */
    DCF_DONE,         /* the last request succeeded */
    DCF_FAILED        /* the last request timed out */
} dcf_state_t;

typedef struct {
    dcf_state_t state;
    uint32_t    timeout_left_s;   /* while syncing */

    bool        have_radio_time;  /* a validated time has been decoded, ever */
    rtc_time_t  radio_utc;        /* ...this one, at radio_at_ms */
    uint64_t    radio_at_ms;
    /* P3: the same instant, snapped to an interrupt-timestamped edge. False
     * means the capture had no candidate and radio_at_ms is all there is --
     * which is what every board had before P3, so a caller may simply prefer
     * this when it is available. */
    /* P4: the measured receiver delay, in microseconds, as the interval from
     * the GPS PPS edge that began a second to the DCF-77 mark for that same
     * second. Positive means the radio mark is late, which is what a
     * propagation and demodulation delay looks like. Only populated while a
     * trustworthy PPS is attached; pps_n is zero otherwise and the service
     * behaves exactly as it did before. */
    uint32_t    pps_n;
    bool        pps_have;
    int64_t     pps_last_us;
    int64_t     pps_mean_us;
    int64_t     pps_min_us, pps_max_us;
    uint32_t    pps_sd_us;

    /* The radio's claim checked against the local GPS reference (PPS for the
     * instant, NMEA for the second), in microseconds. This is the NTP
     * comparison without the network, and it validates the second label that
     * pps_* alone cannot. gerr_secbad counts frames that were a whole second
     * or more out -- a decode fault rather than a delay, kept out of the mean
     * so it cannot hide behind a plausible average. */
    uint32_t    gerr_n;
    uint32_t    gerr_secbad;
    int64_t     gerr_last_us;
    int64_t     gerr_mean_us;
    uint32_t    gerr_sd_us;

    uint64_t    radio_at_us;
    bool        radio_at_us_ok;
    uint32_t    edges_dropped;   /* a ring too small, or a line too noisy */
    uint32_t    edges_total;

    bool        ever_synced;
    rtc_time_t  last_sync_utc;
    uint64_t    last_sync_ms;
    uint32_t    attempts, successes;

    bool        pulse;            /* the second pulse is active right now */
    dcf77_stats_t decoder;        /* including the per-second quality bars */
} dcf_status_t;

/* Resets the decoder. Safe to call more than once. */
void dcf77_service_init(void);

/* Feed one sample of the receiver pin. Call at 50 Hz or better -- the clock's
 * row scan calls it at ~1 kHz. Cheap: one GPIO read and some arithmetic, no
 * I2C, no blocking, nothing that can fail. */
void dcf77_service_feed(uint64_t now_ms);

/* Ask for the clock to be set. Commits immediately if a validated time has
 * been decoded recently enough to still be trustworthy (carried forward by
 * the monotonic clock); otherwise waits for the next one, up to `timeout_s`
 * (0 = the default 5 minutes). Returns true if it committed on the spot. */
bool dcf77_service_request_sync(unsigned timeout_s);

void dcf77_service_cancel(void);

void dcf77_service_status(dcf_status_t *out);

/* Seconds since the last successful sync, or UINT32_MAX if there has never
 * been one. */
uint32_t dcf77_service_age_s(void);

/* The nightly automatic sync, at CONFIG_DCF77_AUTO_HOUR:CONFIG_DCF77_AUTO_MIN
 * in LOCAL time -- 03:17 by default. Off the hour, where the band is quieter
 * and every other radio clock in the house is not also listening, and at an
 * hour that covers the European DST changeover (03:00 CEST / 02:00 CET).
 *
 * Runtime, and lost at reset like every other setting on this persona (§5 of
 * the plan): the board-file default is the permanent answer. */
void dcf77_service_set_auto(bool on);
bool dcf77_service_auto(void);

#endif /* DRIVERS_DCF77_SERVICE_H */
