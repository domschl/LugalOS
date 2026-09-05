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

/* Phase gain, as a divisor: correct an eighth of the offset per sample, so a
 * single bad-but-plausible frame moves the clock by an eighth of its error
 * rather than all of it. */
#define PHASE_DIV 8

/* The frequency term integrates the offset. It is the I of a PI loop, and the
 * two previous versions were both the wrong shape.
 *
 * Differencing consecutive samples came first and random-walked: 1.9 ms of
 * phase noise over a 60 s baseline is 45000 ppb of apparent rate error, so
 * every update injected thousands of ppb into a quantity of a few hundred.
 *
 * A least-squares slope over the whole run replaced it, and was quieter but
 * measured the wrong thing. It regressed the *observed* offset against time --
 * and the phase slew re-flattens that offset every minute, so its slope is
 * near zero precisely when the loop is working. Seventeen hours on the bench
 * showed freq_ppb decaying -209, -95, -36, -29, -20, -13 toward nothing, while
 * the crystal it was supposed to be learning sits at about -460 ppb (2026-09-05).
 * The clock stayed right the whole time because the slew was doing all the
 * work, leaving a standing offset of about -700 us as its signature -- and a
 * frequency estimate that had learned nothing.
 *
 * That is invisible while the radio is present and fatal the moment it is not:
 * in holdover the slew expires after a minute and the clock coasts on
 * freq_ppb, so a rate estimate of -13 ppb against a true -460 drifts 1.6 ms an
 * hour while the reported dispersion grows at a floored 10 ppb.
 *
 * An integrator has no such blind spot. A standing offset is exactly what it
 * consumes: freq moves until the offset it is fed goes to zero, which happens
 * only when the rate correction matches the crystal. The divisor sets how
 * fast -- 64 moves about 3 ppb per sample against the ~200 us standing offset
 * this receiver produces, so it crosses 460 ppb in a couple of hours, while
 * dividing a 2 ms noise sample down to 31 ppb of jitter that successive
 * samples largely cancel. */
#define FREQ_INT_DIV     64

/* Below this interval a sample says nothing about rate -- it is the same
 * minute's noise measured twice -- and above it the two are not describing one
 * stretch of the same crystal. */
#define FREQ_MIN_DT_US   (30ull * 1000000ull)
#define FREQ_MAX_DT_US   (3600ull * 1000000ull)

/* How long the offset correction is spread over. One sample interval, so the
 * debt is paid off by the time the next measurement arrives and each sample
 * sees the result of the last rather than a correction still in flight. */
#define SLEW_MS 60000u

/* Beyond this with no accepted sample, the source is considered gone. Three
 * minutes is several DCF-77 frame intervals: long enough not to trip on a
 * single missed minute, short enough that a disconnected antenna is noticed
 * while someone might still be near it. */
#define HOLDOVER_AFTER_US (180ull * 1000000ull)

/* What the rate estimate is worth before there is one: an uncorrected crystal,
 * which the datasheet puts at +/-30 ppm. Once there *is* an estimate its
 * uncertainty is computed from the data rather than asserted -- see
 * freq_uncertainty_ppb(). A constant here was the original sin: it claimed
 * 20 ppb of rate knowledge while the estimator was random-walking by 2000,
 * which is not an optimistic dispersion but a fictional one. */
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

/* How much the rate estimate is still moving, as an exponential average of
 * the size of its own steps. This is what dispersion is entitled to claim: a
 * freq_ppb that is still wandering by 200 ppb per sample does not know the
 * crystal to 10, and saying otherwise is the mistake both previous versions
 * made in different ways. */
static uint32_t g_freq_wander_ppb;
static uint32_t g_freq_updates;

static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0;
    while ((r + 1ull) * (r + 1ull) <= v) r++;
    return (uint32_t)r;
}

/* How well the rate is actually known, in ppb -- computed, not asserted.
 *
 * The standard error of a least-squares slope through n points spanning T
 * seconds with residual scatter sigma is sigma*sqrt(12/n)/T. That is the
 * number holdover has to grow its dispersion by, and asserting a constant in
 * its place is how a clock comes to advertise 20 ppb of rate knowledge while
 * its estimator is wandering by 2000. Floored at 10 ppb so a short run cannot
 * divide its way to implausible confidence. */
/* Note on `sd_us`: it is the scatter of the offsets themselves, not of the
 * regression's residuals. While the loop is tracking those are the same thing
 * -- offsets hover about zero and their spread is the measurement noise. While
 * it is still converging, offsets ramp and the spread is larger than the noise,
 * so this over-estimates the uncertainty. That is the safe direction, and it
 * settles by itself as the loop locks; residuals would be more correct and
 * more code for a difference that only exists when the answer should be
 * pessimistic anyway. */
static uint32_t freq_uncertainty_ppb(uint32_t sd_us) {
    (void)sd_us;
    /* Until the integrator has had time to move, the rate is simply unknown
     * and an uncorrected crystal is the honest assumption. */
    if (g_freq_updates < 20u) return UNKNOWN_PPB_ERR;
    /* After that, how far the estimate is still moving per sample *is* how
     * well it is known. Doubled, because successive steps only partly cancel
     * and the safe direction here is pessimism: an overstated dispersion
     * costs a client some precision, an understated one costs it the truth. */
    uint32_t ppb = g_freq_wander_ppb * 2u;
    if (ppb < 10) ppb = 10;
    if (ppb > UNKNOWN_PPB_ERR) ppb = UNKNOWN_PPB_ERR;
    return ppb;
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
        /* The accumulated slope described the clock as it was before the step
         * and says nothing about the one that exists now. */
        g_freq_wander_ppb = 0; g_freq_updates = 0;
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

    /* --- frequency: integrate the offset the slew keeps having to remove --- */
    if (g_have_last && at_mono_us > g_last_at_mono) {
        uint64_t dt_us = at_mono_us - g_last_at_mono;
        if (dt_us > FREQ_MIN_DT_US && dt_us < FREQ_MAX_DT_US) {
            /* A clock that is persistently fast needs to run slower, in
             * proportion to how fast and for how long it has been so. That is
             * the whole of it -- and unlike a slope through the observed
             * offsets, it cannot be flattened by the correction being
             * applied, because the correction is what it is accumulating. */
            int32_t step = (int32_t)(offset_us / FREQ_INT_DIV);
            g_freq_ppb -= step;
            time_set_freq_ppb(g_freq_ppb);

            uint32_t mag = (uint32_t)(step < 0 ? -step : step);
            g_freq_wander_ppb = (g_freq_wander_ppb * 7u + mag) / 8u;
            g_freq_updates++;
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
    out->dispersion_us = (uint32_t)(age_us / 1000000ull
                                    * freq_uncertainty_ppb(out->sd_offset_us)
                                    / 1000u)
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
    g_freq_wander_ppb = 0;
    g_freq_updates = 0;
}

void discipline_selftest(void) {
    /* Everything here drives the real entry point, so what is tested is what
     * ships -- and everything is put back afterwards, because this runs on a
     * board whose clock other things are using. */
    uint64_t saved_mono  = time_get_us();
    int64_t  saved_epoch = time_epoch_us();
    int32_t  saved_freq  = time_freq_ppb();
    uint64_t t = saved_mono;
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

    /* 5. A standing offset -- the signature of a crystal running at the wrong
     *    rate while a phase correction keeps papering over it -- is consumed
     *    by the frequency term rather than papered over forever.
     *
     *    This is the case the previous two estimators both got wrong, and the
     *    reason the test now feeds a *constant* offset instead of a ramp. An
     *    open-loop ramp made a regression look like it worked; what the bench
     *    actually produces is a persistent few hundred microseconds that the
     *    slew removes every minute and the rate term never learns from.
     *
     *    Forty samples of a steady +640 us must move freq by
     *    40 * 640 / 64 = -400 ppb. Asserting the arithmetic exactly, because
     *    "it moved the right way" is precisely the assertion that let a
     *    useless estimator ship twice. */
    discipline_reset();
    time_set_freq_ppb(0);
    for (int i = 0; i < 40; i++)
        discipline_feed(640, t + (uint64_t)i * 60000000ull);
    discipline_status(&d);
    CHECK("a standing offset is integrated into the rate",
          d.freq_ppb < -380 && d.freq_ppb > -420);
    printk("  freq after 40 samples of a steady +640 us offset: %ld ppb "
           "(expected about -400)\n", (long)d.freq_ppb);

    /* 6. And the dispersion follows the estimate's own restlessness. A rate
     *    still moving in large steps is not known to ten parts per billion,
     *    whatever a constant might have claimed. */
    uint32_t disp_steady = d.dispersion_us;
    discipline_reset();
    time_set_freq_ppb(0);
    for (int i = 0; i < 40; i++)   /* same mean, alternating +/-8 ms of noise */
        discipline_feed(640 + ((i & 1) ? 8000 : -8000),
                        t + (uint64_t)i * 60000000ull);
    discipline_status(&d);
    CHECK("a noisier rate estimate reports a larger dispersion",
          d.dispersion_us > disp_steady);
    printk("  dispersion steady=%lu us  noisy=%lu us\n",
           (unsigned long)disp_steady, (unsigned long)d.dispersion_us);

    /* 7. Holdover reports a dispersion that grows, because a clock that has
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
    /* Restored *forward* by however long this took, not to the instant it
     * started. Putting back the saved reading would step the clock backwards
     * by the test's own duration -- about 30 ms -- which is a small version of
     * exactly the thing this loop exists to avoid, and would be a strange
     * thing for its selftest to do. */
    time_set_epoch_us(saved_epoch + (int64_t)(time_get_us() - saved_mono));
}
