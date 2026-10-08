#ifndef DRIVERS_CCS811_H
#define DRIVERS_CCS811_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* AMS CCS811 Ultra-Low Power Digital Gas Sensor for Indoor Air Quality Monitoring.
 * Phase 46, plan/phase46_sensor_framework.md §4.7 (Milestone 46.7).
 *
 * Measures:
 * - Equivalent Carbon Dioxide (eCO2) in ppm (parts-per-million)
 * - Total Volatile Organic Compounds (TVOC) in ppb (parts-per-billion)
 *
 * Supports cross-sensor environmental baseline compensation via ENV_DATA (reg 0x05).
 */

#define CCS811_ADDR_LOW      0x5Au  /* ADDR pin tied to GND (default) */
#define CCS811_ADDR_HIGH     0x5Bu  /* ADDR pin tied to VDD */
#define CCS811_HW_ID         0x81u

typedef enum {
    CCS811_DRIVE_MODE_IDLE   = 0x00u, /* Idle (no measurements) */
    CCS811_DRIVE_MODE_1S     = 0x10u, /* Constant power, measurement every 1s */
    CCS811_DRIVE_MODE_10S    = 0x20u, /* Pulse heating, measurement every 10s */
    CCS811_DRIVE_MODE_60S    = 0x30u, /* Low power pulse, measurement every 60s */
    CCS811_DRIVE_MODE_250MS  = 0x40u, /* Constant power, raw data every 250ms */
} ccs811_drive_mode_t;

typedef struct {
    uint16_t eco2_ppm;       /* Equivalent CO2 in ppm (400 - 8192) */
    uint16_t tvoc_ppb;       /* Total VOC in ppb (0 - 1187) */
    uint8_t  status;         /* STATUS register byte */
    uint8_t  error_id;       /* ERROR_ID register byte if error asserted */
} ccs811_reading_t;

/* Probes 0x5A/0x5B, boots application if in bootloader, configures drive mode */
bool ccs811_init(void);

bool        ccs811_is_detected(void);
uint8_t     ccs811_address(void);
const char *ccs811_part_name(void);

/* Reads current eCO2 and TVOC measurements */
bool ccs811_read(ccs811_reading_t *out);

/* Writes ambient temperature (0.01 C) and relative humidity (0.01 %RH) to ENV_DATA */
bool ccs811_set_env_data(int32_t temp_c100, int32_t rh_cpercent);

/* Encodes temperature and humidity into 4-byte ENV_DATA payload */
void ccs811_encode_env_data(int32_t temp_c100, int32_t rh_cpercent, uint8_t out[4]);

/* Baseline register operations (0x11) */
bool ccs811_get_baseline(uint16_t *baseline_out);
bool ccs811_set_baseline(uint16_t baseline);

/* Selftest against golden vectors (tools/ccs811_reference.py) */
uint32_t ccs811_selftest(bool report);

/* Cache and diagnostics */
bool        ccs811_cached(ccs811_reading_t *out, uint32_t *age_s);
uint32_t    ccs811_read_count(void);
uint32_t    ccs811_fail_count(void);
const char *ccs811_last_failure(void);

void ccs811_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t ccs811_sensor_dev;

#endif /* DRIVERS_CCS811_H */
