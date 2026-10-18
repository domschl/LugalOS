#ifndef DRIVERS_INA226_H
#define DRIVERS_INA226_H

#include <stdbool.h>
#include <stdint.h>
#include "drivers/sensor.h"

/* TI INA226 current and bus-voltage monitor (0x40-0x4F).
 *
 * The board's own part (the Tab5's U31 at 0x41, on the 2S battery) uses the
 * board's shunt and sign (CONFIG_INA226_SHUNT_UOHM, CONFIG_INA226_INVERT)
 * and feeds the board battery's charge estimate (drivers/battery.h); a module
 * found anywhere else is taken as the common breakout with a 0.1 Ohm shunt.
 *
 * Current is computed from the shunt-voltage register (2.5 uV/LSB), not the
 * current register, so no calibration value has to be programmed and the
 * shunt is one number here. Bus voltage is 1.25 mV/LSB. */

#define INA226_REG_CONFIG  0x00u
#define INA226_REG_SHUNT   0x01u
#define INA226_REG_BUS     0x02u
#define INA226_REG_MFG_ID  0xFEu
#define INA226_REG_DIE_ID  0xFFu
#define INA226_MFG_ID      0x5449u   /* "TI": the HDC1080 reads the same here */
#define INA226_DIE_ID      0x2260u   /* which tells the two apart */

/* 16 averages, 1.1 ms bus and shunt conversions, both continuous: a new
 * value every 35 ms, averaged over it. */
#define INA226_CONFIG      0x4527u

typedef struct {
    int32_t mv;     /* bus voltage */
    int32_t ma;     /* signed; + into the battery for the board's part */
    int32_t mw;
} ina226_reading_t;

bool ina226_is_detected(void);
bool ina226_read(ina226_reading_t *out);

/* Pure conversions, for the selftest. */
int32_t ina226_calc_mv(uint16_t bus_raw);
int32_t ina226_calc_ma(int16_t shunt_raw, uint32_t shunt_uohm);

uint32_t ina226_selftest(bool report);

extern sensor_dev_t ina226_sensor_dev;

#endif /* DRIVERS_INA226_H */
