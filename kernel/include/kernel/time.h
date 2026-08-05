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

/* Wall clock RTC time */
void time_get_rtc(rtc_time_t *tm);
void time_set_rtc(const rtc_time_t *tm);

/* Formatted ISO string helpers (YYYY-MM-DD HH:MM:SS) */
void time_format_iso(const rtc_time_t *tm, char *buf, int max_len);
bool time_parse_iso(const char *str, rtc_time_t *tm);

#endif // KERNEL_TIME_H
