#ifndef KERNEL_TIMEZONE_H
#define KERNEL_TIMEZONE_H

#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * POSIX TZ rules: the whole timezone story in one string.
 *
 * The kernel clock runs on UTC (kernel/time.c). Local time is not stored
 * anywhere -- it is computed from UTC and this rule whenever someone wants to
 * look at a clock face. That ordering is the point: a clock that stores local
 * time has no correct answer during the hour that repeats every October, and
 * every additional time source we plan to add (GPS, NTP) speaks UTC natively,
 * while DCF-77 states its own offset in the frame. Storing UTC makes all
 * three agree by construction.
 *
 * The format is the one every embedded stack already understands -- the same
 * string an ESP32 or an Arduino takes, from the tz-list summary at
 * https://mm.icann.org/pipermail/tz/2016-April/023570.html:
 *
 *   std offset [dst [offset] [,start[/time],end[/time]]]
 *
 *   "CET-1CEST,M3.5.0,M10.5.0/3"   Germany (the default, CONFIG_TIMEZONE)
 *   "UTC0"                         no local offset, no DST
 *   "MST7"                         constant UTC-7, no DST
 *   "AEST-10AEDT,M10.1.0,M4.1.0/3" Sydney -- southern hemisphere, DST wraps
 *                                  the new year
 *
 * Two details that catch everyone:
 *
 *  - The offset sign is *inverted* from what a human means by "UTC+1". POSIX
 *    writes the offset that must be ADDED to local time to get UTC, so
 *    central Europe is "CET-1". This header and everything below it work in
 *    minutes EAST of UTC (Berlin = +60), and the sign is flipped exactly once,
 *    at parse time.
 *
 *  - A rule's transition time is in the local time in force just before the
 *    transition: standard time for the spring rule, summer time for the
 *    autumn one. "M3.5.0,M10.5.0/3" is therefore 02:00 CET and 03:00 CEST,
 *    which are the same instant in UTC (01:00) -- as the EU intends, so the
 *    whole union switches together.
 *
 * `Mm.w.d` means month m (1-12), week w (1-4, or 5 for "last"), day d
 * (0 = Sunday). The `Jn` and `n` julian-day rule forms are deliberately NOT
 * supported: nothing in the tz database uses them for any zone this clock
 * will see, and rejecting them loudly beats implementing two more date
 * dialects that no test would ever exercise.
 */

/* Install a rule. Returns false and leaves the previous rule in force if the
 * string does not parse -- a clock with a stale-but-valid timezone is more
 * use than one that has fallen back to UTC without saying so. */
bool tz_set(const char *spec);

/* The string currently in force, exactly as it was given. */
const char *tz_get(void);

/* Offset EAST of UTC, in minutes, at the given UTC instant. `abbrev` (may be
 * NULL) receives the zone abbreviation in force -- "CET" or "CEST" -- and
 * `is_dst` (may be NULL) whether summer time applies. */
int tz_offset_min(const rtc_time_t *utc, const char **abbrev, bool *is_dst);

void tz_utc_to_local(const rtc_time_t *utc, rtc_time_t *local);

/* The inverse. During the hour that repeats at the autumn transition a local
 * time names two instants; this resolves to the first (still summer time),
 * which is what every POSIX implementation does. During the hour that does
 * not exist in spring the result is the instant one offset-step later. Both
 * are deterministic, and neither is "correct" in any deeper sense -- the
 * ambiguity is in the question, not the answer. */
void tz_local_to_utc(const rtc_time_t *local, rtc_time_t *utc);

/* Convert a time that states its own offset -- a DCF-77 frame's Z1/Z2 bits, a
 * GPS receiver's leap-second-corrected output, an NTP peer -- into UTC,
 * without consulting the rule at all. A source that tells you its offset is
 * more trustworthy than a rule that has to infer one, and this is the path
 * that stays right if the EU ever abolishes DST between releases. */
void tz_from_offset(const rtc_time_t *local, int offset_min, rtc_time_t *utc);

/* Rule parsing and conversion against a table of known transitions, including
 * both sides of both European switchovers and a southern-hemisphere zone.
 * Returns the number of failures (0 = all passed). No hardware, no clock --
 * see kernel/shell.c's `tzselftest`. */
int tz_selftest(void);

#endif /* KERNEL_TIMEZONE_H */
