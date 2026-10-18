#ifndef DRIVERS_SENSOR_H
#define DRIVERS_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Environmental Sensor Device-Class Contract.
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
 *   SENSOR_CHAN_CO:       0.01 ppm     (centi-ppm: 120 is 1.20 ppm)
 *   SENSOR_CHAN_NO2:      0.01 ppm     (centi-ppm: 15 is 0.15 ppm)
 *   SENSOR_CHAN_NH3:      0.01 ppm     (centi-ppm: 68 is 0.68 ppm)
 *   SENSOR_CHAN_CO2:      1 ppm        (parts per million true CO2: 400 is 400 ppm)
 *   SENSOR_CHAN_PRESSURE_MSL: 1 Pa     (Sea-level pressure / QNH: 101325 is 1013.25 hPa)
 *   SENSOR_CHAN_AH:       0.01 g/m³    (Absolute humidity: 1125 is 11.25 g/m³)
 *   SENSOR_CHAN_DEW_POINT: 0.01 °C     (Dew point: 1520 is 15.20 °C)
 *   SENSOR_CHAN_IAQ:      1            (Index of Air Quality: 0-500)
 *
 * Power and motion (board parts: a battery monitor, an IMU):
 *
 *   SENSOR_CHAN_VOLTAGE:  1 mV         (7861 is 7.861 V)
 *   SENSOR_CHAN_CURRENT:  1 mA, signed (+ into the battery: -412 is 0.412 A discharging)
 *   SENSOR_CHAN_POWER:    1 mW, signed (same sign as the current)
 *   SENSOR_CHAN_BATTERY:  0.1 %        (state of charge: 715 is 71.5 %)
 *   SENSOR_CHAN_ACCEL_X/Y/Z: 1 mg, signed (1000 is 1 g)
 *   SENSOR_CHAN_GYRO_X/Y/Z:  0.1 deg/s, signed
 */

typedef enum {
    SENSOR_CHAN_TEMP = 0,
    SENSOR_CHAN_PRESSURE,
    SENSOR_CHAN_HUMIDITY,
    SENSOR_CHAN_LUX,
    SENSOR_CHAN_ECO2,
    SENSOR_CHAN_TVOC,
    SENSOR_CHAN_GAS_RES,
    SENSOR_CHAN_CO,
    SENSOR_CHAN_NO2,
    SENSOR_CHAN_NH3,
    SENSOR_CHAN_CO2,
    SENSOR_CHAN_PRESSURE_MSL,
    SENSOR_CHAN_AH,
    SENSOR_CHAN_DEW_POINT,
    SENSOR_CHAN_IAQ,
    SENSOR_CHAN_VOLTAGE,
    SENSOR_CHAN_CURRENT,
    SENSOR_CHAN_POWER,
    SENSOR_CHAN_BATTERY,
    SENSOR_CHAN_ACCEL_X,
    SENSOR_CHAN_ACCEL_Y,
    SENSOR_CHAN_ACCEL_Z,
    SENSOR_CHAN_GYRO_X,
    SENSOR_CHAN_GYRO_Y,
    SENSOR_CHAN_GYRO_Z,
    SENSOR_CHAN_MAX
} sensor_chan_t;

/* chan_mask is a uint32_t bitmask over these. */
_Static_assert(SENSOR_CHAN_MAX <= 32, "sensor_dev_t.chan_mask holds one bit per channel");

/* Measurement provenance and inference tier */
typedef enum {
    SENSOR_TIER_HW = 0,       /* Direct physical transducer (thermistor, piezoresistor, NDIR, photodiode) */
    SENSOR_TIER_INFERRED,     /* Inferred physics equations (Magnus, hypsometric, August-Roche-Magnus, IAQ) */
    SENSOR_TIER_FUSED,        /* Multi-sensor arbitrated consensus (e.g. MH-Z19B NDIR over MOX eCO2) */
} sensor_tier_t;

const char *sensor_tier_name(sensor_tier_t tier);

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
    uint8_t             bus;        /* Detected I2C bus index (0 or 1) */
    uint32_t            chan_mask;  /* Bitmask of (1u << SENSOR_CHAN_*) supported */
    /* How often the hub samples this device, in ms; 0 is the hub's own period
     * (SENSOR_HUB_DEFAULT_PERIOD_S, `sensor period`). A battery monitor or an
     * IMU sets its own; an environmental sensor leaves it 0. Read by the hub
     * after init(), so init() may set it from what it found. */
    uint32_t            period_ms;
    const sensor_ops_t *ops;
    void               *priv;       /* Driver-specific context / calibration */
} sensor_dev_t;

/* Channel metadata and uniform decimal scaling descriptor */
typedef struct {
    sensor_chan_t chan;
    const char   *name;        /* Machine key: "temperature", "pressure", "co2", etc. */
    const char   *unit_symbol; /* Display unit: "°C", "hPa", "%RH", "ppm", "ppb", "Ohm", "g/m³" */
    uint8_t       decimals;    /* Decimal places for display */
    int32_t       scale_div;   /* Divider from raw int32_t to standard units: 10^decimals */
    /* The /proc/sensor/<dev> key for the raw integer, unit in the name
     * ("voltage_mv"); NULL for the channels /proc/sensor spells by hand. */
    const char   *raw_key;
    bool          publish;     /* registered as an MQTT source (motion channels are not) */
} sensor_chan_desc_t;

/* Channel metadata accessors */
const sensor_chan_desc_t *sensor_chan_desc(sensor_chan_t chan);
const char               *sensor_chan_name(sensor_chan_t chan);
const char               *sensor_chan_unit(sensor_chan_t chan);
uint8_t                   sensor_chan_decimals(sensor_chan_t chan);
int32_t                   sensor_chan_scale_div(sensor_chan_t chan);

/* Formats a raw sensor channel value into a human-readable string with units.
 * Returns bytes written (excluding NUL). */
size_t sensor_format_channel(sensor_chan_t chan, int32_t raw_val, char *buf, size_t cap);

/* Derived meteorological and physical metric calculations (pure integer, zero-float) */
int32_t  sensor_calc_sea_level_pa(int32_t pa, int32_t t_c100, int32_t alt_m);
int32_t  sensor_calc_abs_humidity_c100(int32_t t_c100, int32_t rh_c100);
int32_t  sensor_calc_dew_point_c100(int32_t t_c100, int32_t rh_c100);
int32_t  sensor_calc_bme680_iaq(int32_t gas_res_ohm, int32_t t_c100, int32_t rh_c100, int32_t *inout_r_base);

/* Pure arithmetic selftest for derived calculations against golden vectors */
uint32_t sensor_derived_selftest(bool report);

#endif /* DRIVERS_SENSOR_H */
