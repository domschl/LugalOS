#ifndef DRIVERS_TMP117_H
#define DRIVERS_TMP117_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Texas Instruments TMP117 ±0.1°C NIST-Traceable Ultra-High Accuracy I2C Temperature Sensor.
 *
 * Datasheet: SNOSD82D
 * Addresses: 0x48 - 0x4B (default 0x48)
 * Device ID: 0x0117 (Reg 0x0F)
 * Resolution: 0.0078125 °C (1/128 °C per LSB, 16-bit two's complement)
 */

#define TMP117_ADDR_DEFAULT    0x48u
#define TMP117_REG_TEMP        0x00u
#define TMP117_REG_CONFIG      0x01u
#define TMP117_REG_THIGH       0x02u
#define TMP117_REG_TLOW        0x03u
#define TMP117_REG_DEVICE_ID   0x0Fu

#define TMP117_DEVICE_ID_VAL   0x0117u
#define TMP117_CONFIG_DEFAULT  0x0220u  /* Continuous conversion, 8 averages, 125 ms */

typedef struct {
    int32_t temp_c100;     /* Centi-degrees Celsius (2500 is 25.00 °C) */
    int16_t raw_temp;
    bool    data_ready;
    bool    high_alert;
    bool    low_alert;
} tmp117_reading_t;

bool     tmp117_init(void);
bool     tmp117_is_detected(void);
uint8_t  tmp117_address(void);
uint8_t  tmp117_bus(void);
bool     tmp117_read(tmp117_reading_t *out);

/* Pure arithmetic conversion: raw 16-bit signed to centi-degrees Celsius (0.01 °C) */
int32_t  tmp117_calc_temp_c100(int16_t raw_val);
uint32_t tmp117_selftest(bool report);

extern sensor_dev_t tmp117_sensor_dev;

#endif /* DRIVERS_TMP117_H */
