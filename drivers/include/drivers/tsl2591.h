#ifndef DRIVERS_TSL2591_H
#define DRIVERS_TSL2591_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* AMS TSL2591 High Dynamic Range Light-to-Digital Converter.
 * Phase 46, plan/phase46_sensor_framework.md §4.6 (Milestone 46.6).
 *
 * Measures ambient light illuminance (Lux) across a 600,000,000:1 dynamic range
 * using dual photodiodes (broadband visible+IR and infrared-only).
 *
 * Pure fixed-point / integer calculation in centi-Lux (0.01 Lux) in accordance
 * with LugalOS's mstatus.FS = 0 kernel invariant.
 */

#define TSL2591_ADDR       0x29u
#define TSL2591_CHIP_ID    0x50u

typedef enum {
    TSL2591_GAIN_LOW  = 0x00u, /* 1x */
    TSL2591_GAIN_MED  = 0x10u, /* 25x (default) */
    TSL2591_GAIN_HIGH = 0x20u, /* 428x */
    TSL2591_GAIN_MAX  = 0x30u, /* 9876x */
} tsl2591_gain_t;

typedef enum {
    TSL2591_INTEG_100MS = 0x00u, /* 100 ms */
    TSL2591_INTEG_200MS = 0x01u, /* 200 ms */
    TSL2591_INTEG_300MS = 0x02u, /* 300 ms */
    TSL2591_INTEG_400MS = 0x03u, /* 400 ms */
    TSL2591_INTEG_500MS = 0x04u, /* 500 ms */
    TSL2591_INTEG_600MS = 0x05u, /* 600 ms */
} tsl2591_integ_t;

typedef struct {
    uint16_t ch0;        /* Broadband visible + IR */
    uint16_t ch1;        /* Infrared only */
    int32_t  lux_c100;   /* Illuminance in 0.01 Lux (centi-Lux) */
} tsl2591_reading_t;

/* Probes 0x29, verifies chip ID 0x50, configures default gain and integration time */
bool tsl2591_init(void);

bool        tsl2591_is_detected(void);
uint8_t     tsl2591_address(void);
const char *tsl2591_part_name(void);

/* Triggers an on-demand integration cycle and updates reading */
bool tsl2591_read(tsl2591_reading_t *out);

/* Pure integer Lux calculation from raw CH0 and CH1 ADC counts */
int32_t tsl2591_compensate_lux(uint16_t ch0, uint16_t ch1, uint16_t gain_mult, uint16_t int_time_ms);

/* Selftest against golden reference vectors (tools/tsl2591_reference.py) */
uint32_t tsl2591_selftest(bool report);

/* Cache and diagnostics */
bool        tsl2591_cached(tsl2591_reading_t *out, uint32_t *age_s);
uint32_t    tsl2591_read_count(void);
uint32_t    tsl2591_fail_count(void);
const char *tsl2591_last_failure(void);

void tsl2591_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t tsl2591_sensor_dev;

#endif /* DRIVERS_TSL2591_H */
