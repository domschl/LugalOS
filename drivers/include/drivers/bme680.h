#ifndef DRIVERS_BME680_H
#define DRIVERS_BME680_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Bosch BME680 environmental & gas sensor (Phase 46, plan/phase46_sensor_framework.md).
 *
 * Four measurements in one sensor: Temperature, Pressure, Humidity, and
 * metal-oxide (MOX) Gas Resistance (VOC).
 *
 * ## Zero Floating-Point Rule
 *
 * In accordance with LugalOS's kernel invariant (mstatus.FS = 0), all physical
 * quantities and calibration compensation formulas are implemented in pure
 * fixed-point and integer arithmetic without hardware FPU instructions or
 * soft-float libm calls.
 *
 * ## Blob-Free Architecture
 *
 * Bosch provides a closed-source binary blob ("BSEC") for air quality estimation.
 * LugalOS strictly rejects all binary blobs. The physical gas resistance across
 * the heated MOX plate is computed directly in Ohms using the public integer
 * formulas and lookup tables documented in the Bosch datasheet §3.3–3.4.
 *
 * ## Forced Mode
 *
 * The sensor runs in forced mode: wake, heat the gas plate for ~150 ms at ~320 °C,
 * sample T, P, H and gas resistance, and return to sleep.
 */

#define BME680_ADDR_LOW   0x76u   /* SDO pulled to GND */
#define BME680_ADDR_HIGH  0x77u   /* SDO pulled to VDD */
#define BME680_CHIP_ID    0x61u

/* The per-part calibration parameters read from sensor NVM */
typedef struct {
    uint16_t par_t1;
    int16_t  par_t2;
    int8_t   par_t3;

    uint16_t par_p1;
    int16_t  par_p2;
    int8_t   par_p3;
    int16_t  par_p4;
    int16_t  par_p5;
    int8_t   par_p6;
    int8_t   par_p7;
    int16_t  par_p8;
    int16_t  par_p9;
    uint8_t  par_p10;

    uint16_t par_h1;
    uint16_t par_h2;
    int8_t   par_h3;
    int8_t   par_h4;
    int8_t   par_h5;
    uint8_t  par_h6;
    int8_t   par_h7;

    int8_t   par_gh1;
    int16_t  par_gh2;
    int8_t   par_gh3;

    uint8_t  res_heat_range;
    int8_t   res_heat_val;
    int8_t   range_sw_err;
} bme680_calib_t;

typedef struct {
    int32_t  temperature_c100;   /* degrees C x 100   -- 2150 is 21.50 C */
    int32_t  pressure_pa;        /* pascals           -- 101325 is 1013.25 hPa */
    int32_t  humidity_cpercent;  /* %RH x 100         -- 4360 is 43.60 %RH */
    int32_t  gas_res_ohm;        /* MOX resistance    -- in Ohms */
    bool     gas_valid;
    bool     heat_stab;
} bme680_reading_t;

/* Probes 0x76 then 0x77, verifies chip ID 0x61, resets part, reads calibration,
 * configures heater profile (320 C for 150 ms) and forced mode.
 * Returns true if BME680 was found. */
bool bme680_init(void);

bool        bme680_is_detected(void);
uint8_t     bme680_address(void);
const char *bme680_part_name(void);

/* Performs one forced-mode measurement and updates cached state. */
bool bme680_read(bme680_reading_t *out);

/* Pure compensation arithmetic (no bus access, no hardware). */
void bme680_compensate(const bme680_calib_t *cal,
                       int32_t raw_temp, int32_t raw_press,
                       int32_t raw_hum, int32_t raw_gas,
                       uint8_t gas_range, bme680_reading_t *out);

uint8_t bme680_calc_res_heat(uint16_t target_temp, int16_t amb_temp, const bme680_calib_t *cal);
uint8_t bme680_calc_gas_wait(uint16_t dur_ms);

const bme680_calib_t *bme680_calibration(void);

/* Pure arithmetic selftest against golden reference vectors (tools/bme680_reference.py).
 * Returns number of failed checks (0 on success). */
uint32_t bme680_selftest(bool report);

/* Cache and diagnostic counters */
bool        bme680_cached(bme680_reading_t *out, uint32_t *age_s);
uint32_t    bme680_read_count(void);
uint32_t    bme680_fail_count(void);
const char *bme680_last_failure(void);

void bme680_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t bme680_sensor_dev;

#endif /* DRIVERS_BME680_H */
