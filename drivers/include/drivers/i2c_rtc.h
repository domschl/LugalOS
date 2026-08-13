#ifndef DRIVERS_I2C_RTC_H
#define DRIVERS_I2C_RTC_H

#include "kernel/time.h"
#include <stdbool.h>

void i2c_rtc_init(void);
bool i2c_rtc_read_time(rtc_time_t *tm);
bool i2c_rtc_write_time(const rtc_time_t *tm);
bool i2c_rtc_is_detected(void);
void i2c_scan_bus(void);

/* DS3231-only (registers 0x11/0x12; a DS1307 has no temperature sensor and
 * this returns false for one). Rounded to the nearest whole degree C --
 * L2/plan/phase11_pico_clock_green.md's display only ever wants a plain
 * int, so the quarter-degree register precision is collapsed here rather
 * than pushed onto every caller. */
bool i2c_rtc_read_temperature_c(int *temp_c);

#endif // DRIVERS_I2C_RTC_H
