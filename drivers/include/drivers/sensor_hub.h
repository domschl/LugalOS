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

#define SENSOR_HUB_MAX_DEVS     8u
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

#endif /* DRIVERS_SENSOR_HUB_H */
