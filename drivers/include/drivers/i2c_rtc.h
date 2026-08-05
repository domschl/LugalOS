#ifndef DRIVERS_I2C_RTC_H
#define DRIVERS_I2C_RTC_H

#include "kernel/time.h"
#include <stdbool.h>

void i2c_rtc_init(void);
bool i2c_rtc_read_time(rtc_time_t *tm);
bool i2c_rtc_write_time(const rtc_time_t *tm);
bool i2c_rtc_is_detected(void);
void i2c_scan_bus(void);

#endif // DRIVERS_I2C_RTC_H
