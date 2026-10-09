#ifndef DRIVERS_HDC1080_H
#define DRIVERS_HDC1080_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Texas Instruments HDC1080 High Accuracy Digital Humidity & Temperature Sensor.
 * Phase 46, plan/phase46_sensor_framework.md (Milestone 46.17).
 *
 * Characteristics:
 * - Fixed I2C Address: 0x40
 * - Manufacturer ID: 0x5449 ('TI')
 * - Device ID: 0x1050
 * - Temperature accuracy: +/- 0.2 C (typical)
 * - Relative humidity accuracy: +/- 2 %RH (typical)
 * - Strict zero-float integer math.
 */

#define HDC1080_I2C_ADDR         0x40u
#define HDC1080_MANUFACTURER_ID  0x5449u
#define HDC1080_DEVICE_ID        0x1050u

#define HDC1080_REG_TEMP         0x00u
#define HDC1080_REG_HUMIDITY     0x01u
#define HDC1080_REG_CONFIG       0x02u
#define HDC1080_REG_SERIAL_FIRST 0xFBu
#define HDC1080_REG_SERIAL_MID   0xFCu
#define HDC1080_REG_SERIAL_LAST  0xFDu
#define HDC1080_REG_MANUF_ID     0xFEu
#define HDC1080_REG_DEV_ID       0xFFu

/* Configuration register bits */
#define HDC1080_CONFIG_RST       (1u << 15)
#define HDC1080_CONFIG_HEAT      (1u << 13)
#define HDC1080_CONFIG_MODE_BOTH (1u << 12) /* 1 = sequential Temp + Hum */
#define HDC1080_CONFIG_BTST      (1u << 11)
#define HDC1080_CONFIG_TRES_14   (0u << 10)
#define HDC1080_CONFIG_TRES_11   (1u << 10)
#define HDC1080_CONFIG_HRES_14   (0u << 8)
#define HDC1080_CONFIG_HRES_11   (1u << 8)
#define HDC1080_CONFIG_HRES_8    (2u << 8)

typedef struct {
    int32_t  temp_c100;     /* Temperature in 0.01 C (e.g. 2530 = 25.30 C) */
    int32_t  humidity_rh1000;/* Humidity in 0.001 %RH (e.g. 50000 = 50.000 %RH) */
    int32_t  humidity_c100;  /* Humidity in 0.01 %RH (e.g. 5000 = 50.00 %RH) */
    uint16_t raw_temp;
    uint16_t raw_hum;
} hdc1080_reading_t;

/* Driver lifecycle */
bool        hdc1080_init(void);
bool        hdc1080_is_detected(void);
uint8_t     hdc1080_address(void);
const char *hdc1080_part_name(void);

/* Reads live temperature and humidity */
bool hdc1080_read(hdc1080_reading_t *out);

/* Cached readings and diagnostic counters */
bool        hdc1080_cached(hdc1080_reading_t *out, uint32_t *age_s);
uint32_t    hdc1080_read_count(void);
uint32_t    hdc1080_fail_count(void);
const char *hdc1080_last_failure(void);

/* Pure integer selftest against datasheet golden vectors */
uint32_t hdc1080_selftest(bool report);

/* Environmental Sensor Device-Class instance */
extern sensor_dev_t hdc1080_sensor_dev;

#endif /* DRIVERS_HDC1080_H */
