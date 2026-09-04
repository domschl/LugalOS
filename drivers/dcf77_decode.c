/*
 * DCF-77 frame decoder (D1, plan/phase17_clock_ui_and_dcf77.md).
 * See drivers/include/drivers/dcf77_decode.h for the protocol and the API.
 */

#include "drivers/dcf77_decode.h"
#include "kernel/timezone.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include <string.h>

/* Pulse classification. Nominal 100 ms and 200 ms; the windows are wide
 * because the receiver's own AGC and our sampling both add tens of
 * milliseconds, and the two symbols are 100 ms apart -- there is no benefit
 * in being strict and a real cost in rejecting good frames. */
#define BIT0_MIN_MS   60u
#define BIT0_MAX_MS  150u
#define BIT1_MIN_MS  150u
#define BIT1_MAX_MS  300u

/* Second and minute spacing, measured pulse-start to pulse-start. */
#define SEC_MIN_MS    850u
#define SEC_MAX_MS   1150u
#define MARK_MIN_MS  1700u
#define MARK_MAX_MS  2300u

/* A level must hold this long to count as a transition.
 *
 * Was 5 ms, which is a switch-bounce figure and has no business here: the
 * shortest feature this protocol has is a 100 ms pulse, so anything an order
 * of magnitude below that is noise by definition. Measured on hardware
 * 2026-08-23 -- with the LED panel running, 300 seconds of radio produced
 * *309* pulses. The display was not degrading pulses, it was inventing about
 * nine of them, each long enough to clear 5 ms. Every invented pulse makes
 * two impossible gaps, which cost 24 sync losses -- one every 12 seconds --
 * and frame assembly needs 59 consecutive clean ones, so not a single frame
 * completed in five minutes. With the panel dark: 300 pulses, 10 losses, two
 * frames, one accepted.
 *
 * 25 ms rejects anything four times shorter than the shortest real pulse
 * while remaining four times shorter than that pulse itself, so a genuine
 * 100 ms bit cannot be filtered out. Widths and gaps are unaffected either
 * way: transitions are timestamped at the first sample that saw the new
 * level, not at the moment the debounce completes. */
#define DEBOUNCE_MS    25u
#define POLARITY_WIN_MS 4000u

#define FRAME_BITS 59

/* How far apart two frames may be and still be paired. Our own monotonic
 * clock is a crystal; over half an hour its error against the transmitter is
 * nowhere near the +/-30 s that would make the rounded minute count ambiguous.
 * Beyond that, retire the old frame rather than trust it. */
#define PAIR_MAX_MINUTES 30u

/* ------------------------------------------------------------ calendar -- */

/* Both of these used to be built on a private copy of Howard Hinnant's
 * days_from_civil(). kernel/time.c grew the same arithmetic when the clock
 * moved to UTC, so they defer to it -- one calendar implementation in the
 * tree, exercised by two selftests instead of one. The dependency is on
 * kernel/time.h only, which is a header of pure conversions with no hardware
 * behind it, so this file stays as target-independent and as QEMU-testable as
 * it was.
 *
 * The weekday is a public entry point because two callers outside the decoder
 * want it: the frame's own weekday cross-check here, and the clock UI's
 * weekday LEDs. The minute count turns a decoded date into a single number,
 * so "one minute later" is one comparison and day/month/year rollover needs
 * no special case. */
unsigned dcf77_weekday_from_date(uint16_t year, uint8_t month, uint8_t day) {
    rtc_time_t t = { year, month, day, 0, 0, 0, 0 };
    return time_weekday(&t);
}

static uint32_t linear_minutes(const rtc_time_t *t) {
    return (uint32_t)(time_to_epoch(t) / 60);
}

/* -------------------------------------------------------------- frame --- */

static unsigned bcd(const uint8_t *bits, unsigned first, unsigned count) {
    static const unsigned weights[8] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    unsigned v = 0;
    for (unsigned i = 0; i < count; i++) {
        if (bits[first + i]) v += weights[i];
    }
    return v;
}

static bool even_parity_ok(const uint8_t *bits, unsigned first, unsigned last) {
    unsigned ones = 0;
    for (unsigned i = first; i <= last; i++) ones += bits[i] ? 1u : 0u;
    return (ones & 1u) == 0u;
}

/* Decode one complete frame. Returns false, and bumps the matching counter,
 * on any failure -- every check is a real one taken from the spec, not a
 * plausibility guess. */
static bool decode_frame(dcf77_t *d, rtc_time_t *out) {
    const uint8_t *b = d->bits;

    if (b[0] != 0 || b[20] != 1) { d->stats.framing_errors++; return false; }

    /* Exactly one timezone bit. Both set or both clear is a corrupt frame,
     * not an ambiguity to resolve. */
    if ((b[17] ? 1 : 0) + (b[18] ? 1 : 0) != 1) { d->stats.framing_errors++; return false; }

    if (!even_parity_ok(b, 21, 28) ||
        !even_parity_ok(b, 29, 35) ||
        !even_parity_ok(b, 36, 58)) { d->stats.parity_errors++; return false; }

    /* A2: taken from a frame that has already passed framing and parity, so
     * it is as trustworthy as the time in the same frame and no more. */
    d->stats.leap_announced = b[19] != 0;

    unsigned minute  = bcd(b, 21, 7);
    unsigned hour    = bcd(b, 29, 6);
    unsigned day     = bcd(b, 36, 6);
    unsigned weekday = bcd(b, 42, 3);
    unsigned month   = bcd(b, 45, 5);
    unsigned year    = bcd(b, 50, 8);

    if (minute > 59 || hour > 23 || day < 1 || day > 31 ||
        weekday < 1 || weekday > 7 || month < 1 || month > 12 || year > 99) {
        d->stats.range_errors++;
        return false;
    }

    rtc_time_t t;
    t.year  = (uint16_t)(2000u + year);
    t.month = (uint8_t)month;
    t.day   = (uint8_t)day;
    t.hour  = (uint8_t)hour;
    t.min   = (uint8_t)minute;
    t.sec   = 0;
    t.ms    = 0;

    /* The frame carries a weekday *and* a date, so they can be checked
     * against each other -- a free integrity test the protocol hands us, and
     * one that catches corruptions all three parity bits can survive. */
    if (dcf77_weekday_from_date(t.year, t.month, t.day) != weekday) {
        d->stats.weekday_errors++;
        return false;
    }

    /* Hand back UTC, not the German wall-clock time the frame carries. Z1/Z2
     * state the offset that applies, so this needs no timezone rule and no
     * guess -- the transmitter tells us, twice a year at exactly the right
     * second. Converting here rather than in the caller also puts the
     * two-frame agreement below on a monotonic scale: across a DST switch two
     * frames a minute apart differ by 61 or -59 local minutes, and by exactly
     * one UTC minute. The old code would have rejected the pair on the one
     * night of the year it matters most. */
    int off_min = b[17] ? 120 : 60;   /* CEST : CET */
    d->stats.utc_offset_min = off_min;
    d->stats.is_dst = (b[17] != 0);
    tz_from_offset(&t, off_min, out);
    return true;
}

/* A frame is only believed when the next one decodes to exactly one minute
 * later. Two independent 59-bit frames agreeing that way is far stronger than
 * any single frame's parity, and it is what every serious DCF-77 clock does;
 * the cost is that a sync takes at least two minutes. */
static void frame_complete(dcf77_t *d, uint64_t mark_ms) {
    d->stats.frames_seen++;

    rtc_time_t t;
    if (!decode_frame(d, &t)) {
        /* Deliberately does NOT discard the previous good frame: it is a
         * validated absolute time, and nothing that happens afterwards makes
         * it wrong. Discarding it here is what made marginal reception
         * hopeless -- one bad bit and the pairing had to start over. */
        return;
    }

    uint32_t minutes = linear_minutes(&t);
    if (d->have_prev_frame) {
        /* Round to the nearest minute: the mark timestamps are real edges,
         * so the interval is a whole number of minutes plus jitter. */
        uint64_t elapsed_ms = mark_ms - d->prev_frame_mark_ms;
        uint32_t elapsed_min = (uint32_t)((elapsed_ms + 30000u) / 60000u);
        if (elapsed_min >= 1u && elapsed_min <= PAIR_MAX_MINUTES &&
            minutes == d->prev_frame_minutes + elapsed_min) {
            d->stats.frames_accepted++;
            d->out_time = t;
            d->out_mark_ms = mark_ms;   /* P1: the instant it was true */
            d->time_ready = true;
        }
    }
    d->prev_frame_minutes = minutes;
    d->prev_frame_mark_ms = mark_ms;
    d->have_prev_frame = true;
}

/* --------------------------------------------------------------- feed --- */

static void quality_push(dcf77_t *d, uint8_t score) {
    /* Running totals as well as the 24-slot ring. The ring is for the bar
     * chart, which is a picture; these are for comparing two runs, which
     * needs a number. */
    d->stats.quality_sum += score;
    d->stats.quality_total++;

    /* The longest unbroken run of seconds that kept bit alignment. This is
     * the number that decides whether a clock can ever set itself: a frame is
     * 59 CONSECUTIVE seconds, so a run of 58 is worth exactly as much as a run
     * of 3. Neither a mean nor a total loss count says that -- 6 losses in 300
     * seconds sounds like 98% success and can still mean no frame ever
     * completes (measured 2026-08-23). */
    if (score == 0) {
        d->clean_run = 0;
    } else {
        d->clean_run++;
        if (d->clean_run > d->stats.clean_run_max) d->stats.clean_run_max = d->clean_run;
    }

    if (d->stats.quality_count < DCF77_QUALITY_SLOTS) {
        d->stats.quality[d->stats.quality_count++] = score;
    } else {
        for (unsigned i = 1; i < DCF77_QUALITY_SLOTS; i++) {
            d->stats.quality[i - 1] = d->stats.quality[i];
        }
        d->stats.quality[DCF77_QUALITY_SLOTS - 1] = score;
    }
}

void dcf77_reset(dcf77_t *d, bool pulse_is_high) {
    memset(d, 0, sizeof(*d));
    d->pulse_is_high = pulse_is_high;
    d->bit_index = -1;
    d->stats.bit_index = -1;
    d->stats.pulse_is_high = pulse_is_high;
}

/* 7 is a clean pulse, 1 is one that arrived but told us nothing useful, and 0
 * is reserved for "no pulse in this second at all" -- a different statement,
 * made by the empty columns.
 *
 * Deliberately finer than the thresholds this replaces. Those went 7, 6, 5, 3
 * on spacing error alone and so read 7 in any reception good enough to work.
 * A monitor whose job is to be watched while an antenna is rotated has to
 * move *before* the signal fails, not when it does. */
static uint8_t grade(uint32_t gap_err_ms, uint32_t width_err_ms, uint32_t glitches) {
    /* Noise counts explode rather than creep, so the glitch term is graded on
     * a roughly logarithmic ladder rather than counted one for one. The first
     * rung starts at three deliberately: a demodulated output with a slow
     * edge can bounce once or twice on every single transition even when
     * reception is perfect, and a meter that reads 6 out of 7 in ideal
     * conditions has thrown away its top division for nothing. */
    static const uint16_t GLITCH_RUNG[4] = { 3u, 6u, 10u, 18u };

    uint32_t penalty = 0;
    penalty += (gap_err_ms   / 25u > 2u) ? 2u : gap_err_ms   / 25u;
    penalty += (width_err_ms / 25u > 2u) ? 2u : width_err_ms / 25u;
    for (unsigned i = 0; i < 4; i++) if (glitches >= GLITCH_RUNG[i]) penalty++;

    return (penalty >= 6u) ? 1u : (uint8_t)(7u - penalty);
}

static void note_pulse_start(dcf77_t *d, uint64_t t) {
    /* Silence beyond a minute mark is measured in whole seconds from here. */
    d->quiet_next_ms = t + 2500u;
    d->pending_gap_err = 0;

    if (d->have_prev) {
        uint64_t gap = t - d->prev_start_ms;
        uint32_t err = (gap > 1000u) ? (uint32_t)(gap - 1000u) : (uint32_t)(1000u - gap);

        if (gap >= MARK_MIN_MS && gap <= MARK_MAX_MS) {
            /* The missing second-59 pulse: the frame just collected is
             * complete, and the minute it describes starts now. */
            if (d->bit_index == FRAME_BITS - 1) frame_complete(d, t);
            d->bit_index = 0;
        } else if (gap >= SEC_MIN_MS && gap <= SEC_MAX_MS) {
            if (d->bit_index >= 0) {
                d->bit_index++;
                if (d->bit_index >= FRAME_BITS) {
                    /* More than 59 seconds without a minute mark: the mark was
                     * missed, so this frame is not trustworthy. Resync -- but
                     * keep any previously decoded frame, which losing bit
                     * alignment now does not invalidate. */
                    d->bit_index = -1;
                    d->clean_run = 0;
                    d->stats.sync_losses++;
                }
            }
            if (err > d->stats.spacing_err_max_ms) d->stats.spacing_err_max_ms = err;
            d->pending_gap_err = err;
        } else {
            /* Neither one second nor two: a pulse was missed or invented, and
             * with it the bit positions. Everything after this is guesswork
             * until the next minute mark, so stop pretending otherwise. */
            d->bit_index = -1;
            d->clean_run = 0;
            d->stats.sync_losses++;
            d->pending_gap_err = 1000u;   /* saturates the penalty */
        }
    }

    d->prev_start_ms = t;
    d->have_prev = true;
}

static void note_pulse_end(dcf77_t *d, uint64_t t) {
    uint32_t width = (uint32_t)(t - d->pulse_start_ms);
    d->stats.pulses_seen++;

    int value = -1;
    if (width >= BIT0_MIN_MS && width < BIT0_MAX_MS)       value = 0;
    else if (width >= BIT1_MIN_MS && width <= BIT1_MAX_MS) value = 1;
    else                                                    d->stats.pulses_bad++;

    /* Distance from whichever nominal width this pulse was trying to be. An
     * unreadable width saturates it rather than being a special case. */
    uint32_t width_err;
    if (value < 0) {
        width_err = 1000u;
        /* An unreadable bit makes the whole frame unusable: better to resync
         * than to decode a frame with one guessed bit in it. The previously
         * decoded frame survives -- it is an absolute time, not a position. */
        d->bit_index = -1;
        d->clean_run = 0;
    } else {
        uint32_t nominal = value ? 200u : 100u;
        width_err = (width > nominal) ? (width - nominal) : (nominal - width);
        if (d->bit_index >= 0 && d->bit_index < FRAME_BITS) {
            d->bits[d->bit_index] = (uint8_t)value;
        }
    }

    /* The glitches counted since the previous pulse ended -- one whole second
     * of line noise, whether it landed in the gap or on the pulse itself. */
    uint32_t glitches = d->glitches;
    d->glitches = 0;

    quality_push(d, grade(d->pending_gap_err, width_err, glitches));
}

void dcf77_feed(dcf77_t *d, bool raw_level, uint64_t now_ms) {
    /* --- duty cycle: reported, never acted on -------------------------- */
    if (d->win_samples == 0) d->win_start_ms = now_ms;
    d->win_samples++;
    if (raw_level) d->win_high++;
    if (now_ms - d->win_start_ms >= POLARITY_WIN_MS) {
        d->stats.high_permille = (d->win_high * 1000u) / d->win_samples;
        d->win_samples = 0;
        d->win_high = 0;
    }

    /* Silence past a minute mark's 2 s pushes an empty column a second, so
     * turning a ferrite rod into its null shows the chart marching on and
     * emptying, rather than simply freezing -- which looks identical to the
     * clock having crashed. */
    if (d->have_prev && d->quiet_next_ms && now_ms >= d->quiet_next_ms) {
        quality_push(d, 0);
        d->quiet_next_ms += 1000u;
    }

    bool active = d->pulse_is_high ? raw_level : !raw_level;

    /* --- debounced edges ------------------------------------------------ */
    if (!d->have_level) {
        d->have_level = true;
        d->level = active;
        d->cand = active;
        d->cand_since_ms = now_ms;
        d->in_pulse = active;
        return;
    }

    if (active != d->cand) {
        /* A candidate transition that dies before the debounce completes is a
         * glitch: the line moved and moved back inside 5 ms. These used to be
         * silently discarded, which cost the quality score the one quantity
         * that varies *continuously* with antenna orientation. Everything
         * else -- spacing, width -- is stable right up until reception fails
         * and then collapses, so a score built from those alone is a pass/fail
         * light. Glitch count rises smoothly as the rod turns off broadside
         * and the AGC opens up, which is exactly what someone aiming an
         * antenna needs to watch (user, 2026-08-23). */
        if (d->cand != d->level) {
            d->glitches++;
            d->stats.glitches++;
        }
        d->cand = active;
        d->cand_since_ms = now_ms;
        return;
    }
    if (d->cand == d->level) return;
    if (now_ms - d->cand_since_ms < DEBOUNCE_MS) return;

    /* Timestamp the transition at the first sample that saw the new level,
     * not at the moment the debounce completed -- otherwise every width and
     * every gap carries a systematic +DEBOUNCE_MS. */
    uint64_t at = d->cand_since_ms;
    d->level = d->cand;

    if (d->level) {
        d->pulse_start_ms = at;
        d->in_pulse = true;
        note_pulse_start(d, at);
    } else if (d->in_pulse) {
        d->in_pulse = false;
        note_pulse_end(d, at);
    }
    d->stats.bit_index = d->bit_index;
}

bool dcf77_take_time(dcf77_t *d, rtc_time_t *out, uint64_t *mark_ms_out) {
    if (!d->time_ready) return false;
    *out = d->out_time;
    if (mark_ms_out) *mark_ms_out = d->out_mark_ms;
    d->time_ready = false;
    return true;
}

void dcf77_get_stats(const dcf77_t *d, dcf77_stats_t *out) {
    *out = d->stats;
}

/* ----------------------------------------------------------- selftest --- */
/*
 * Everything below exists so the frame logic above is exercised without a
 * radio: synthetic sample streams, simulated time, a few milliseconds per
 * case. It runs on every target from `dcf77selftest` (kernel/shell.c) and is
 * in tests/runner.py, so a change to the decoder is caught on QEMU rather
 * than by flashing a board and waiting minutes for a signal that may not
 * arrive.
 */

static void civil_from_days(long z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const long y0 = (long)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = mp + (mp < 10u ? 3u : (unsigned)-9);
    *y = (int)(y0 + (mm <= 2u));
    *m = mm;
    *d = dd;
}

static void add_minutes(rtc_time_t *t, int n) {
    long total = (long)linear_minutes(t) + n;
    long days = total / 1440;
    long rem = total % 1440;
    if (rem < 0) { rem += 1440; days--; }
    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)d;
    t->hour = (uint8_t)(rem / 60);
    t->min = (uint8_t)(rem % 60);
    t->sec = 0;
    t->ms = 0;
}

static void set_bcd(uint8_t *bits, unsigned first, unsigned count, unsigned value) {
    static const unsigned weights[8] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    for (unsigned i = count; i-- > 0;) {
        if (value >= weights[i]) { bits[first + i] = 1; value -= weights[i]; }
        else                       bits[first + i] = 0;
    }
}

static void set_even_parity(uint8_t *bits, unsigned first, unsigned last) {
    unsigned ones = 0;
    for (unsigned i = first; i < last; i++) ones += bits[i] ? 1u : 0u;
    bits[last] = (uint8_t)(ones & 1u);
}

/* Build the 59 bits a transmitter would send to announce `t`. */
static void build_frame(uint8_t *bits, const rtc_time_t *t) {
    memset(bits, 0, FRAME_BITS);
    bits[0] = 0;
    bits[17] = 1;  /* CEST; exactly one of Z1/Z2 must be set */
    bits[18] = 0;
    bits[20] = 1;
    set_bcd(bits, 21, 7, t->min);
    set_even_parity(bits, 21, 28);
    set_bcd(bits, 29, 6, t->hour);
    set_even_parity(bits, 29, 35);
    set_bcd(bits, 36, 6, t->day);
    set_bcd(bits, 42, 3, dcf77_weekday_from_date(t->year, t->month, t->day));
    set_bcd(bits, 45, 5, t->month);
    set_bcd(bits, 50, 8, (unsigned)(t->year % 100u));
    set_even_parity(bits, 36, 58);
}

#define SELFTEST_STEP_MS 5u

typedef enum {
    CORRUPT_NONE = 0,
    CORRUPT_PARITY,     /* flip a data bit, leave its parity bit alone */
    CORRUPT_DROP,       /* one second carries no pulse at all */
    CORRUPT_EXTRA,      /* a 20 ms spurious pulse -- below the debounce */
    CORRUPT_EXTRA_LONG, /* a 60 ms one -- above it, and still fatal */
    CORRUPT_WEEKDAY,    /* weekday field disagrees with the frame's own date */
} corrupt_t;

/* Feed one minute of samples: 59 pulses at one-second spacing, 100 ms for a
 * 0 and 200 ms for a 1, then a silent second 59. Returns the timestamp one
 * minute later. */
static uint64_t feed_frame(dcf77_t *d, uint64_t t0, const uint8_t *bits,
                           bool inverted, corrupt_t how) {
    for (uint64_t off = 0; off < 60000u; off += SELFTEST_STEP_MS) {
        uint64_t now = t0 + off;
        unsigned sec = (unsigned)(off / 1000u);
        unsigned in_sec = (unsigned)(off % 1000u);

        bool active = false;
        if (sec < 59u && !(how == CORRUPT_DROP && sec == 30u)) {
            unsigned width = bits[sec] ? 200u : 100u;
            active = (in_sec < width);
        }
        if (how == CORRUPT_EXTRA && sec == 30u && in_sec >= 500u && in_sec < 520u) {
            active = true;
        }
        if (how == CORRUPT_EXTRA_LONG && sec == 30u && in_sec >= 500u && in_sec < 560u) {
            active = true;
        }

        dcf77_feed(d, inverted ? !active : active, now);
    }
    return t0 + 60000u;
}

static bool times_equal(const rtc_time_t *a, const rtc_time_t *b) {
    return a->year == b->year && a->month == b->month && a->day == b->day &&
           a->hour == b->hour && a->min == b->min;
}

/* One case: feed `frames` consecutive minutes starting at `start`, applying
 * `how` to frame index `corrupt_at`, and check what came out. `expect_time`
 * of NULL means "nothing should be accepted". */
static bool run_case(const char *name, rtc_time_t start, int frames,
                     bool inverted, int corrupt_at, corrupt_t how,
                     int skip_at, const rtc_time_t *expect_time) {
    dcf77_t d;
    /* The decoder is told the polarity, exactly as the board file will tell
     * it -- there is nothing to detect and nothing to get wrong on a weak
     * signal. `inverted` here stands for the module's polarity jumper. */
    dcf77_reset(&d, !inverted);

    rtc_time_t t = start;
    uint64_t now = 1000;
    uint8_t bits[FRAME_BITS];

    for (int i = 0; i < frames; i++) {
        build_frame(bits, &t);
        if (i == corrupt_at) {
            if (how == CORRUPT_PARITY) {
                bits[21] = (uint8_t)!bits[21];      /* minute LSB, parity left stale */
            } else if (how == CORRUPT_WEEKDAY) {
                unsigned wd = bcd(bits, 42, 3);
                set_bcd(bits, 42, 3, (wd % 7u) + 1u);
                set_even_parity(bits, 36, 58);       /* parity still correct */
            }
        }
        now = feed_frame(&d, now, bits,
                         inverted,
                         (i == corrupt_at) ? how : CORRUPT_NONE);
        add_minutes(&t, (i == skip_at) ? 5 : 1);
    }

    /* Zeroed, not merely declared: dcf77_take_time() fills it only when it
     * returns true, and the "expected nothing, got ..." branch below prints it
     * either way -- the same reason the timezone case further down zeroes its
     * own. Latent until P1 added a branch that made the flow visible to the
     * compiler. */
    rtc_time_t got = {0};
    uint64_t   mark_ms = 0;
    bool have = dcf77_take_time(&d, &got, &mark_ms);

    bool ok;
    if (expect_time) ok = have && times_equal(&got, expect_time);
    else             ok = !have;
    /* P1: a time without the instant it was true is half an answer, and the
     * instant has to be the mark rather than anything the caller could read
     * for itself. `now` here is where the synthetic stream has got to, which
     * is at least a debounce past the mark -- so a mark that equals it, or
     * exceeds it, means the decoder handed back its caller's clock instead of
     * its own measurement. */
    if (ok && expect_time && (mark_ms == 0 || mark_ms >= now)) {
        ok = false;
        cprintf("  (mark %lu is not before now %lu) ",
                (unsigned long)mark_ms, (unsigned long)now);
    }

    cprintf("  %s %s", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        if (expect_time && !have) {
            cprintf(" (expected %u-%02u-%02u %02u:%02u, got nothing)",
                    expect_time->year, expect_time->month, expect_time->day,
                    expect_time->hour, expect_time->min);
        } else if (expect_time) {
            cprintf(" (expected %u-%02u-%02u %02u:%02u, got %u-%02u-%02u %02u:%02u)",
                    expect_time->year, expect_time->month, expect_time->day,
                    expect_time->hour, expect_time->min,
                    got.year, got.month, got.day, got.hour, got.min);
        } else {
            cprintf(" (expected nothing, got %u-%02u-%02u %02u:%02u)",
                    got.year, got.month, got.day, got.hour, got.min);
        }
    }
    cprintf("\n");
    return ok;
}

/* build_frame() always sets Z1 (CEST), so every expectation built from a
 * transmitted wall-clock time has to come back two hours earlier. */
static void as_utc_from_cest(rtc_time_t *t) {
    tz_from_offset(t, 120, t);
}

/* The one case build_frame() cannot express: a winter frame, where Z2 is set
 * and the same reading is UTC+1. Hand-rolled rather than threaded through
 * run_case(), because it is the only test that cares which Z bit is on. */
static bool run_cet_case(void) {
    rtc_time_t local = { 2026, 1, 15, 13, 35, 0, 0 };   /* CET, midwinter */
    rtc_time_t expect = local;
    add_minutes(&expect, 2);          /* the third frame is the accepted one */
    tz_from_offset(&expect, 60, &expect);

    dcf77_t d;
    dcf77_reset(&d, true);

    uint64_t t = 1000;
    rtc_time_t cur = local;
    for (int i = 0; i < 4; i++) {
        uint8_t bits[FRAME_BITS];
        build_frame(bits, &cur);
        bits[17] = 0;                 /* not CEST... */
        bits[18] = 1;                 /* ...but CET. Neither bit is covered by
                                       * any of the three parity fields. */
        t = feed_frame(&d, t, bits, false, CORRUPT_NONE);
        add_minutes(&cur, 1);
    }

    /* Zeroed, not merely declared: dcf77_take_time() fills it only when it
     * returns true, and the failure branch below prints it either way. */
    rtc_time_t got = {0};
    bool ok = dcf77_take_time(&d, &got, NULL) && times_equal(&got, &expect);
    cprintf("  [%s] a CET frame decodes to UTC+1, not UTC+2", ok ? "ok" : "FAIL");
    if (!ok) cprintf(" (got %04u-%02u-%02u %02u:%02u)", got.year, got.month,
                     got.day, got.hour, got.min);
    cprintf("\n");
    return ok;
}

int dcf77_selftest(void) {
    int failures = 0;

    cprintf("DCF-77 decoder selftest:\n");

    /* The accepted time is the THIRD frame's: the first is missed (its start
     * was never seen), the second establishes the pair's first half, and the
     * third is the one that has to agree with it. */
    rtc_time_t base = { 2026, 8, 22, 14, 35, 0, 0 };
    rtc_time_t expect = base;
    add_minutes(&expect, 2);
    as_utc_from_cest(&expect);
    if (!run_case("clean frames decode", base, 4, false, -1, CORRUPT_NONE, -1, &expect)) failures++;

    if (!run_case("inverted output decodes (polarity configured, not guessed)",
                  base, 4, true, -1, CORRUPT_NONE, -1, &expect)) failures++;

    if (!run_case("stale parity is rejected", base, 4, false, 2, CORRUPT_PARITY, -1, NULL)) failures++;
    if (!run_case("dropped pulse is rejected", base, 4, false, 2, CORRUPT_DROP, -1, NULL)) failures++;
    /* The pair that encodes the hardware finding of 2026-08-23. A 20 ms
     * spurious pulse is what a running LED panel injects, and it used to
     * destroy the frame; it is now below the debounce and the minute survives
     * it intact. A 60 ms one is above the debounce and still fatal, which is
     * the honest limit of a filter that cannot tell a short pulse from a
     * short burst of noise. */
    if (!run_case("a 20 ms spurious pulse is filtered out and the frame survives",
                  base, 4, false, 2, CORRUPT_EXTRA, -1, &expect)) failures++;
    if (!run_case("a 60 ms spurious pulse still costs the frame",
                  base, 4, false, 2, CORRUPT_EXTRA_LONG, -1, NULL)) failures++;
    if (!run_case("weekday disagreeing with date is rejected",
                  base, 4, false, 2, CORRUPT_WEEKDAY, -1, NULL)) failures++;

    /* Two frames that each decode perfectly but are not one minute apart must
     * not be believed -- the whole point of the two-frame rule. */
    if (!run_case("non-consecutive frames are rejected",
                  base, 4, false, -1, CORRUPT_NONE, 1, NULL)) failures++;

    /* The case that matters for marginal reception, and the reason frames no
     * longer have to be adjacent: a bad minute between two good ones must not
     * cost the pair. Frame 1 decodes, frame 2 is corrupt, frame 3 decodes two
     * minutes later -- and our own clock agrees it has been two minutes. */
    rtc_time_t gap_expect = base;
    add_minutes(&gap_expect, 3);
    as_utc_from_cest(&gap_expect);
    if (!run_case("frames two minutes apart still pair (bad minute between)",
                  base, 5, false, 2, CORRUPT_PARITY, -1, &gap_expect)) failures++;

    /* Year, month and day rollover in one step, which is where hand-rolled
     * date arithmetic usually breaks. */
    rtc_time_t nye = { 2026, 12, 31, 23, 58, 0, 0 };
    rtc_time_t nye_expect = nye;
    add_minutes(&nye_expect, 2);   /* 2027-01-01 00:00 */
    as_utc_from_cest(&nye_expect);
    if (!run_case("year boundary decodes", nye, 4, false, -1, CORRUPT_NONE, -1, &nye_expect)) failures++;

    /* A leap day, for the same reason. */
    rtc_time_t leap = { 2028, 2, 28, 23, 58, 0, 0 };
    rtc_time_t leap_expect = leap;
    add_minutes(&leap_expect, 2);  /* 2028-02-29 00:00 */
    as_utc_from_cest(&leap_expect);
    if (!run_case("leap day decodes", leap, 4, false, -1, CORRUPT_NONE, -1, &leap_expect)) failures++;

    if (!run_cet_case()) failures++;

    /* The score has to be a *meter*, not a light. Three synthetic pulses,
     * identical except for how noisy the line is around them, must come back
     * with three different scores -- otherwise a signal monitor cannot show
     * an antenna getting better before it starts working. */
    {
        static const struct { uint32_t gap_err, width_err, glitches; uint8_t want; } G[] = {
            {  0,   0,  0, 7 },  /* textbook */
            {  0,   0,  2, 7 },  /* an edge that bounces twice is still clean */
            {  0,  30,  0, 6 },  /* a pulse 30 ms off nominal */
            {  0,   0,  4, 6 },  /* the line starting to pick up noise */
            {  0,   0,  9, 5 },
            {  0,   0, 15, 4 },
            {  0,   0, 30, 3 },  /* very noisy, but the pulse still lands right */
            { 60,  60,  1, 3 },  /* wandering in both width and spacing */
            { 1000, 1000, 30, 1 }, /* saturated: still 1, never 0 */
        };
        bool ok = true;
        for (unsigned i = 0; i < sizeof(G) / sizeof(G[0]); i++) {
            uint8_t got = grade(G[i].gap_err, G[i].width_err, G[i].glitches);
            if (got != G[i].want) {
                cprintf("  score(gap=%u width=%u glitch=%u) = %u, wanted %u\n",
                        (unsigned)G[i].gap_err, (unsigned)G[i].width_err,
                        (unsigned)G[i].glitches, got, G[i].want);
                ok = false;
            }
        }
        cprintf("  [%s] the quality score grades, rather than passing or failing\n",
                ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    if (failures == 0) cprintf("DCF77_SELFTEST_OK (13/13)\n");
    else               cprintf("DCF77_SELFTEST_FAIL (%d failed)\n", failures);
    return failures;
}
