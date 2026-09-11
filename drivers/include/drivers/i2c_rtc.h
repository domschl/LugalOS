#ifndef DRIVERS_I2C_RTC_H
#define DRIVERS_I2C_RTC_H

/* The DS3231/DS1307 real-time clock: a device on the I2C bus, and nothing
 * more (category E, plan/phase30_driver_framework.md).
 *
 * The bus it sits on is drivers/i2c_bus.h, which is where the controller, the
 * owning task and i2c_xfer() live. They used to live here, which is why a
 * pressure sensor once reached the bus by including a clock's header.
 */

#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

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


#endif // DRIVERS_I2C_RTC_H
