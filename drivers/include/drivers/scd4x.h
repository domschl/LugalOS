#ifndef DRIVERS_SCD4X_H
#define DRIVERS_SCD4X_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Sensirion SCD40 / SCD41 / SCD43 photoacoustic CO2 sensor.
 *
 * Measures true CO2 (400 - 5000 ppm), plus the sensor's own temperature and
 * relative humidity. The driver runs the chip in periodic mode (a new sample
 * every 5 s) and the hub reads the latest one at its own period. The
 * temperature and humidity are those of the air at the sensor, a little above
 * the room's from the chip's self-heating, so they are kept in the reading
 * and not offered as hub channels; the hub's CO2 channel is the one output.
 */

#define SCD4X_ADDR           0x62u

typedef struct {
    uint16_t co2_ppm;        /* CO2, ppm */
    int32_t  temp_c100;      /* sensor temperature, 0.01 degC */
    int32_t  rh_c100;        /* relative humidity, 0.01 %RH */
} scd4x_reading_t;

/* Probes 0x62, reads the variant word, starts periodic measurement. */
bool scd4x_init(void);

bool        scd4x_is_detected(void);
uint8_t     scd4x_address(void);
const char *scd4x_part_name(void);   /* "scd40", "scd41", "scd43" */

/* Reads the newest measurement; waits up to 6 s for the first one after init. */
bool scd4x_read(scd4x_reading_t *out);

/* Ambient pressure compensation, in Pa (700 - 1200 hPa accepted by the chip). */
bool scd4x_set_ambient_pressure(int32_t pa);

/* Sensirion CRC-8 (polynomial 0x31, init 0xFF). */
uint8_t scd4x_crc8(const uint8_t *data, uint32_t len);

/* Raw-word conversions, pure integer. */
int32_t scd4x_temp_c100(uint16_t raw);
int32_t scd4x_rh_c100(uint16_t raw);

/* Selftest against the datasheet's example vectors. */
uint32_t scd4x_selftest(bool report);

bool        scd4x_cached(scd4x_reading_t *out, uint32_t *age_s);
uint32_t    scd4x_read_count(void);
uint32_t    scd4x_fail_count(void);
const char *scd4x_last_failure(void);

void scd4x_print_status(void);

extern sensor_dev_t scd4x_sensor_dev;

#endif /* DRIVERS_SCD4X_H */
