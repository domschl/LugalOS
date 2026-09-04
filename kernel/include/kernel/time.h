#ifndef KERNEL_TIME_H
#define KERNEL_TIME_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t year;   // e.g. 2026
    uint8_t  month;  // 1-12
    uint8_t  day;    // 1-31
    uint8_t  hour;   // 0-23
    uint8_t  min;    // 0-59
    uint8_t  sec;    // 0-59
    uint16_t ms;     // 0-999
} rtc_time_t;

void time_init(void);

/* Monotonic time */
uint64_t time_get_us(void);
uint64_t time_get_ms(void);
void time_delay_us(uint64_t us);

/* Wall clock. The clock the kernel keeps is UTC; local time is derived from
 * it through kernel/timezone.c and stored nowhere (see the note there and in
 * time.c). Every caller has to say which one it means, which is the point:
 * `date` and the clock face want local, the DS3231 and every network or radio
 * time source want UTC, and a single time_get_rtc() that silently meant one
 * of them is how a clock ends up an hour out twice a year. */
void time_get_utc(rtc_time_t *tm);
void time_set_utc(const rtc_time_t *tm);

/* The wall clock as microseconds since 1970, and the way to set it.
 *
 * The primitive the rtc_time_t accessors are built on (P2,
 * plan/phase24_dcf77_precision_and_ntp_server.md). rtc_time_t carries only
 * milliseconds and deliberately still does; anything comparing two instants
 * finely -- an NTP offset, a DCF-77 mark against a GPS pulse -- wants these
 * instead. Signed, because every quantity derived from them is a difference
 * and half of those are negative. */
int64_t time_epoch_us(void);
void    time_set_epoch_us(int64_t us);

/* The wall clock as it read at a past monotonic instant. What a discipline
 * loop needs: a radio frame is recognised long after the mark it timestamps,
 * and comparing the mark against the clock's reading *now* would fold that
 * delay straight into the measurement. */
int64_t time_epoch_us_at(uint64_t mono_us);

/* The clock's rate, in parts per billion against the raw monotonic counter.
 * Billion rather than million because this board's crystal error is -0.46 ppm
 * -- a ppm knob could only round that to nothing or to double. */
void    time_set_freq_ppb(int32_t ppb);
int32_t time_freq_ppb(void);

/* Pays off `amount_us` of accumulated offset by running at a different rate
 * for `over_ms`, instead of stepping. Positive amount moves the clock forward.
 * A clock that jumps backwards to correct itself breaks anything timing an
 * interval across the jump -- which after P6 is every NTP client on the
 * segment -- so stepping is reserved for the case where the clock is so wrong
 * that monotonicity is not worth preserving. */
void    time_slew_us(int64_t amount_us, uint32_t over_ms);
bool    time_slewing(void);

/* False while the clock still holds the instant compiled into kernel/time.c
 * -- i.e. nothing (RTC, NTP, DCF-77 or a person) has ever set it. The value
 * is plausible either way, so this is the only way to know. */
bool time_is_set(void);

/* `timeselftest`: the microsecond clock's own checks, on every target. */
void time_selftest(void);
void time_get_local(rtc_time_t *tm);
void time_set_local(const rtc_time_t *tm);

/* Naive civil-time <-> seconds-since-1970 conversion. Knows nothing about
 * timezones: feed it UTC and you get a UTC epoch, feed it local time and you
 * get local seconds, which is exactly what timezone arithmetic needs. */
int64_t time_to_epoch(const rtc_time_t *tm);
void    time_from_epoch(int64_t sec, rtc_time_t *tm);

/* Day of week, 1 = Monday .. 7 = Sunday (ISO, and the numbering DCF-77 uses).
 * Computed from the date, never read from a DS3231 weekday register: that
 * register is a free-running counter the chip never checks against the date,
 * and nothing in this tree writes it. */
unsigned time_weekday(const rtc_time_t *tm);

/* Formatted ISO string helpers (YYYY-MM-DD HH:MM:SS) */
void time_format_iso(const rtc_time_t *tm, char *buf, int max_len);
bool time_parse_iso(const char *str, rtc_time_t *tm);

#endif // KERNEL_TIME_H
