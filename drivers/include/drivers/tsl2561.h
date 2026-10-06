#ifndef DRIVERS_TSL2561_H
#define DRIVERS_TSL2561_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* AMS / TAOS TSL2561 Light-to-Digital Converter.
 * Phase 46, plan/phase46_sensor_framework.md §4.6 (Milestone 46.6).
 *
 * Measures ambient illuminance (Lux) using dual photodiodes (broadband visible+IR
 * and infrared-only).
 *
 * Pure fixed-point / integer calculation in centi-Lux (0.01 Lux) in accordance
 * with LugalOS's mstatus.FS = 0 kernel invariant.
 */

#define TSL2561_ADDR_LOW     0x29u
#define TSL2561_ADDR_FLOAT   0x39u
#define TSL2561_ADDR_HIGH    0x49u

typedef enum {
    TSL2561_GAIN_1X  = 0x00u,
    TSL2561_GAIN_16X = 0x10u,
} tsl2561_gain_t;

typedef enum {
    TSL2561_INTEG_13MS  = 0x00u, /* 13.7 ms */
    TSL2561_INTEG_101MS = 0x01u, /* 101 ms */
    TSL2561_INTEG_402MS = 0x02u, /* 402 ms */
} tsl2561_integ_t;

typedef struct {
    uint16_t ch0;        /* Broadband visible + IR */
    uint16_t ch1;        /* Infrared only */
    int32_t  lux_c100;   /* Illuminance in 0.01 Lux (centi-Lux) */
} tsl2561_reading_t;

/* Probes 0x39, 0x49, or 0x29, verifies chip ID and control register, configures default gain/timing */
bool tsl2561_init(void);

bool        tsl2561_is_detected(void);
uint8_t     tsl2561_address(void);
const char *tsl2561_part_name(void);

/* Triggers an on-demand integration cycle and updates reading */
bool tsl2561_read(tsl2561_reading_t *out);

/* Pure integer Lux calculation from raw CH0 and CH1 ADC counts */
int32_t tsl2561_compensate_lux(uint16_t ch0, uint16_t ch1, uint8_t gain, uint8_t integ);

/* Selftest against golden reference vectors (tools/tsl2561_reference.py) */
uint32_t tsl2561_selftest(bool report);

/* Cache and diagnostics */
bool        tsl2561_cached(tsl2561_reading_t *out, uint32_t *age_s);
uint32_t    tsl2561_read_count(void);
uint32_t    tsl2561_fail_count(void);
const char *tsl2561_last_failure(void);

void tsl2561_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t tsl2561_sensor_dev;

#endif /* DRIVERS_TSL2561_H */
