/*
 * P0, plan/phase24_dcf77_precision_and_ntp_server.md: the instrument that
 * measures the DCF-77 path before anything about it is changed.
 *
 * Two numbers have to exist before phase 24 can design anything: **this
 * board's crystal error in ppm**, and **the DCF path's mean offset and
 * standard deviation** against a reference. Both are measured here, against
 * the GPS-disciplined stratum-1 named by CONFIG_DCF77_P0_NTP, and both are
 * exposed at /proc/dcf77log for a host to collect over 9P.
 *
 * **Nothing here ever sets the clock, and that is the whole design.** The
 * method compares two independent statements about what time it is -- the
 * radio's and the reference's -- against one free-running local clock. A
 * commit would step that clock and destroy the comparison, so:
 *
 *   * DCF auto-sync is turned OFF when this starts (it is on by default,
 *     nightly at 03:17),
 *   * the reference is asked with ntp_query(), which reports an offset and
 *     touches nothing, rather than ntp_sync(), which applies it,
 *   * the DS3231 is never written.
 *
 * The one exception is deliberate and happens once: a single ntp_sync() at
 * startup, so the run begins near the truth. That is not needed for the
 * comparison -- which only needs the clock to be *stable*, not correct --
 * but it keeps every later offset small enough to read and to store in 32
 * bits. Recorded in the log's header so a collector can see it happened.
 *
 * **The sync interval is not increased, because there is nothing to
 * increase.** drivers/dcf77_service.c records every accepted frame
 * continuously, whether or not a sync was asked for -- so a validated radio
 * time already arrives about once a minute, for free, and always has. What
 * the nightly schedule controls is only whether the clock is *written*,
 * which is exactly what must not happen here.
 *
 * **Gaps are survivable by construction.** The board is an appliance at the
 * far end of a radio, so a collector will miss stretches. The running
 * accumulators below are updated per sample and never rolled off, so the two
 * headline numbers are correct however little of the raw series was
 * collected; the ring is a recent window for eyeballing and for a host that
 * polls often enough to reassemble the full series.
 */

#ifndef DRIVERS_DCF77_P0LOG_H
#define DRIVERS_DCF77_P0LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Starts the supervising task. Safe to call on a board with no receiver and
 * no network -- it simply logs nothing. Returns the pid, or -1. */
int dcf77_p0log_start(void);

/* True once dcf77_p0log_start() has been called. */
bool dcf77_p0log_active(void);

/* Renders the report behind /proc/dcf77log into `buf`, returning the length.
 * Header first (the two accumulated results and enough context to trust
 * them), then the most recent samples, oldest first. */
uint32_t dcf77_p0log_render(char *buf, uint32_t cap);

#endif /* DRIVERS_DCF77_P0LOG_H */
