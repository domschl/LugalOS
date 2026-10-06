#ifndef DRIVERS_SENSOR_H
#define DRIVERS_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

/* Environmental Sensor Device-Class Contract (Category D, phase 46 §4).
 *
 * All environmental and light sensors in LugalOS implement this interface.
 *
 * ## Zero Floating-Point Rule
 *
 * In accordance with LugalOS's kernel invariant (mstatus.FS = 0), all physical
 * quantities are represented as signed 32-bit integers in fixed-point metric
 * units:
 *
 *   SENSOR_CHAN_TEMP:     0.01 °C      (centi-degrees: 2150 is 21.50 °C)
 *   SENSOR_CHAN_PRESSURE: 1 Pa         (Pascals: 101325 is 1013.25 hPa)
 *   SENSOR_CHAN_HUMIDITY: 0.01 %RH     (centi-percent: 5520 is 55.20 %RH)
 *   SENSOR_CHAN_LUX:      0.01 Lux     (centi-Lux: 45000 is 450.00 Lux)
 *   SENSOR_CHAN_ECO2:     1 ppm        (parts per million: 420 is 420 ppm)
 *   SENSOR_CHAN_TVOC:     1 ppb        (parts per billion: 125 is 125 ppb)
 *   SENSOR_CHAN_GAS_RES:  1 Ohm        (Ohms: 125400 is 125.4 kOhm)
 */

typedef enum {
    SENSOR_CHAN_TEMP = 0,
    SENSOR_CHAN_PRESSURE,
    SENSOR_CHAN_HUMIDITY,
    SENSOR_CHAN_LUX,
    SENSOR_CHAN_ECO2,
    SENSOR_CHAN_TVOC,
    SENSOR_CHAN_GAS_RES,
    SENSOR_CHAN_MAX
} sensor_chan_t;

struct sensor_dev;

typedef struct sensor_ops {
    /* Probes chip ID, loads factory calibration, configures mode.
     * Returns true if part is detected and ready. */
    bool (*init)(struct sensor_dev *dev);

    /* Triggers one conversion, waits/yields for completion, updates raw/compensated state.
     * Returns true on successful measurement. */
    bool (*sample)(struct sensor_dev *dev);

    /* Retrieves a standard metric value for channel chan in fixed-point.
     * Returns true if the channel is supported and a valid reading exists. */
    bool (*get_value)(struct sensor_dev *dev, sensor_chan_t chan, int32_t *out_val);

    /* Pure arithmetic selftest against golden reference vectors (no bus/hardware needed).
     * Returns number of failed cases (0 = success). */
    uint32_t (*selftest)(bool report);
} sensor_ops_t;

typedef struct sensor_dev {
    const char         *name;       /* e.g. "bme280", "bme680", "tsl2591" */
    uint8_t             addr;       /* Detected I2C address */
    uint32_t            chan_mask;  /* Bitmask of (1u << SENSOR_CHAN_*) supported */
    const sensor_ops_t *ops;
    void               *priv;       /* Driver-specific context / calibration */
} sensor_dev_t;

/* String representation of a channel name for MQTT topics and /proc/sensors */
const char *sensor_chan_name(sensor_chan_t chan);

/* Number of decimal digits for fixed-point printing (e.g. 2 for centi-units) */
uint8_t sensor_chan_decimals(sensor_chan_t chan);

#endif /* DRIVERS_SENSOR_H */
