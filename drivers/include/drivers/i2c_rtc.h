#ifndef DRIVERS_I2C_RTC_H
#define DRIVERS_I2C_RTC_H

#include "lugalos_config.h"
#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

/* Whether this build has a real I2C controller behind the functions below.
 *
 * Named as a capability rather than spelled `#if defined(CONFIG_BOARD_RP2350)`
 * at each site, because that is what those sites meant and it stopped being
 * what it said the moment a second board grew a controller: the ESP32-P4
 * spent E7 announcing "No I2C controller on this target" while answering a
 * bus scan (plan/phase27_esp32p4_bringup.md E7). A board that gains one adds
 * itself here, once. */
#if defined(CONFIG_BOARD_RP2350) || defined(CONFIG_BOARD_ESP32P4)
#define I2C_HAVE_CONTROLLER 1
#else
#define I2C_HAVE_CONTROLLER 0
#endif

void i2c_rtc_init(void);
bool i2c_rtc_read_time(rtc_time_t *tm);
/* True when the RTC reports that its oscillator stopped -- a DS3231 setting
 * OSF, which is what a power loss with no working backup cell looks like from
 * software. The stored time is meaningless while this holds, and meaningless
 * in a way that *passes every range check*: a reset DS3231 reads
 * 2000-01-01 00:00:00. Writing the time clears it. */
bool i2c_rtc_lost_power(void);

bool i2c_rtc_write_time(const rtc_time_t *tm);
bool i2c_rtc_is_detected(void);
void i2c_scan_bus(void);

/* DS3231-only (registers 0x11/0x12; a DS1307 has no temperature sensor and
 * this returns false for one). Rounded to the nearest whole degree C --
 * L2/plan/phase11_pico_clock_green.md's display only ever wants a plain
 * int, so the quarter-degree register precision is collapsed here rather
 * than pushed onto every caller. */
bool i2c_rtc_read_temperature_c(int *temp_c);

/* The last reading this driver took, without touching the bus, plus how long
 * ago it was taken. For callers that must not do I2C -- /proc is served by the
 * 9P task -- and for correlating something against temperature, where a value
 * without its age is worse than no value. False until one has been taken. */
bool i2c_rtc_cached_temperature_c(int *temp_c, uint32_t *age_s);

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

/* Q4, plan/phase26_mqtt_and_environment_sensors.md: one generic transfer for
 * any device on this bus -- write `wlen` bytes, then read `rlen`; either half
 * may be empty. Routed through the shared "i2c" task when it is running, so a
 * caller never contends with the RTC or the EEPROM for the bus, and taken
 * straight to the hardware before that task exists.
 *
 * This exists so that adding an I2C part costs no kernel-surface change:
 * drivers/bme280.c is ordinary M-mode code that builds requests here, rather
 * than another opcode in a U-mode dispatch (and so, unlike the drivers with
 * UATTR code in them, it needs no -fno-jump-tables entry in CMakeLists.txt).
 *
 * Bounded at 16 bytes written and 64 read, which is the shape a register map
 * needs -- a bulk device wants its own op, not a bigger one of these. */
/* The bounds i2c_xfer() enforces, public because its callers size buffers
 * against them. Sized by the largest single transaction any device on this
 * bus performs, which is an AT24C32 page write: two address bytes plus a
 * 32-byte page. The write bound was 16 while the EEPROM had its own opcode
 * carrying up to 128 bytes; with every device a plain i2c_xfer() client
 * (plan/phase30_driver_framework.md category E) this is the bound that
 * matters, and the endpoint's buffers get smaller rather than larger. */
#define I2C_XFER_WMAX 40u
#define I2C_XFER_RMAX 64u

bool i2c_xfer(uint8_t addr, const uint8_t *w, uint32_t wlen,
              uint8_t *r, uint32_t rlen);

/* Internal: used by drivers/at24c32.c to route EEPROM ops through this same
 * task/endpoint instead of duplicating bus-arbitration logic. Not meant for
 * callers outside this driver pair. */
bool i2c_task_alive(void);
int i2c_task_call(const uint8_t *req, uint32_t req_len,
                  uint8_t *resp, uint32_t resp_max);

#endif // DRIVERS_I2C_RTC_H
