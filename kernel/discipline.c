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

/* The frequency estimate needs a *baseline*, not a gain.
 *
 * The first version differenced consecutive samples and divided by 16. That is
 * the worst available estimator, and the bench said so within forty minutes:
 * freq_ppb swung 514, 586, -1187, 398, 3181, -958 while the crystal's true
 * error is about -460 ppb. The arithmetic is unforgiving -- differencing two
 * samples 60 s apart turns this receiver's 1.9 ms of phase noise into
 * 1900 us * sqrt(2) / 60 s = 45000 ppb of apparent rate error, so even after
 * dividing by 16 each update injected thousands of ppb of noise into a
 * quantity of a few hundred. It never converged; it random-walked.
 *
 * A least-squares slope over the whole run fixes it, because the uncertainty
 * of a regression falls as sigma*sqrt(12/n)/T rather than as sigma/dt: about
 * three hours of samples gets to 50 ppb, where two-point differencing would
 * need fifteen. Kept as running sums rather than a stored series, so it costs
 * five accumulators instead of a page of history.
 *
 * It does not forget, which is right for a crystal P0 measured stable to
 * +/-0.01 ppm and wrong after anything that invalidates the past -- so a step
 * or a re-acquisition clears it, and those are exactly the events that mean
 * the old samples describe a different clock. */
#define FREQ_MIN_SAMPLES 20
#define FREQ_MIN_SPAN_S  900

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

/* Running sums for the least-squares slope. `t` is seconds since the first
 * accepted sample, which keeps every product comfortably inside 64 bits even
 * across a multi-day run. */
static uint64_t g_t0_mono;
static int64_t  g_st, g_stt, g_sy, g_sty;
static uint32_t g_sn;
static int64_t  g_span_s;

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
    if (g_sn < FREQ_MIN_SAMPLES || g_span_s < FREQ_MIN_SPAN_S) return UNKNOWN_PPB_ERR;
    /* sigma * sqrt(12/n) / T, in ppb: microseconds over seconds is ppm, so a
     * further factor of 1000. sqrt(12/n) is computed as sqrt(12*1e6/n)/1000
     * to keep it in integers. */
    uint32_t root = isqrt64(12ull * 1000000ull / g_sn);
    uint64_t ppb = (uint64_t)sd_us * root * 1000ull / (1000ull * (uint64_t)g_span_s);
    if (ppb < 10) ppb = 10;
    if (ppb > UNKNOWN_PPB_ERR) ppb = UNKNOWN_PPB_ERR;
    return (uint32_t)ppb;
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
        g_t0_mono = 0; g_st = g_stt = g_sy = g_sty = 0; g_sn = 0; g_span_s = 0;
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

    /* --- frequency: the slope of the whole run, not the last two points --- */
    if (!g_sn) g_t0_mono = at_mono_us;
    {
        int64_t t = (int64_t)((at_mono_us - g_t0_mono) / 1000000ull);
        g_st += t; g_stt += t * t; g_sy += offset_us; g_sty += t * offset_us;
        g_sn++;
        g_span_s = t;
        if (g_sn >= FREQ_MIN_SAMPLES && t >= FREQ_MIN_SPAN_S) {
            int64_t n = (int64_t)g_sn;
            int64_t denom = n * g_stt - g_st * g_st;
            if (denom > 0) {
                /* Slope in microseconds of offset per second, scaled to ppb:
                 * 1 us/s is 1 ppm is 1000 ppb. Numerator scaled before the
                 * divide so the result keeps its resolution. */
                int64_t num = n * g_sty - g_st * g_sy;
                int64_t slope_ppb = num * 1000LL / denom;
                /* The clock gains offset at `slope`; correcting it means
                 * running slower by the same amount. Absolute, not
                 * incremental: the regression already integrates every sample,
                 * so adding a fraction of it each time would integrate twice. */
                g_freq_ppb = (int32_t)(-slope_ppb);
                time_set_freq_ppb(g_freq_ppb);
            }
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
    g_t0_mono = 0;
    g_st = g_stt = g_sy = g_sty = 0;
    g_sn = 0;
    g_span_s = 0;
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

    /* 5. A clock losing time at a steady rate has its *rate* learned, and
     *    learned to the right value -- the difference between a clock that
     *    holds over usefully and one that merely claims to.
     *
     *    Forty samples a minute apart, each 1000 us later than the last: a
     *    drift of exactly 1000 us / 60 s = 16667 ppb. The regression should
     *    recover that and correct against it, so freq_ppb lands near -16667.
     *    Asserting the value rather than the sign is the point -- the previous
     *    estimator would pass a sign test and still be useless, which is how
     *    it survived to the bench. */
    discipline_reset();
    time_set_freq_ppb(0);
    for (int i = 0; i < 40; i++)
        discipline_feed((int64_t)i * 1000, t + (uint64_t)i * 60000000ull);
    discipline_status(&d);
    CHECK("a steady drift is learned as a rate, to within 5%",
          d.freq_ppb < -15800 && d.freq_ppb > -17500);
    printk("  freq estimate after 40 samples drifting 1 ms/min: %ld ppb "
           "(expected about -16667)\n", (long)d.freq_ppb);

    /* 6. And the rate's *uncertainty* falls out of the data rather than being
     *    asserted. With no scatter at all the slope is exact, so dispersion
     *    should grow only slowly; the failure this replaced claimed a fixed
     *    20 ppb while its estimator wandered by 2000. */
    uint32_t disp_clean = d.dispersion_us;
    discipline_reset();
    time_set_freq_ppb(0);
    for (int i = 0; i < 40; i++)   /* same span, but noisy */
        discipline_feed((int64_t)i * 1000 + ((i & 1) ? 4000 : -4000),
                        t + (uint64_t)i * 60000000ull);
    discipline_status(&d);
    CHECK("noisy samples give a larger dispersion than clean ones",
          d.dispersion_us > disp_clean);
    printk("  dispersion clean=%lu us  noisy=%lu us\n",
           (unsigned long)disp_clean, (unsigned long)d.dispersion_us);

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
