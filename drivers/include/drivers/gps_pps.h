/*
 * A GPS module read for its pulse per second (P3b,
 * plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * **A transfer standard, not a time source, and the distinction is the
 * phase.** This exists to calibrate the DCF-77 path: DCF says *which* second
 * it is, PPS says *exactly when* that second began, and the difference is the
 * receiver's group delay measured directly rather than fitted. Phase 24 §3.4
 * settles why the module is attached for the calibration and taken off
 * afterwards -- a board that depends on GPS is a satellite clock with a
 * longwave hobby, and the whole point of the phase is a clock that keeps time
 * when the network and the sky are both gone.
 *
 * **So nothing here sets a clock.** Not the kernel's, not the DS3231's. It
 * reads, timestamps and reports. The easiest way to honour a design
 * constraint is for the code that would violate it not to exist, so the
 * function that would do it is absent rather than guarded.
 *
 * **Two halves, and the second is not optional.** The pulse is what the
 * calibration needs; NMEA is what says the pulse can be trusted. A module
 * without a fix may still emit a pulse, and a *regular* pulse train is not
 * evidence of a *correct* one -- it can be a free-running oscillator with no
 * relationship to UTC at all. A front-panel LED is not a software gate. So
 * the sentences are parsed for fix quality and satellite count, and the UTC
 * second they carry cross-checks the second the pulse falls in.
 */

#ifndef DRIVERS_GPS_PPS_H
#define DRIVERS_GPS_PPS_H

#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     enabled;          /* built and initialised on this persona */

    /* --- NMEA --- */
    uint32_t sentences;        /* accepted, checksum verified */
    uint32_t bad_checksum;     /* seen and rejected -- a wiring/baud symptom */
    uint32_t bytes;            /* raw, so "nothing at all" is distinguishable
                                * from "noise that never forms a sentence" */
    bool     rx_muxed;         /* the RX pin took its UART function. Without
                                * this, a refused mux and an unplugged module
                                * both read as zero bytes and nothing tells
                                * them apart. */
    uint32_t rx_pad;           /* IO_BANK0 CTRL as it reads back, for the case
                                * where even that is not enough */
    /* Why the stream stopped, when it stops. A byte counter that freezes is
     * ambiguous on its own -- a module that lost power and a receiver that
     * wedged look exactly alike -- and the board doing this measurement is
     * usually on a windowsill rather than next to a console. rx_idle_ms turns
     * "stopped" into a duration; the error counters say whether we noticed
     * anything going wrong first. A rising rx_err_frame with sentences that
     * never parse is the wrong-baud signature; rx_err_break is the RX line
     * held low, which is what an unpowered module looks like. */
    uint32_t rx_idle_ms;       /* since the last byte; 0 if none ever */
    uint32_t rx_err_overrun;   /* we did not drain fast enough */
    uint32_t rx_err_break;
    uint32_t rx_err_parity;
    uint32_t rx_err_frame;
    uint32_t rx_fr;            /* UARTFR as it reads right now */
    uint32_t rx_cr;            /* UARTCR -- proves the receiver is still on */

    uint8_t  fix_quality;      /* GGA field 6: 0 = no fix, 1 = GPS, 2 = DGPS */
    uint8_t  satellites;
    bool     rmc_valid;        /* RMC status 'A' rather than 'V' */
    bool     have_utc;
    rtc_time_t utc;            /* from RMC: date and time together */

    /* --- PPS --- */
    uint32_t pps_count;        /* rising edges captured */
    uint64_t pps_last_us;      /* TIMER0 at the most recent one */
    uint32_t pps_interval_us;  /* between the last two; ~1000000 when locked */
    uint32_t pps_dropped;      /* edges the ring could not hold */
    bool     pps_stormed;      /* the pin is shut off right now for oscillating.
                                * Recoverable: it is retried on a doubling
                                * backoff (see edgecap.h), because a module
                                * emits ~1 kHz on TIMEPULSE until it locks. */
    uint32_t pps_storms;       /* how many times, ever */
    uint32_t pps_storm_rate;   /* edges/sec at the last trip. ~2 kHz is an
                                * unlocked module; tens of kHz is a floating
                                * pin. That distinction is the diagnosis. */
} gps_status_t;

/* Brings up the UART and registers the PPS pin with the edge capture. Safe on
 * a board that has neither -- it simply reports nothing thereafter. */
void gps_init(void);

/* Drains the UART and the PPS ring. Wants calling at least every ~20 ms: a
 * PL011 FIFO is 32 bytes and a GGA sentence is about seventy, so a slower
 * caller loses sentences to overrun rather than to anything interesting. */
void gps_poll(void);

void gps_status(gps_status_t *out);

/* True when the module is reporting a fix *and* its pulse is arriving at
 * roughly one second. Both, because either alone has been wrong: a fix with
 * no pulse is a wiring fault, and a pulse with no fix is exactly the
 * free-running case the NMEA half exists to catch. This is the gate a
 * calibration should consult before believing anything. */
bool gps_pps_trustworthy(void);

#endif /* DRIVERS_GPS_PPS_H */
