#ifndef DRIVERS_I2C_RTC_H
#define DRIVERS_I2C_RTC_H

/* The real-time clock (a DS3231/DS1307 module found at 0x68, or the board's
 * RX8130CE at 0x32 where CONFIG_RTC_RX8130 says so): a device on the I2C bus, and nothing
 * more.
 *
 * The bus it sits on is drivers/i2c_bus.h, which is where the controller, the
 * owning task and i2c_xfer() live. They used to live here, which is why a
 * pressure sensor once reached the bus by including a clock's header.
 */

#include "kernel/time.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RTC_TYPE_NONE = 0,
    RTC_TYPE_DS1307,
    RTC_TYPE_DS3231,
    RTC_TYPE_RX8130,    /* Epson RX8130CE at 0x32, a board part (CONFIG_RTC_RX8130) */
} rtc_chip_type_t;

void i2c_rtc_init(void);
bool i2c_rtc_read_time(rtc_time_t *tm);
/* True when the RTC reports that its oscillator stopped -- on a DS3231 this
 * is OSF in status register 0x0F; on a DS1307 this is CH (clock halt, bit 7 of
 * seconds register 0x00). Both indicate power loss with no working backup
 * cell, or a stopped oscillator. Writing the time clears it (and starts the
 * DS1307 oscillator). */
bool i2c_rtc_lost_power(void);

bool i2c_rtc_write_time(const rtc_time_t *tm);
bool i2c_rtc_is_detected(void);
rtc_chip_type_t i2c_rtc_get_type(void);
const char *i2c_rtc_get_model_name(void);
/* The detected chip's I2C address (0x68 for the DS parts, 0x32 for the RX8130CE). */
uint8_t i2c_rtc_get_addr(void);

/* DS3231-only (registers 0x11/0x12; a DS1307 has no temperature sensor and
 * this returns false for one). Rounded to the nearest whole degree C --
 * L2/'s display only ever wants a plain
 * int, so the quarter-degree register precision is collapsed here rather
 * than pushed onto every caller. */
bool i2c_rtc_read_temperature_c(int *temp_c);

/* The last reading this driver took, without touching the bus, plus how long
 * ago it was taken. For callers that must not do I2C -- /proc is served by the
 * 9P task -- and for correlating something against temperature, where a value
 * without its age is worse than no value. False until one has been taken. */
bool i2c_rtc_cached_temperature_c(int *temp_c, uint32_t *age_s);



/* one generic transfer for
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
