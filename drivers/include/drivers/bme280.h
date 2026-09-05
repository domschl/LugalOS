#ifndef DRIVERS_BME280_H
#define DRIVERS_BME280_H

#include <stdint.h>
#include <stdbool.h>

/* Bosch BMP280 / BME280 environment sensors (Q4,
 * plan/phase26_mqtt_and_environment_sensors.md).
 *
 * Two parts, one driver: they are register-compatible except that the BME280
 * adds humidity, so this probes and adapts rather than being configured for
 * one. It rides the shared "i2c" task through i2c_xfer() (see
 * drivers/i2c_rtc.h) rather than touching the bus itself -- a second bus
 * master on the same pins is not a design, it is a race.
 *
 * **Forced mode, not normal mode.** Wake, take one measurement, return to
 * sleep. That is the right mode for a node that samples once a minute, and
 * it matters for *accuracy* rather than for power: a continuously converting
 * BME280 self-heats, and the temperature reading is the first casualty.
 * Phase 17 spent real time on a 1-4 C offset in the DS3231's die sensor for
 * exactly this class of reason; here it is avoidable by construction.
 *
 * **The compensation is the fiddly part, so it is pure and testable.** The
 * raw registers are meaningless without the per-part calibration constants,
 * and t_fine from the temperature step feeds both of the others -- so an
 * error in temperature quietly corrupts all three. bme280_compensate() takes
 * a calibration block and raw values and returns the three results, touching
 * no hardware, so `sensor selftest` exercises it on both QEMU targets with
 * no sensor present at all.
 */

#define BME280_ADDR_LOW   0x76u   /* SDO tied low -- the common strapping */
#define BME280_ADDR_HIGH  0x77u   /* SDO tied high */

typedef enum {
    BME280_PART_NONE = 0,
    BME280_PART_BMP280,      /* chip id 0x58 (0x56/0x57 on engineering samples) */
    BME280_PART_BME280,      /* chip id 0x60 -- the one with humidity */
} bme280_part_t;

/* The per-part calibration block, exactly as the datasheet names it. Read
 * once at init; these are trimming values burned in at the factory, not
 * state. */
typedef struct {
    uint16_t dig_T1;  int16_t dig_T2, dig_T3;
    uint16_t dig_P1;  int16_t dig_P2, dig_P3, dig_P4, dig_P5,
                              dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t  dig_H1;  int16_t dig_H2;  uint8_t dig_H3;
    int16_t  dig_H4, dig_H5;  int8_t dig_H6;
    bool     has_humidity;
} bme280_calib_t;

typedef struct {
    int32_t  temperature_c100;   /* degrees C x 100   -- 2194 is 21.94 C */
    uint32_t pressure_pa256;     /* pascals in Q24.8  -- >> 8 for whole Pa */
    uint32_t humidity_rh1024;    /* %RH in Q22.10     -- >> 10 for whole %RH */
    bool     have_humidity;
} bme280_reading_t;

/* Probes 0x76 then 0x77, identifies the part by its chip id, resets it, reads
 * the calibration and configures forced mode. Safe to call more than once.
 * Returns the part found, or BME280_PART_NONE. */
bme280_part_t bme280_init(void);

bme280_part_t bme280_part(void);
uint8_t       bme280_address(void);
const char   *bme280_part_name(void);
bool          bme280_is_detected(void);

/* One forced-mode measurement: wake the part, wait for it to finish, read the
 * raw registers, compensate. Blocks for the conversion (a few milliseconds at
 * the oversampling this driver sets), yielding while it waits. False when no
 * part is fitted or the bus transfer failed. */
bool bme280_read(bme280_reading_t *out);

/* The arithmetic, with no hardware anywhere near it: the datasheet's
 * fixed-point compensation formulas over a calibration block and the three
 * raw values. Public precisely so it can be tested without a sensor. */
void bme280_compensate(const bme280_calib_t *cal,
                       int32_t raw_temp, int32_t raw_press, int32_t raw_hum,
                       bme280_reading_t *out);

/* The calibration the fitted part reported, for `sensor` to show and for a
 * golden vector to be captured from a real part. */
const bme280_calib_t *bme280_calibration(void);

/* Runs the built-in vector through bme280_compensate() and checks it against
 * values computed independently (tools/bme280_reference.py, transcribed from
 * the datasheet rather than from this code). Returns the number of failures;
 * prints each when `report` is true. Needs no sensor and no bus. */
uint32_t bme280_selftest(bool report);

/* Q5: registers this part's measurements with `mqttd`, one topic each, but
 * only if a part was actually found. Called after bme280_init(). */
void bme280_register_sources(void);

/* The human-readable report behind `sensor` and /proc/sensors. */
void bme280_print_status(void);

#endif // DRIVERS_BME280_H
