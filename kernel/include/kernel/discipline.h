#ifndef KERNEL_DISCIPLINE_H
#define KERNEL_DISCIPLINE_H

/*
 * Disciplining the wall clock from a phase reference (P5,
 * plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * **Every accepted frame is a phase measurement; a run of them is a frequency
 * measurement.** That sentence is the whole design. A clock set once a night
 * from the radio is only ever as good as its last frame and spends the rest of
 * the day drifting; a clock that keeps a *rate* estimate learns what its
 * crystal does and stays right between frames -- including through a night
 * when no frame arrives at all.
 *
 * Two states, and both are needed. Phase alone chases every noisy sample and
 * never learns anything; frequency alone drifts without a way to correct the
 * accumulated error. So: a slew for the offset, an integrator for the rate.
 *
 * **Source-agnostic on purpose.** It takes an offset and the instant that
 * offset refers to, not a radio frame. DCF-77 is the first caller; P6's NTP
 * server needs the same clock, and an NTP *client* disciplining from the
 * network would feed the identical interface. Nothing here knows what a
 * longwave carrier is.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DISC_UNSET = 0,   /* nothing has ever been fed in                       */
    DISC_STEP,        /* the clock was so wrong it had to be stepped        */
    DISC_TRACK,       /* locked: slewing small offsets, learning the rate   */
    DISC_HOLDOVER,    /* the source is gone; coasting on the learned rate   */
} disc_state_t;

typedef struct {
    disc_state_t state;
    uint32_t samples;        /* fed in */
    uint32_t accepted;       /* survived outlier rejection */
    uint32_t rejected;
    int32_t  freq_ppb;       /* the rate correction currently applied */
    int64_t  last_offset_us; /* the most recent accepted measurement */
    int64_t  mean_offset_us; /* over the accepted ones */
    uint32_t sd_offset_us;
    uint32_t age_s;          /* since the last accepted sample */

    /* How far the clock could have drifted since the last accepted sample,
     * given how well the rate is known. This is the number an NTP server has
     * to report as its dispersion, and the reason holdover is a state rather
     * than an absence: a clock that has lost its source is still useful, but
     * only if it says how much less useful it is getting. */
    uint32_t dispersion_us;
} disc_status_t;

/* Feeds one phase measurement: `offset_us` is how far the wall clock was
 * *ahead* of true time at the monotonic instant `at_mono_us`. Returns true if
 * the sample was used rather than rejected as an outlier.
 *
 * Marginal reception delivers occasional confidently wrong frames -- a parity
 * check catches a flipped bit, not a plausible minute -- and a naive mean is
 * exactly what those destroy. */
bool discipline_feed(int64_t offset_us, uint64_t at_mono_us);

void discipline_status(disc_status_t *out);

/* Forgets everything learned. For the selftest, which must not leave a
 * scripted rate estimate behind on a running clock, and for a caller that
 * knows the reference has changed underneath it. */
void discipline_reset(void);

/* `disciplineselftest`: the loop's own checks, on every target. */
void discipline_selftest(void);

#endif /* KERNEL_DISCIPLINE_H */
