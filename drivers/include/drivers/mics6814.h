#ifndef DRIVERS_MICS6814_H
#define DRIVERS_MICS6814_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Grove Multichannel Gas Sensor V1.0 (MiCS-6814).
 * Phase 46, plan/phase46_sensor_framework.md.
 *
 * Senses:
 * - Carbon Monoxide (CO) in 0.01 ppm (centi-ppm)
 * - Nitrogen Dioxide (NO2) in 0.01 ppm (centi-ppm)
 * - Ammonia (NH3) in 0.01 ppm (centi-ppm)
 *
 * Companion ATmega MCU at I2C address 0x04 (default).
 */

#define MICS6814_DEFAULT_ADDR   0x04u

typedef struct {
    uint16_t r0_nh3;        /* Baseline resistance / ADC */
    uint16_t r0_co;
    uint16_t r0_no2;
    uint16_t rs_nh3;        /* Current sensing resistance / ADC */
    uint16_t rs_co;
    uint16_t rs_no2;
    uint16_t ratio_nh3_q10; /* Rs/R0 * 1024 (Q10 fixed-point) */
    uint16_t ratio_co_q10;
    uint16_t ratio_no2_q10;
    int32_t  co_c_ppm;      /* Carbon monoxide in 0.01 ppm */
    int32_t  no2_c_ppm;     /* Nitrogen dioxide in 0.01 ppm */
    int32_t  nh3_c_ppm;     /* Ammonia in 0.01 ppm */
    uint8_t  fw_version;    /* 1 or 2 */
    bool     is_warming_up; /* Preheating phase (first 10 minutes) */
    uint16_t warmup_remaining_s;
} mics6814_reading_t;

/* Standard MOX stabilization window (10 minutes from heater on) */
#define MICS6814_WARMUP_PERIOD_S 600u

/* Probes 0x04, detects firmware version (1 or 2), turns on heater */
bool        mics6814_init(void);
bool        mics6814_is_detected(void);
uint8_t     mics6814_address(void);
const char *mics6814_part_name(void);
bool        mics6814_is_warming_up(uint32_t *remaining_s);

/* Reads current channel values and calculates gas concentrations */
bool        mics6814_read(mics6814_reading_t *out);

/* Baseline R0 resistance get/set */
bool        mics6814_get_r0(uint16_t *r0_nh3, uint16_t *r0_co, uint16_t *r0_no2);
bool        mics6814_set_r0(uint16_t r0_nh3, uint16_t r0_co, uint16_t r0_no2);

/* Cache and diagnostics */
bool        mics6814_cached(mics6814_reading_t *out, uint32_t *age_s);
uint32_t    mics6814_read_count(void);
uint32_t    mics6814_fail_count(void);
const char *mics6814_last_failure(void);

/* Arithmetic selftest against reference vectors */
uint32_t    mics6814_selftest(bool report);

/* Diagnostic and status report */
void        mics6814_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t mics6814_sensor_dev;

#endif /* DRIVERS_MICS6814_H */
