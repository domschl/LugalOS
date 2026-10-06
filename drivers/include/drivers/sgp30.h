#ifndef DRIVERS_SGP30_H
#define DRIVERS_SGP30_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Sensirion SGP30 Multi-Pixel Gas Sensor.
 * Phase 46, plan/phase46_sensor_framework.md §4.8 (Milestone 46.8).
 *
 * Measures:
 * - Equivalent Carbon Dioxide (eCO2) in ppm (400 - 60000 ppm)
 * - Total Volatile Organic Compounds (TVOC) in ppb (0 - 60000 ppb)
 *
 * Supports cross-sensor absolute humidity compensation via 0x2061 command.
 */

#define SGP30_ADDR           0x58u

typedef struct {
    uint16_t eco2_ppm;       /* Equivalent CO2 in ppm (400 - 60000) */
    uint16_t tvoc_ppb;       /* Total VOC in ppb (0 - 60000) */
    uint16_t feature_set;    /* SGP30 feature set version */
} sgp30_reading_t;

/* Probes 0x58, verifies feature set, sends IAQ init (0x2003) */
bool sgp30_init(void);

bool        sgp30_is_detected(void);
uint8_t     sgp30_address(void);
const char *sgp30_part_name(void);

/* Reads current eCO2 and TVOC measurements */
bool sgp30_read(sgp30_reading_t *out);

/* Computes and writes absolute humidity compensation (fixed-point 8.8 g/m^3) */
bool sgp30_set_absolute_humidity(int32_t temp_c100, int32_t rh_cpercent);

/* Sensirion CRC-8 checksum calculation */
uint8_t sgp30_crc8(const uint8_t *data, uint32_t len);

/* Selftest against golden vectors (tools/sgp30_reference.py) */
uint32_t sgp30_selftest(bool report);

/* Cache and diagnostics */
bool        sgp30_cached(sgp30_reading_t *out, uint32_t *age_s);
uint32_t    sgp30_read_count(void);
uint32_t    sgp30_fail_count(void);
const char *sgp30_last_failure(void);

void sgp30_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t sgp30_sensor_dev;

#endif /* DRIVERS_SGP30_H */
