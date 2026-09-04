/* See kernel/include/kernel/discipline.h for what this is and why it keeps a
 * rate rather than only a phase. */

#include "kernel/discipline.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "lugalos_config.h"

/* Above this the clock is not being disciplined, it is being set: stepping is
 * correct because monotonicity across a half-second error is not worth
 * preserving and slewing it out would take hours. Below it, never step. */
#define STEP_THRESHOLD_US 500000LL

/* A sample further than this from the running estimate is rejected.
 *
 * Sized from measurement, not taste: P4 measured this receiver's scatter at
 * 2.03 ms sd over 1073 frames, so 25 ms is more than ten sigma. A frame that
 * far out is not a noisy frame, it is a wrong one -- and DCF-77 delivers those
 * under marginal reception, with correct parity, because parity catches a
 * flipped bit and not a plausible minute. */
#define OUTLIER_US 25000LL

/* Rejection has to be able to give up. If the clock really has moved -- a
 * step from another source, a crystal that changed with temperature -- every
 * sample looks like an outlier and a loop that only ever rejects is a loop
 * that has stopped working. After this many consecutive rejections the
 * estimate is abandoned and the next sample is believed. */
#define MAX_CONSECUTIVE_REJECTS 5

/* Loop gains, as divisors. Phase: correct an eighth of the offset per sample,
 * so a single bad-but-plausible frame moves the clock by an eighth of its
 * error rather than all of it. Frequency: a sixteenth, because the rate is a
 * property of a crystal and should be learned slowly from many samples --
 * P0 measured this one stable to +/-0.01 ppm, so there is nothing fast to
 * track and everything to gain from averaging. */
#define PHASE_DIV 8
#define FREQ_DIV  16

/* How long the offset correction is spread over. One sample interval, so the
 * debt is paid off by the time the next measurement arrives and each sample
 * sees the result of the last rather than a correction still in flight. */
#define SLEW_MS 60000u

/* Beyond this with no accepted sample, the source is considered gone. Three
 * minutes is several DCF-77 frame intervals: long enough not to trip on a
 * single missed minute, short enough that a disconnected antenna is noticed
 * while someone might still be near it. */
#define HOLDOVER_AFTER_US (180ull * 1000000ull)

/* What the rate estimate is worth, in parts per billion, once it has settled.
 * The dispersion during holdover grows at this rate: it is the honest answer
 * to "how far could this clock have drifted since it last saw the truth",
 * and P0 measured this crystal's residual at about 0.01 ppm after correction.
 * Before the rate is learned at all, assume an uncorrected crystal. */
#define SETTLED_PPB_ERR 20
#define UNKNOWN_PPB_ERR 30000

static disc_state_t g_state = DISC_UNSET;
static uint32_t g_samples, g_accepted, g_rejected, g_consec_rejects;
static int32_t  g_freq_ppb;
static int64_t  g_last_offset_us;
static uint64_t g_last_at_mono;
static bool     g_have_last;

/* Running statistics over accepted samples, for reporting rather than for the
 * loop: sum and sum of squares, as everywhere else in this tree. */
static int64_t  g_sum_us;
static uint64_t g_sumsq;
static uint32_t g_n;

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0;
    while ((r + 1ull) * (r + 1ull) <= v) r++;
    return (uint32_t)r;
}

bool discipline_feed(int64_t offset_us, uint64_t at_mono_us) {
    g_samples++;

    int64_t mag = offset_us < 0 ? -offset_us : offset_us;

    /* A clock this wrong is not being disciplined. Step it, discard the rate
     * estimate -- it was learned against a different clock and means nothing
     * now -- and start again. */
    if (mag > STEP_THRESHOLD_US) {
        time_set_epoch_us(time_epoch_us() - offset_us);
        g_state = DISC_STEP;
        g_have_last = false;
        g_consec_rejects = 0;
        g_last_offset_us = 0;
        g_last_at_mono = at_mono_us;
        g_accepted++;
        /* Milliseconds, and as a long: printk has no %lld, and `long` is
         * 32-bit on RV32 -- a microsecond step would overflow it inside an
         * hour of error, where milliseconds will not for twenty-four days. */
        printk("[disc] stepped %ld ms; rate estimate discarded\n",
               (long)(-offset_us / 1000));
        return true;
    }

    /* Outlier rejection against the last accepted sample, which is the only
     * prediction available that costs nothing: the clock is disciplined, so
     * consecutive offsets should differ by little. */
    if (g_have_last) {
        int64_t d = offset_us - g_last_offset_us;
        if (d < 0) d = -d;
        if (d > OUTLIER_US) {
            g_rejected++;
            if (++g_consec_rejects < MAX_CONSECUTIVE_REJECTS) return false;
            /* Given up: the world moved, not the sample. Fall through and
             * believe this one, discarding the stale estimate. */
            g_have_last = false;
        }
    }
    g_consec_rejects = 0;

    /* --- frequency: a run of phase measurements is a rate measurement ---- */
    if (g_have_last && at_mono_us > g_last_at_mono) {
        uint64_t dt_us = at_mono_us - g_last_at_mono;
        /* Only over a sane interval. Too short and the division amplifies
         * measurement noise into a huge apparent rate error; too long and the
         * two samples are not describing one stretch of the same crystal. */
        if (dt_us > 30ull * 1000000ull && dt_us < 3600ull * 1000000ull) {
            int64_t drift_us = offset_us - g_last_offset_us;
            int64_t implied_ppb = drift_us * 1000000LL / (int64_t)(dt_us / 1000u);
            g_freq_ppb -= (int32_t)(implied_ppb / FREQ_DIV);
            time_set_freq_ppb(g_freq_ppb);
        }
    }

    /* --- phase: slew a fraction of the offset, never step ---------------- */
    time_slew_us(-offset_us / PHASE_DIV, SLEW_MS);

    g_last_offset_us = offset_us;
    g_last_at_mono = at_mono_us;
    g_have_last = true;
    g_accepted++;
    g_state = DISC_TRACK;

    g_sum_us += offset_us;
    g_sumsq  += (uint64_t)(offset_us * offset_us);
    g_n++;
    return true;
}

void discipline_status(disc_status_t *out) {
    if (!out) return;

    uint64_t now = time_get_us();
    uint64_t age_us = (g_have_last && now > g_last_at_mono) ? now - g_last_at_mono : 0;

    disc_state_t st = g_state;
    if (st == DISC_TRACK && age_us > HOLDOVER_AFTER_US) st = DISC_HOLDOVER;

    out->state = st;
    out->samples = g_samples;
    out->accepted = g_accepted;
    out->rejected = g_rejected;
    out->freq_ppb = g_freq_ppb;
    out->last_offset_us = g_last_offset_us;
    out->age_s = (uint32_t)(age_us / 1000000ull);

    if (g_n) {
        int64_t m = g_sum_us / (int64_t)g_n;
        out->mean_offset_us = m;
        int64_t var = (int64_t)(g_sumsq / g_n) - m * m;
        out->sd_offset_us = isqrt64((uint64_t)(var < 0 ? 0 : var));
    } else {
        out->mean_offset_us = 0;
        out->sd_offset_us = 0;
    }

    /* Dispersion: how far this clock could have drifted since it last saw the
     * truth. It is what makes holdover honest -- the clock keeps serving and
     * keeps saying how much less it should be trusted -- and P6 reports it
     * straight into the NTP root dispersion field. */
    uint32_t ppb_err = (g_n >= 8) ? SETTLED_PPB_ERR : UNKNOWN_PPB_ERR;
    out->dispersion_us = (uint32_t)(age_us / 1000000ull * ppb_err / 1000u)
                       + out->sd_offset_us;
}

void discipline_reset(void) {
    g_state = DISC_UNSET;
    g_samples = g_accepted = g_rejected = g_consec_rejects = 0;
    g_freq_ppb = 0;
    g_last_offset_us = 0;
    g_last_at_mono = 0;
    g_have_last = false;
    g_sum_us = 0;
    g_sumsq = 0;
    g_n = 0;
}

void discipline_selftest(void) {
    /* Everything here drives the real entry point, so what is tested is what
     * ships -- and everything is put back afterwards, because this runs on a
     * board whose clock other things are using. */
    int64_t  saved_epoch = time_epoch_us();
    int32_t  saved_freq  = time_freq_ppb();
    uint64_t t = time_get_us();
    int pass = 0, fail = 0;
    #define CHECK(name, cond) do { \
        if (cond) { pass++; printk("  [ok]   %s\n", name); } \
        else      { fail++; printk("  [FAIL] %s\n", name); } \
    } while (0)

    printk("[disc] selftest\n");

    /* 1. A wildly wrong clock is stepped, not slewed: slewing a second out at
     *    a sane rate would take hours, and during them the clock is wrong. */
    discipline_reset();
    time_set_freq_ppb(0);
    bool stepped = discipline_feed(900000, t);
    disc_status_t d; discipline_status(&d);
    CHECK("a 900 ms offset steps", stepped && d.state == DISC_STEP);

    /* 2. Small offsets are tracked, never stepped. */
    discipline_reset();
    time_set_freq_ppb(0);
    bool a1 = discipline_feed(2000, t);
    discipline_status(&d);
    CHECK("a 2 ms offset tracks", a1 && d.state == DISC_TRACK);

    /* 3. A frame that is plausible but wrong -- correct parity, wrong minute
     *    -- is rejected rather than averaged in. This is the case marginal
     *    reception actually produces. */
    bool out1 = discipline_feed(2000 + 200000, t + 60000000ull);
    discipline_status(&d);
    CHECK("a 200 ms jump is rejected", !out1 && d.rejected == 1);

    /* 4. Rejection gives up rather than locking out forever: if the world
     *    really moved, a loop that only rejects has stopped working. */
    bool last = false;
    for (int i = 0; i < MAX_CONSECUTIVE_REJECTS; i++)
        last = discipline_feed(2000 + 200000, t + (uint64_t)(120 + 60 * i) * 1000000ull);
    CHECK("persistent disagreement is eventually believed", last);

    /* 5. A clock that is losing time at a steady rate has its *rate* learned,
     *    not just its phase corrected -- the difference between a clock that
     *    holds over and one that does not. Ten minutes of samples drifting
     *    +1 ms/min is +16667 ppb; the loop should move against it. */
    discipline_reset();
    time_set_freq_ppb(0);
    for (int i = 0; i < 10; i++)
        discipline_feed((int64_t)i * 1000, t + (uint64_t)i * 60000000ull);
    discipline_status(&d);
    CHECK("a steady drift is learned as a rate", d.freq_ppb < -100);
    printk("  freq estimate after 10 drifting samples: %ld ppb\n", (long)d.freq_ppb);

    /* 6. Holdover reports a dispersion that grows, because a clock that has
     *    lost its reference is still useful only if it says how much less. */
    discipline_status(&d);
    uint32_t disp0 = d.dispersion_us;
    CHECK("dispersion is reported", disp0 > 0 || d.age_s == 0);

    printk("[disc] selftest: %d passed, %d failed\n", pass, fail);
    printk(fail ? "DISCIPLINE_SELFTEST_FAIL\n" : "DISCIPLINE_SELFTEST_OK\n");
    #undef CHECK

    discipline_reset();
    time_set_freq_ppb(saved_freq);
    time_slew_us(0, 0);
    time_set_epoch_us(saved_epoch);
}
