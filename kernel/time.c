#include "kernel/time.h"
#include "kernel/printk.h"
#include <string.h>

static uint64_t g_boot_us_offset = 0;
static rtc_time_t g_base_rtc = { .year = 2026, .month = 8, .day = 5, .hour = 12, .min = 0, .sec = 0, .ms = 0 };
static uint64_t g_base_rtc_ms = 0;

#if defined(CONFIG_BOARD_RP2350)
#define TIMER0_BASE      0x400B0000UL
#define TIMER0_TIMEHR    (*(volatile uint32_t *)(TIMER0_BASE + 0x08))
#define TIMER0_TIMELR    (*(volatile uint32_t *)(TIMER0_BASE + 0x0C))
#endif

static inline uint64_t read_hardware_counter_us(void) {
#if defined(CONFIG_BOARD_RP2350)
    uint32_t hi, lo;
    do {
        hi = TIMER0_TIMEHR;
        lo = TIMER0_TIMELR;
    } while (hi != TIMER0_TIMEHR);
    return ((uint64_t)hi << 32) | lo;
#else
    static volatile uint64_t g_soft_us = 0;
    return ++g_soft_us * 100;
#endif
}

void time_init(void) {
    g_boot_us_offset = read_hardware_counter_us();
    g_base_rtc_ms = time_get_ms();
    printk("[Timer] System Hardware Clock Initialized (Resolution: 1 us).\n");
}

uint64_t time_get_us(void) {
    uint64_t cur = read_hardware_counter_us();
    return (cur >= g_boot_us_offset) ? (cur - g_boot_us_offset) : cur;
}

uint64_t time_get_ms(void) {
    return time_get_us() / 1000;
}

static bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static const uint8_t days_in_months[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static uint8_t get_days_in_month(uint16_t year, uint8_t month) {
    if (month < 1 || month > 12) return 31;
    if (month == 2 && is_leap_year(year)) return 29;
    return days_in_months[month - 1];
}

void time_get_rtc(rtc_time_t *tm) {
    if (!tm) return;
    uint64_t elapsed_ms = time_get_ms() - g_base_rtc_ms;

    uint64_t total_sec = (uint64_t)g_base_rtc.sec + (elapsed_ms / 1000);
    uint32_t add_ms = (uint32_t)(elapsed_ms % 1000) + g_base_rtc.ms;

    uint32_t sec = total_sec % 60;
    uint64_t total_min = (g_base_rtc.min + (total_sec / 60));
    uint32_t min = total_min % 60;
    uint64_t total_hr = (g_base_rtc.hour + (total_min / 60));
    uint32_t hr = total_hr % 24;
    uint32_t days_to_add = (uint32_t)(total_hr / 24);

    uint16_t y = g_base_rtc.year;
    uint8_t m = g_base_rtc.month;
    uint8_t d = g_base_rtc.day;

    while (days_to_add > 0) {
        uint8_t dim = get_days_in_month(y, m);
        if (d + days_to_add <= dim) {
            d += days_to_add;
            days_to_add = 0;
        } else {
            days_to_add -= (dim - d + 1);
            d = 1;
            m++;
            if (m > 12) {
                m = 1;
                y++;
            }
        }
    }

    tm->year = y;
    tm->month = m;
    tm->day = d;
    tm->hour = (uint8_t)hr;
    tm->min = (uint8_t)min;
    tm->sec = (uint8_t)sec;
    tm->ms = (uint16_t)(add_ms % 1000);
}

void time_set_rtc(const rtc_time_t *tm) {
    if (!tm) return;
    g_base_rtc = *tm;
    g_base_rtc_ms = time_get_ms();
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
