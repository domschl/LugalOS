/*
 * DCF-77 frame decoder (D1, plan/phase17_clock_ui_and_dcf77.md).
 *
 * Target-independent by construction: it consumes timestamped samples of a
 * pin level, never a register, so it builds and is tested on QEMU exactly as
 * it runs on RP2350. That is the point -- the fiddly part of DCF-77 is the
 * frame logic, and it should not be debugged by flashing a board and waiting
 * minutes for radio. `dcf77selftest` drives every path below from synthetic
 * sample streams (kernel/shell.c), on every target, in milliseconds.
 *
 * The protocol, since every constant here refers to it: the 77.5 kHz carrier
 * from Mainflingen is attenuated to ~15% at the start of every second, for
 * 100 ms (bit 0) or 200 ms (bit 1). Second 59 carries no attenuation at all,
 * so a ~2 s gap is the minute boundary. Bits 0-58 of a frame:
 *
 *   0        always 0 (start of minute)
 *   1-14     civil warning bits (encrypted, ignored here)
 *   15       R  - backup antenna in use
 *   16       A1 - a DST change is announced within the hour
 *   17,18    Z1,Z2 - timezone: 1,0 = CEST (UTC+2), 0,1 = CET (UTC+1)
 *   19       A2 - a leap second is announced
 *   20       always 1 (start of encoded time)
 *   21-27    minute,  BCD weights 1,2,4,8,10,20,40
 *   28       P1 - even parity over 21-28
 *   29-34    hour,    BCD weights 1,2,4,8,10,20
 *   35       P2 - even parity over 29-35
 *   36-41    day,     BCD weights 1,2,4,8,10,20
 *   42-44    weekday, 1 = Monday .. 7 = Sunday
 *   45-49    month,   BCD weights 1,2,4,8,10
 *   50-57    year,    BCD weights 1,2,4,8,10,20,40,80 (within the century)
 *   58       P3 - even parity over 36-58
 *
 * And the detail that silently costs an hour of confusion if missed: the
 * time in a frame is the time of the minute that *starts at the end of that
 * frame*, not the minute during which it was transmitted. It becomes valid
 * at the second-mark following the missing 59th pulse.
 *
 * Everything this decoder hands out is UTC, converted using the frame's own
 * Z1/Z2 bits. What goes on the air is German local time; what a clock should
 * keep is UTC (kernel/timezone.h).
 */

#ifndef DRIVERS_DCF77_DECODE_H
#define DRIVERS_DCF77_DECODE_H

#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

#define DCF77_QUALITY_SLOTS 24  /* one per second; 24 = the matrix's width */

typedef struct {
    uint32_t pulses_seen;
    uint32_t pulses_bad;        /* width outside both 100 ms and 200 ms windows */
    uint32_t sync_losses;       /* spacing that fitted neither 1 s nor 2 s */
    uint32_t frames_seen;       /* minute marks reached with a full frame */
    uint32_t frames_accepted;   /* ...that passed every check */
    uint32_t parity_errors;
    uint32_t framing_errors;    /* bit 0 / bit 20 / timezone bits wrong */
    uint32_t range_errors;      /* a field outside its legal range */
    uint32_t weekday_errors;    /* frame's weekday disagrees with its own date */
    uint32_t spacing_err_max_ms;
    uint32_t glitches;          /* transitions rejected by the debounce, total */
    /* Every score ever pushed, not just the 24 still in the ring: the mean of
     * these is the single number that makes two reception conditions
     * comparable (plan/phase17_clock_ui_and_dcf77.md D5). */
    uint32_t quality_sum, quality_total;
    /* Longest unbroken run of seconds that held bit alignment. A frame needs
     * 59 consecutive ones, so this says directly whether reception is capable
     * of producing a frame at all -- which neither a mean nor a loss count
     * does. */
    uint32_t clean_run_max;
    /* Per-second score, newest at [count-1], sized for a per-column bar on the
     * LED matrix. 0 means no pulse arrived in that second at all; 1 to 7 grade
     * one that did, from three measurements taken together: how far the
     * spacing was from 1000 ms, how far the width was from 100 or 200 ms, and
     * how many sub-debounce glitches the line produced in that second.
     *
     * The glitch term is the one that makes this useful for aiming an
     * antenna. Spacing and width are stable right up until reception fails
     * and then collapse, so a score built from those alone is a pass/fail
     * light; glitch count climbs smoothly as the rod turns off broadside. */
    uint8_t  quality[DCF77_QUALITY_SLOTS];
    uint8_t  quality_count;
    bool     pulse_is_high;     /* the configured polarity, echoed back */
    uint32_t high_permille;     /* measured duty cycle: for a jumper-vs-config
                                 * warning only, never used for a decision */
    int      bit_index;         /* how far into the current frame, -1 = unsynced */
    int      utc_offset_min;    /* +60 CET / +120 CEST, from the last frame's
                                 * Z1/Z2 bits -- what was subtracted to reach
                                 * the UTC that dcf77_take_time() hands back */
    bool     is_dst;            /* that frame said CEST */
} dcf77_stats_t;

typedef struct {
    /* Polarity is CONFIGURED, not inferred (user, 2026-08-22): the receiver
     * has a jumper for it, so it is a hardware fact like any other pin fact,
     * and inferring it from duty cycle needs a good signal to work -- which
     * is exactly the condition that is not guaranteed while a clock is
     * starting up or an antenna is being moved. Guessing wrong there locks
     * the decoder onto the gaps rather than the pulses.
     *
     * The duty cycle is still measured, but only so a caller can warn that
     * the jumper and the board file disagree. It never changes a decision. */
    bool     pulse_is_high;
    uint32_t win_samples, win_high;
    uint64_t win_start_ms;

    /* Debounced edge detection. */
    bool     have_level, level, cand;
    uint64_t cand_since_ms;

    /* Pulse timing. */
    bool     in_pulse, have_prev;
    uint64_t pulse_start_ms, prev_start_ms;

    /* Carried from a pulse's start to its end, where the score is worked out
     * from all three measurements at once. */
    uint32_t pending_gap_err;
    uint32_t glitches;          /* since the previous pulse ended */
    uint32_t clean_run;         /* consecutive seconds holding bit alignment */
    uint64_t quiet_next_ms;     /* when to push the next empty column */

    /* Frame assembly. */
    uint8_t  bits[59];
    int      bit_index;         /* -1 until a minute mark is seen */

    /* Two-frame agreement, but NOT necessarily two *consecutive* frames: a
     * frame is believed when a later one decodes to exactly as many minutes
     * later as our own monotonic clock says have passed. Requiring adjacency
     * makes marginal reception hopeless -- one bad bit anywhere in 59 costs
     * the whole pair -- while checking against elapsed time is an equally
     * strong agreement (arguably stronger: it constrains two independent
     * frames *and* the interval between them). A previously decoded frame
     * therefore survives sync losses and rejected frames; only the pairing
     * window retires it. */
    bool     have_prev_frame;
    uint32_t prev_frame_minutes;    /* linear minute count */
    uint64_t prev_frame_mark_ms;    /* when that frame's minute started */

    /* Result, latched until taken. */
    bool     time_ready;
    rtc_time_t out_time;
    /* The instant out_time was true: the start of the pulse that ended the
     * minute mark, on the same clock dcf77_feed() is given. Latched with the
     * time because a caller that learns *what* the time was without learning
     * *when* has to substitute its own "now", and its now is later than this
     * by the debounce plus however long since it last sampled -- which is
     * tens of milliseconds it will then never get back. See
     * dcf77_take_time(). */
    uint64_t out_mark_ms;

    dcf77_stats_t stats;
} dcf77_t;

/* `pulse_is_high` states which level the carrier attenuation appears as on
 * this module -- a board fact (CONFIG_DCF77_OUT_ACTIVE_LOW, matching the
 * module's own polarity jumper), passed explicitly so no caller can forget
 * that it is a decision someone has to make. */
void dcf77_reset(dcf77_t *d, bool pulse_is_high);

/* Feed one sample of the RAW pin level -- polarity correction happens inside
 * from the configured value, so callers pass what the pin reads and nothing
 * else. Call at 50 Hz or
 * better; 1 kHz is what the RP2350 scan loop provides and is ~100x finer than
 * the 100/200 ms distinction needs. Timestamps come from the caller so this
 * is testable against synthetic time. */
void dcf77_feed(dcf77_t *d, bool raw_level, uint64_t now_ms);

/* True once, per validated frame pair; copies the time out and clears the
 * flag. The time is the start of a minute, so sec and ms are always 0.
 *
 * It is **UTC**, not the German wall-clock time on the air. The frame states
 * its own offset in Z1/Z2, so the conversion is exact rather than inferred,
 * and the kernel clock this feeds runs on UTC (kernel/timezone.h). The offset
 * that was applied is in the stats, for anyone who wants to show CET/CEST.
 *
 * `mark_ms_out`, if given, receives **the instant that time was true** -- the
 * mark this decoder already computed, on the caller's own clock. Take it.
 * The alternative is to read a clock at the moment `true` comes back, and
 * that moment is not the mark: a transition is only confirmed once
 * DEBOUNCE_MS has passed at the new level, and the caller only learns of it
 * on its next sample, so "now" runs about 25-35 ms behind the second it
 * describes. Between phase 17 and P1 of phase 24 every radio-set clock in
 * this tree was that much slow, silently, because this function had nothing
 * to hand back but a date -- and the decoder had the right number the whole
 * time. Measured before the fix: -65.5 ms against a GPS reference, of which
 * this was about half. NULL is allowed, for a caller that genuinely only
 * wants the date. */
bool dcf77_take_time(dcf77_t *d, rtc_time_t *out, uint64_t *mark_ms_out);

void dcf77_get_stats(const dcf77_t *d, dcf77_stats_t *out);

/* 1 = Monday .. 7 = Sunday, computed from the date. Used to cross-check the
 * weekday a frame carries, and by the clock UI's weekday LEDs (C1), which is
 * why it is public rather than file-local. */
unsigned dcf77_weekday_from_date(uint16_t year, uint8_t month, uint8_t day);

/* Drives every path above from synthetic sample streams and returns the
 * number of failures (0 = all passed), printing a line per case. No hardware,
 * no radio, no waiting -- see kernel/shell.c's `dcf77selftest`. */
int dcf77_selftest(void);

#endif /* DRIVERS_DCF77_DECODE_H */
