#ifndef DRIVERS_MMC5983_H
#define DRIVERS_MMC5983_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* MEMSIC MMC5983MA 3-axis AMR magnetometer.
 *
 * Datasheet: MMC5983MA Rev A (4/3/2019).
 * Address: 0x30 (fixed). Product ID: 0x30 (reg 0x2F).
 * Range +-8 G, 18 bits, 16384 counts/G: 1 count is 6.1035 nT.
 *
 * Every sample is a set of SET/RESET pairs: a measurement after a SET pulse and
 * one after a RESET pulse carry the field with opposite signs and the bridge
 * offset with the same one, so half their difference is the field with the
 * offset and its temperature drift removed. That drift is larger than the
 * fluctuations of the earth's field, so this is the only mode the driver has.
 * The result is the mean over MMC5983_PAIRS pairs, in nT.
 */

#define MMC5983_ADDR       0x30u
#define MMC5983_PRODUCT_ID 0x30u

#define MMC5983_REG_XOUT0  0x00u   /* 7 bytes: X[17:10] X[9:2] Y.. Y.. Z.. Z.. then X[1:0] Y[1:0] Z[1:0] <<6,4,2 */
#define MMC5983_REG_TOUT   0x07u
#define MMC5983_REG_STATUS 0x08u
#define MMC5983_REG_CTRL0  0x09u
#define MMC5983_REG_CTRL1  0x0Au
#define MMC5983_REG_CTRL2  0x0Bu
#define MMC5983_REG_ID     0x2Fu

/* SET/RESET pairs per sample, as a power of two (the mean is then a shift). */
#define MMC5983_PAIRS_LOG2 2u

typedef struct {
    int32_t bx_nt, by_nt, bz_nt;   /* nT, signed, sensor axes */
    int32_t temp_c10;              /* die temperature, 0.1 degC; +-3 degC at best */
} mmc5983_reading_t;

bool        mmc5983_init(void);
bool        mmc5983_is_detected(void);
uint8_t     mmc5983_address(void);
uint8_t     mmc5983_bus(void);
bool        mmc5983_read(mmc5983_reading_t *out);

/* The 18-bit output of one measurement from the seven bytes at XOUT0. */
void        mmc5983_unpack(const uint8_t b[7], uint32_t raw[3]);

/* nT from the sum over `pairs` (a power of two) of (SET count - RESET count)
 * of one axis: half the difference is the field, 16384 counts per gauss. */
int32_t     mmc5983_field_nt(int32_t diff_sum, uint32_t pairs_log2);

/* 0.1 degC from the 8-bit output: 0 is -75 degC and 255 is +125 degC. */
int32_t     mmc5983_temp_c10(uint8_t raw);

uint32_t    mmc5983_selftest(bool report);

extern sensor_dev_t mmc5983_sensor_dev;

#endif /* DRIVERS_MMC5983_H */
