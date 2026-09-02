#include "kernel/time.h"
#include "kernel/timezone.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "lugalos_config.h"
#include <string.h>

static uint64_t g_boot_us_offset = 0;

/* The wall clock is kept as UTC milliseconds since 1970, pinned to a reading
 * of the monotonic counter. UTC, not local time (user, 2026-08-23): every
 * other time source this system will grow -- GPS, NTP -- speaks UTC, DCF-77
 * states its own offset in each frame, and local time is the one
 * representation with no correct answer during the hour that repeats every
 * October. Local time is computed on demand from kernel/timezone.c and is
 * never stored. */
/* Microseconds, since P2 (plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * The pair was milliseconds until a measurement ran into the floor: P0's own
 * noise came out at 3.7 ms against 5.0 ms of total scatter, so the instrument
 * and the thing it was measuring were the same size, and the residual that
 * mattered afterwards was ~5 ms -- a quantity a millisecond clock cannot
 * settle either way. §3.4's PPS comparison is expressed in microseconds and
 * simply cannot be recorded here otherwise.
 *
 * The monotonic side matters as much as the epoch side: reading it in
 * milliseconds threw away three digits of a counter that has them
 * (time_get_us() is a 1 us TIMER0 read), so a set-then-read round trip lost
 * sub-millisecond detail even when both ends had it. */
static int64_t  g_base_epoch_us = 1785931200000000LL; /* 2026-08-05 12:00:00 UTC */
static uint64_t g_base_mono_us = 0;

/* Whether anything has ever set this clock, as opposed to it still holding
 * the instant compiled in above. The difference is invisible in the value --
 * 2026-08-05 12:00 UTC is a perfectly plausible time, and on a board in CEST
 * it displays as 14:00 -- so a caller that wants to know cannot work it out
 * by looking. A clock face showing a fabricated time with no way to tell is
 * exactly what this exists to prevent. */
static bool g_clock_set = false;

#if defined(CONFIG_BOARD_RP2350)
#define TIMER0_BASE      0x400B0000UL
#define TIMER0_TIMEHR    (*(volatile uint32_t *)(TIMER0_BASE + 0x08))
#define TIMER0_TIMELR    (*(volatile uint32_t *)(TIMER0_BASE + 0x0C))
/* TIMER0 counts one tick per pulse from the TICKS block, which the boot code
 * programs to divide the 12 MHz clk_ref by 12. Every "microsecond" in this
 * kernel is that divider being right, so print it: a 12/28 mistake here (see
 * arch/riscv/rp2350/boot_header.S) made the whole system run at 43% speed and
 * was invisible for months because nothing ever said what the divisor was. */
#define TICKS_TIMER0_CYCLES (*(volatile uint32_t *)0x4010801CUL)
#elif !defined(CONFIG_MODE_S)
/* QEMU RV32 (M-mode): the same CLINT kernel/ticker.c already reads for its
 * preemption deadline, at the same documented 10 MHz virt-machine rate
 * (ticker.c's own TICK_HZ). Found and fixed 2026-08-12
 * (plan/phase10_chess_completion.md J0): this branch used to be a bare call
 * counter (`++g_soft_us * 100`), entirely decoupled from real elapsed time
 * -- every read advanced it by a fixed 100 "microseconds" regardless of how
 * much wall-clock time had actually passed, so any caller measuring a time
 * budget against it (chess's iterative-deepening search, `check_up_time()`)
 * was really measuring "how many times has this been polled", not time.
 * Harmless for callers that only care about relative ordering, but a search
 * time budget silently became a *node-count* budget instead, which is easy
 * to miss until something runs far longer than its stated time limit. */
#define CLINT_BASE     0x02000000UL
#define CLINT_MTIME    (*(volatile uint64_t *)(CLINT_BASE + 0xBFF8))
#endif

static inline uint64_t read_hardware_counter_us(void) {
#if defined(CONFIG_BOARD_RP2350)
    uint32_t hi, lo;
    do {
        hi = TIMER0_TIMEHR;
        lo = TIMER0_TIMELR;
    } while (hi != TIMER0_TIMEHR);
    return ((uint64_t)hi << 32) | lo;
#elif !defined(CONFIG_MODE_S)
    return CLINT_MTIME / 10; /* 10 MHz ticks -> microseconds */
#else
    /* QEMU RV64 (S-mode) and K210 (S-mode, no board file yet -- same bucket
     * ticker.c already puts it in): the Sstc `rdtime` pseudo-instruction
     * reads the `time` CSR directly, which is what S-mode gets instead of
     * the CLINT's raw MMIO (M-mode-only on most cores). Same 10 MHz virt
     * rate on QEMU as the CLINT branch above -- ticker.c's own comment notes
     * this is "the same virt machine clock, read via rdtime". */
    uint64_t t;
    __asm__ __volatile__("rdtime %0" : "=r"(t));
    return t / 10;
#endif
}

void time_init(void) {
    g_boot_us_offset = read_hardware_counter_us();
    g_base_mono_us = time_get_us();
    tz_set(CONFIG_TIMEZONE);
#if defined(CONFIG_BOARD_RP2350)
    printk("[Timer] System Hardware Clock Initialized (clk_ref/%u = 1 us tick).\n",
           (unsigned)(TICKS_TIMER0_CYCLES & 0x1ffu));
#else
    printk("[Timer] System Hardware Clock Initialized (Resolution: 1 us).\n");
#endif
}

uint64_t time_get_us(void) {
    uint64_t cur = read_hardware_counter_us();
    return (cur >= g_boot_us_offset) ? (cur - g_boot_us_offset) : cur;
}

uint64_t time_get_ms(void) {
    return time_get_us() / 1000;
}

#include "drivers/usb_cdc.h"

void time_delay_us(uint64_t us) {
    uint64_t start = time_get_us();
    while (time_get_us() - start < us) {
        usb_cdc_task();
    }
}

/* Days from 1970-01-01 to a civil (proleptic Gregorian) date, and back.
 * Howard Hinnant's algorithms, the same pair drivers/dcf77_decode.c uses --
 * branch-free, correct for every year this will ever see, and far easier to
 * be sure of than the month-walking loop that used to live here. The date is
 * treated as naive: these are pure calendar arithmetic and know nothing about
 * timezones, which is exactly what both a UTC clock and kernel/timezone.c's
 * local-time arithmetic need. */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - (int)(era * 400));
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (int64_t)doe - 719468LL;
}

static void civil_from_days(int64_t z, int *y_out, unsigned *m_out, unsigned *d_out) {
    z += 719468LL;
    const int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    const unsigned doe = (unsigned)(z - era * 146097LL);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t y = (int64_t)yoe + era * 400LL;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp + (mp < 10u ? 3u : -9u);
    *y_out = (int)(y + (m <= 2u));
    *m_out = m;
    *d_out = d;
}

int64_t time_to_epoch(const rtc_time_t *tm) {
    if (!tm) return 0;
    return days_from_civil((int)tm->year, tm->month, tm->day) * 86400LL
         + (int64_t)tm->hour * 3600LL + (int64_t)tm->min * 60LL + (int64_t)tm->sec;
}

void time_from_epoch(int64_t sec, rtc_time_t *tm) {
    if (!tm) return;
    /* Floor division, not truncation: dates before 1970 are not expected, but
     * a clock that has not been set yet can be handed one by a caller doing
     * arithmetic, and truncation would put it a day out rather than an hour. */
    int64_t days = sec / 86400LL;
    int64_t rem  = sec % 86400LL;
    if (rem < 0) { rem += 86400LL; days -= 1; }

    int y; unsigned m, d;
    civil_from_days(days, &y, &m, &d);
    tm->year  = (uint16_t)y;
    tm->month = (uint8_t)m;
    tm->day   = (uint8_t)d;
    tm->hour  = (uint8_t)(rem / 3600);
    tm->min   = (uint8_t)((rem % 3600) / 60);
    tm->sec   = (uint8_t)(rem % 60);
    tm->ms    = 0;
}

unsigned time_weekday(const rtc_time_t *tm) {
    if (!tm) return 1;
    /* 1970-01-01 was a Thursday, so (days + 4) mod 7 counts from Sunday = 0. */
    int64_t days = days_from_civil((int)tm->year, tm->month, tm->day);
    int64_t w = (days + 4) % 7;
    if (w < 0) w += 7;
    return (w == 0) ? 7u : (unsigned)w;
}

int64_t time_epoch_us(void) {
    return g_base_epoch_us + (int64_t)(time_get_us() - g_base_mono_us);
}

void time_set_epoch_us(int64_t us) {
    g_clock_set = true;
    g_base_epoch_us = us;
    g_base_mono_us = time_get_us();
}

void time_get_utc(rtc_time_t *tm) {
    if (!tm) return;
    int64_t now_us = time_epoch_us();
    int64_t sec = now_us / 1000000;
    int64_t rem = now_us % 1000000;
    if (rem < 0) { rem += 1000000; sec -= 1; }
    time_from_epoch(sec, tm);
    /* rtc_time_t keeps milliseconds and stays that way: the display, the
     * DS3231 and `date` have no use for microseconds, and widening the struct
     * would touch every caller for the benefit of two. Callers that want the
     * finer number take time_epoch_us(). */
    tm->ms = (uint16_t)(rem / 1000);
}

bool time_is_set(void) { return g_clock_set; }

void time_set_utc(const rtc_time_t *tm) {
    if (!tm) return;
    time_set_epoch_us(time_to_epoch(tm) * 1000000LL + (int64_t)tm->ms * 1000LL);
}

/* P2's own check (plan/phase24_dcf77_precision_and_ntp_server.md).
 *
 * Every assertion here would have passed on the millisecond clock this
 * replaced except the two that matter -- sub-millisecond values surviving a
 * set-then-read, and a difference of a few hundred microseconds being
 * representable at all. Those are exactly what P3b's PPS comparison needs and
 * what a millisecond clock silently rounded to zero. */
void time_selftest(void) {
    int pass = 0, fail = 0;
    #define CHECK(cond, what) do { \
        if (cond) { pass++; cprintf("  [ok] %s\n", what); } \
        else      { fail++; cprintf("  [FAIL] %s\n", what); } \
    } while (0)

    cprintf("\nMicrosecond wall clock (P2)\n");

    int64_t saved = time_epoch_us();

    /* 2031-06-15 12:34:56.500123 UTC -- a sub-millisecond remainder chosen so
     * that truncation to milliseconds is visible rather than plausible. */
    const int64_t probe = 1939293296500123LL;
    time_set_epoch_us(probe);
    int64_t back = time_epoch_us();
    CHECK(back >= probe && back - probe < 50000,
          "set/get round trip returns the instant that was set");
    CHECK((back % 1000) != 0 || (probe % 1000) == 0,
          "the sub-millisecond remainder survives (a ms clock zeroes it)");

    /* Two instants 250 us apart must be distinguishable. On a millisecond
     * clock this difference is 0. */
    time_set_epoch_us(probe);
    int64_t a = time_epoch_us();
    time_set_epoch_us(probe + 250);
    int64_t b = time_epoch_us();
    CHECK(b - a >= 200 && b - a <= 300,
          "250 us of separation is representable and measured");

    /* The rtc_time_t view still agrees, to its own resolution. It carries
     * milliseconds and is meant to: this checks the widening did not shift
     * what every existing caller sees. */
    time_set_epoch_us(probe);
    rtc_time_t tm;
    time_get_utc(&tm);
    CHECK(tm.year == 2031 && tm.month == 6 && tm.day == 15,
          "rtc_time_t date is unchanged by the widening");
    CHECK(tm.hour == 12 && tm.min == 34 && tm.sec == 56 && tm.ms == 500,
          "rtc_time_t time truncates to whole milliseconds, as it always did");

    /* And setting through the old interface still lands where it did. */
    rtc_time_t set = { 2031, 6, 15, 12, 34, 56, 500 };
    time_set_utc(&set);
    int64_t via_rtc = time_epoch_us();
    CHECK(via_rtc / 1000 >= probe / 1000 && via_rtc / 1000 - probe / 1000 < 50,
          "time_set_utc() still sets the same instant, to its ms resolution");

    CHECK(time_is_set(), "the clock reports itself as set");

    time_set_epoch_us(saved);
    cprintf("%s (%d passed, %d failed)\n",
            fail ? "TIME_SELFTEST_FAIL" : "TIME_SELFTEST_OK", pass, fail);
    #undef CHECK
}

void time_get_local(rtc_time_t *tm) {
    if (!tm) return;
    rtc_time_t utc;
    time_get_utc(&utc);
    tz_utc_to_local(&utc, tm);
    tm->ms = utc.ms;
}

void time_set_local(const rtc_time_t *tm) {
    if (!tm) return;
    rtc_time_t utc;
    tz_local_to_utc(tm, &utc);
    utc.ms = tm->ms;
    time_set_utc(&utc);
}

void time_format_iso(const rtc_time_t *tm, char *buf, int max_len) {
    if (!tm || !buf || max_len < 20) return;
    // YYYY-MM-DD HH:MM:SS
    int y = tm->year;
    int m = tm->month;
    int d = tm->day;
    int hr = tm->hour;
    int min = tm->min;
    int sec = tm->sec;

    buf[0] = '0' + ((y / 1000) % 10);
    buf[1] = '0' + ((y / 100) % 10);
    buf[2] = '0' + ((y / 10) % 10);
    buf[3] = '0' + (y % 10);
    buf[4] = '-';
    buf[5] = '0' + (m / 10);
    buf[6] = '0' + (m % 10);
    buf[7] = '-';
    buf[8] = '0' + (d / 10);
    buf[9] = '0' + (d % 10);
    buf[10] = ' ';
    buf[11] = '0' + (hr / 10);
    buf[12] = '0' + (hr % 10);
    buf[13] = ':';
    buf[14] = '0' + (min / 10);
    buf[15] = '0' + (min % 10);
    buf[16] = ':';
    buf[17] = '0' + (sec / 10);
    buf[18] = '0' + (sec % 10);
    buf[19] = '\0';
}

bool time_parse_iso(const char *str, rtc_time_t *tm) {
    if (!str || strlen(str) < 19 || !tm) return false;
    // Format: YYYY-MM-DD HH:MM:SS
    uint16_t y = (str[0]-'0')*1000 + (str[1]-'0')*100 + (str[2]-'0')*10 + (str[3]-'0');
    uint8_t m = (str[5]-'0')*10 + (str[6]-'0');
    uint8_t d = (str[8]-'0')*10 + (str[9]-'0');
    uint8_t hr = (str[11]-'0')*10 + (str[12]-'0');
    uint8_t min = (str[14]-'0')*10 + (str[15]-'0');
    uint8_t sec = (str[17]-'0')*10 + (str[18]-'0');

    if (m < 1 || m > 12 || d < 1 || d > 31 || hr > 23 || min > 59 || sec > 59) {
        return false;
    }

    tm->year = y;
    tm->month = m;
    tm->day = d;
    tm->hour = hr;
    tm->min = min;
    tm->sec = sec;
    tm->ms = 0;
    return true;
}
