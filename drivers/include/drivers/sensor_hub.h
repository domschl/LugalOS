#ifndef DRIVERS_SENSOR_HUB_H
#define DRIVERS_SENSOR_HUB_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Sensor Hub & Centralized Cache (Category D, phase 46 §5).
 *
 * Coordinates environmental and light sensors across LugalOS:
 * - Autonomous background sampler task
 * - Central cache with staleness tracking (age_s)
 * - Standalone exponential moving average (EMA) filtering
 * - Feed for /proc/sensors, shell 'sensor' command, and mqttd
 */

#define SENSOR_HUB_MAX_DEVS     10u
#define SENSOR_HUB_DEFAULT_PERIOD_S 60u

/* Registers a sensor driver instance with the hub */
bool sensor_hub_register(sensor_dev_t *dev);

/* Probes and initializes all registered sensor drivers */
void sensor_hub_init(void);

/* Number of active, detected sensor devices */
uint32_t      sensor_hub_device_count(void);
sensor_dev_t *sensor_hub_device_get(uint32_t idx);

/* Runs one conversion cycle across all active sensors and updates cache */
void sensor_hub_sample_all(void);

/* Starts the autonomous sampler task at period_s (0 uses default 60s) */
int      sensor_hub_sampler_start(uint32_t period_s);
uint32_t sensor_hub_sample_period_s(void);

/* Cached read from the primary sensor providing chan, without taking the I2C bus */
bool sensor_hub_get(sensor_chan_t chan, int32_t *out_val, uint32_t *age_s);

/* Cached read of EMA filtered value */
bool sensor_hub_get_filtered(sensor_chan_t chan, int32_t *out_val);

/* Cached read from a specific sensor device */
bool sensor_hub_get_dev(const sensor_dev_t *dev, sensor_chan_t chan, int32_t *out_val, uint32_t *age_s);

/* Diagnostic selftest running all drivers' math tests against golden vectors */
uint32_t sensor_hub_selftest(bool report);

/* Publishes the hub's channels through mqttd: one source per channel any detected device provides,
 * named by sensor_chan_name() ("lux", "eco2", ...), read from the hub's cache. A channel already
 * registered by name -- the BME280 registers its own, with its own rules, before this runs -- is left
 * alone. Does nothing without a broker: mqttd only publishes what it was told about once it runs. */
void sensor_hub_register_sources(void);

/* Human-readable status report for the shell 'sensor' command */
void sensor_hub_print_status(void);

/* Ground-truth cross-calibration status (Phase 46 §7.5) */
bool    sensor_hub_is_mox_contaminated(void);
bool    sensor_hub_is_fresh_air_verified(void);
int32_t sensor_hub_co2_ratio_pct(void);

/* Reload cached node altitude from identity store */
void    sensor_hub_altitude_reload(void);

/* Multi-sensor calibration blob stored in identity store (IDSTORE_FIELD_SENSOR_CAL) */
#define SENSOR_CAL_MAGIC   0x43u  /* 'C' */
#define SENSOR_CAL_VERSION 1u

typedef struct sensor_cal_blob {
    uint8_t  magic;          /* SENSOR_CAL_MAGIC */
    uint8_t  version;        /* SENSOR_CAL_VERSION */
    uint16_t flags;          /* 1 = valid, 2 = fresh_air_verified */
    uint16_t sgp30_eco2_base;
    uint16_t sgp30_tvoc_base;
    uint16_t ccs811_base;
    uint16_t mics6814_r0_nh3;
    uint16_t mics6814_r0_co;
    uint16_t mics6814_r0_no2;
    uint32_t bme680_r_base;
} __attribute__((packed)) sensor_cal_blob_t;

/* Sensor calibration operations */
bool sensor_hub_cal_get_current(sensor_cal_blob_t *out);
bool sensor_hub_cal_apply(const sensor_cal_blob_t *cal);
bool sensor_hub_cal_save(void);
bool sensor_hub_cal_restore(void);
bool sensor_hub_cal_clear(void);
bool sensor_hub_cal_clear_dev(const char *dev_name);
bool sensor_hub_cal_has_saved(sensor_cal_blob_t *out);

/* EEPROM calibration operations (AT24C32 at 0x57) */
#define SENSOR_CAL_EEPROM_ADDR 0x0F00u
bool sensor_hub_cal_eeprom_has_saved(sensor_cal_blob_t *out);
bool sensor_hub_cal_eeprom_save(void);
bool sensor_hub_cal_eeprom_restore(void);
bool sensor_hub_cal_eeprom_clear(void);

#endif /* DRIVERS_SENSOR_HUB_H */

