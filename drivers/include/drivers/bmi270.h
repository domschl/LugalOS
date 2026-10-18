#ifndef DRIVERS_BMI270_H
#define DRIVERS_BMI270_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/sensor.h"

/* Bosch BMI270 6-axis IMU: accelerometer +-4 g at 100 Hz, gyroscope
 * +-1000 deg/s at 200 Hz, both in performance mode.
 *
 * The chip measures nothing until its configuration microcode
 * (drivers/bmi270_config.c, Bosch's, BSD-3) has been uploaded: init does
 * that, then checks INTERNAL_STATUS. Found at a board part named "bmi270"
 * (the Tab5's 0x68), or at 0x69 on any bus. 0x68 is not searched otherwise,
 * because a DS3231 module answers there and init writes registers. */

#define BMI270_REG_CHIP_ID     0x00u
#define BMI270_CHIP_ID         0x24u
#define BMI270_REG_DATA        0x0Cu    /* acc x/y/z, then gyr x/y/z, int16 LE */
#define BMI270_REG_INT_STATUS  0x21u    /* bits 3:0: 1 = initialised */
#define BMI270_REG_ACC_CONF    0x40u
#define BMI270_REG_ACC_RANGE   0x41u
#define BMI270_REG_GYR_CONF    0x42u
#define BMI270_REG_GYR_RANGE   0x43u
#define BMI270_REG_INIT_CTRL   0x59u
#define BMI270_REG_INIT_ADDR0  0x5Bu
#define BMI270_REG_INIT_DATA   0x5Eu
#define BMI270_REG_PWR_CONF    0x7Cu
#define BMI270_REG_PWR_CTRL    0x7Du
#define BMI270_REG_CMD         0x7Eu

typedef struct {
    int32_t ax, ay, az;     /* mg */
    int32_t gx, gy, gz;     /* 0.1 deg/s */
} bmi270_reading_t;

bool bmi270_is_detected(void);
/* One reading straight from the chip (the bus, not the hub's cache). */
bool bmi270_read(bmi270_reading_t *out);

/* Upload the microcode again on the next read (a diagnostic, `imu reload`). */
void bmi270_reload(void);
bool bmi270_is_configured(void);

/* Pure conversions, for the selftest. */
int32_t bmi270_acc_mg(int16_t raw);
int32_t bmi270_gyr_ddps(int16_t raw);

/* Pitch and roll from gravity, in tenths of a degree. */
void bmi270_tilt(const bmi270_reading_t *r, int32_t *pitch_d10, int32_t *roll_d10);

uint32_t bmi270_selftest(bool report);

extern sensor_dev_t bmi270_sensor_dev;

#endif /* DRIVERS_BMI270_H */
