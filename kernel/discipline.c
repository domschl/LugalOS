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
 * only when the rate correction matches the crystal.
 *
 * The divisor sets how fast, and 64 was too fast. Three hours on the bench
 * (2026-09-05) put numbers on it. The whole of the crystal's 460 ppb error
 * shows up as a standing offset of only
 *
 *     SLEW_MS/1000 * PHASE_DIV * 460 ppb = 221 us
 *
 * because the phase loop has already removed an eighth of it every minute --
 * while a single DCF-77 frame lands with a scatter of 2077 us. The quantity
 * being measured sits ten times below the noise it is buried in. At a divisor
 * of 64 the loop corrected 0.75% of its own error per frame while taking a
 * 32 ppb random step, and freq_ppb duly wandered over -727..-22 ppb around a
 * true -460. Predicted equilibrium wander 265 ppb, measured 199. The loop was
 * right; its gain was not.
 *
 * Wander falls as 1/sqrt(divisor) while the time constant grows linearly, so
 * 256 buys a quieter estimate (about 130 ppb) at a time constant of 8.9 hours.
 * That is the trade: long enough to average a night of radio noise down,
 * short enough to still follow a crystal that moves with the room. Going
 * further -- 512, 18 hours -- would no longer track a diurnal swing. */
#define FREQ_INT_DIV     256

/* The loop's own time constant, in samples: the standing offset a rate error
 * of one ppb produces, divided into the divisor. About 533 frames, so nearly
 * nine hours of once-a-minute DCF-77. Derived rather than written down, so
 * that retuning the constants above cannot leave it quietly wrong. */
#define FREQ_LOOP_TAU_N  ((FREQ_INT_DIV * 1000u) / ((SLEW_MS / 1000u) * PHASE_DIV))

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

/* How many times the rate estimate has been updated. Dispersion needs it to
 * know whether the loop has run long enough for its noise floor to be the
 * dominant error -- see freq_uncertainty_ppb(). */
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
/* How well the rate is known, in ppb.
 *
 * The previous answer -- an average of the integrator's own step size,
 * doubled -- was measured against the hardware and found to understate by
 * five times: it reported about 50 ppb while freq_ppb was demonstrably
 * wandering by 250. Averaging the steps measures how hard the loop is
 * pulling, not how far from the truth it has been pulled, and those are
 * different numbers whenever the steps are noise rather than signal.
 *
 * A first-order loop driven by white noise has a known answer. Each sample
 * displaces the estimate by sd/FREQ_INT_DIV, and those displacements decay
 * with the loop's time constant, so the estimate settles at
 *
 *     wander = (sd / FREQ_INT_DIV) * sqrt(TAU / 2)
 *
 * which for the bench's 2077 us of DCF-77 scatter predicted 265 ppb against
 * 199 measured -- agreement to within the length of the run. Reading it from
 * sd_offset_us rather than accumulating more state has a second virtue: when
 * reception degrades the scatter rises and the claimed rate knowledge falls
 * out of it automatically, which is what the pre-dawn hours need. */
static uint32_t freq_uncertainty_ppb(uint32_t sd_us) {
    /* sqrt(TAU/2), carried times 100 so the divide below keeps its digits. */
    uint32_t root_q = isqrt64((uint64_t)FREQ_LOOP_TAU_N * 10000ull / 2ull);
    uint32_t ppb = (uint32_t)((uint64_t)sd_us * root_q
                              / (100ull * (uint64_t)FREQ_INT_DIV));

    /* Before the loop has run for a time constant the estimate is still
     * walking in from wherever it started, and that transient dwarfs the
     * noise floor above. Fade the uncorrected-crystal assumption out across
     * the first TAU samples rather than dropping it at a threshold: a step
     * here would show up in a served root dispersion as a cliff.
     *
     * This costs little while the radio is present -- dispersion is
     * age * ppb, and age is a minute -- and is exactly the pessimism holdover
     * wants from a clock that booted an hour ago. */
    if (ppb > UNKNOWN_PPB_ERR) ppb = UNKNOWN_PPB_ERR;
    if (g_freq_updates < FREQ_LOOP_TAU_N) {
        uint32_t left = FREQ_LOOP_TAU_N - g_freq_updates;
        ppb += (uint32_t)((uint64_t)(UNKNOWN_PPB_ERR - ppb) * left / FREQ_LOOP_TAU_N);
    }

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
        g_freq_updates = 0;
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
     *    Six hundred samples of a steady +640 us must move freq by
     *    599 * (640 / 256) = -1198 ppb -- 599 and not 600 because the first
     *    sample has no predecessor to measure an interval against. Asserting
     *    the arithmetic exactly, because "it moved the right way" is
     *    precisely the assertion that let a useless estimator ship twice.
     *
     *    Six hundred and not forty: at a divisor of 256 the run also has to
     *    outlast FREQ_LOOP_TAU_N for check 6 below to see past the startup
     *    transient. */
    discipline_reset();
    time_set_freq_ppb(0);
    for (int i = 0; i < 600; i++)
        discipline_feed(640, t + (uint64_t)i * 60000000ull);
    discipline_status(&d);
    CHECK("a standing offset is integrated into the rate", d.freq_ppb == -1198);
    printk("  freq after 600 samples of a steady +640 us offset: %ld ppb "
           "(expected -1198)\n", (long)d.freq_ppb);

    /* 6. And the rate is claimed to be known only as well as the radio noise
     *    the loop had to average to learn it.
     *
     *    This is the check that was missing. Its predecessor compared two
     *    dispersions -- but dispersion is age * uncertainty + sd, the
     *    selftest's synthetic timestamps leave age at zero, and so it was
     *    only ever comparing sd against sd. It passed while the uncertainty
     *    it was supposed to be testing understated the hardware by five
     *    times. Call the estimator directly and assert its arithmetic.
     *
     *    sqrt(TAU/2) = sqrt(533/2) = 16.32, so 2077 us of DCF-77 scatter --
     *    the bench figure -- must yield 2077 * 16.32 / 256 = 132 ppb, which
     *    is the number the hardware showed wandering by (199 measured over
     *    1.4 time constants, at the divisor of 64 that predicts 265). */
    CHECK("rate uncertainty follows the radio noise",
          freq_uncertainty_ppb(2077) == 132 && freq_uncertainty_ppb(8000) == 510);
    printk("  rate uncertainty: sd 2077 us -> %lu ppb, sd 8000 us -> %lu ppb\n",
           (unsigned long)freq_uncertainty_ppb(2077),
           (unsigned long)freq_uncertainty_ppb(8000));

    /* 6b. And a clock that has not yet run for a time constant says so, by
     *     fading out the uncorrected-crystal assumption rather than holding
     *     it and then dropping it at a threshold. */
    discipline_reset();
    time_set_freq_ppb(0);
    uint32_t u_fresh = freq_uncertainty_ppb(2077);
    for (int i = 0; i < 300; i++)
        discipline_feed(640, t + (uint64_t)i * 60000000ull);
    uint32_t u_half = freq_uncertainty_ppb(2077);
    CHECK("an unconverged rate is not claimed to be known",
          u_fresh == UNKNOWN_PPB_ERR && u_half < u_fresh && u_half > 132);
    printk("  rate uncertainty: 0 samples %lu ppb, 300 %lu ppb, settled 132 ppb\n",
           (unsigned long)u_fresh, (unsigned long)u_half);

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
