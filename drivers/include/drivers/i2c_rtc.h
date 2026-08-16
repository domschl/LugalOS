#ifndef DRIVERS_I2C_RTC_H
#define DRIVERS_I2C_RTC_H

#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

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

/* M4.5, plan/phase12_microkernel_migration.md, Part B: RTC and EEPROM share
 * one physical I2C bus (both on I2C0 in the default persona -- see
 * drivers/at24c32.c's own comment on why its bit-banging is not literally
 * the same code as this file's), so they are converted together as a single
 * "i2c" driver task rather than two independent ones. That also happens to
 * close a pre-existing hole: before this, a preempted i2c_rtc_write_time()
 * and an interleaved at24c32_write() had no mutual exclusion at all.
 *
 * Must run after sched_init(); every RTC/EEPROM function keeps working via
 * direct hardware access if this fails or hasn't run yet, same as every
 * other M4.5 driver-task conversion. Returns the task's pid, or -1. */
int i2c_task_start(void);

// M4.5 verify: how many chan_call()s the shared "i2c" task has served since
// boot -- see drivers/spisd_rp2350.c's g_blk_calls comment for the reasoning.
uint32_t i2c_task_call_count(void);

/* Internal: used by drivers/at24c32.c to route EEPROM ops through this same
 * task/endpoint instead of duplicating bus-arbitration logic. Not meant for
 * callers outside this driver pair. */
bool i2c_task_alive(void);
int i2c_task_call(const uint8_t *req, uint32_t req_len,
                  uint8_t *resp, uint32_t resp_max);

#endif // DRIVERS_I2C_RTC_H
