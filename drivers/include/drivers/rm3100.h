#ifndef DRIVERS_RM3100_H
#define DRIVERS_RM3100_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* PNI RM3100 3-axis magneto-inductive magnetometer (I2C mode).
 *
 * Datasheet: RM3100 & RM2100 Sensor Suite User Manual.
 * Addresses: 0x20 - 0x23 (SA0 and SA1 pins).
 * Range +-800 uT; noise 15 nT at 200 cycle counts, lower at more. The
 * sensor has no offset to drift, which is why it is the better of the two
 * for watching the earth's field over hours.
 *
 * A sample is RM3100_AVG single measurements of all three axes at
 * RM3100_CYCLE_COUNT cycles each (about 13 ms each), averaged, in nT.
 */

#define RM3100_ADDR_BASE    0x20u
#define RM3100_ADDR_COUNT   4u

#define RM3100_REG_POLL     0x00u
#define RM3100_REG_CMM      0x01u
#define RM3100_REG_CCX      0x04u   /* CCX, CCY, CCZ: 16 bits each, big-endian */
#define RM3100_REG_MX       0x24u   /* 9 bytes: X, Y, Z, each 24 bits two's complement, big-endian */
#define RM3100_REG_STATUS   0x34u
#define RM3100_REG_HSHAKE   0x35u
#define RM3100_REG_REVID    0x36u

/* Cycle counts per axis: 400 is the top of the range the manual calls useful
 * (noise limits anything above) and gives 148 LSB/uT, 6.7 nT per LSB. */
#define RM3100_CYCLE_COUNT  400u
#define RM3100_AVG          8u

typedef struct {
    int32_t bx_nt, by_nt, bz_nt;   /* nT, signed, sensor axes */
} rm3100_reading_t;

bool        rm3100_init(void);
bool        rm3100_is_detected(void);
uint8_t     rm3100_address(void);
uint8_t     rm3100_bus(void);
uint8_t     rm3100_revision(void);
bool        rm3100_read(rm3100_reading_t *out);

/* The signed 24-bit result of one axis from its three bytes. */
int32_t     rm3100_unpack(const uint8_t b[3]);

/* nT from the sum of `n` raw results of one axis taken at `cycle_count`.
 * Gain is 0.3671 * cycle_count + 1.5 LSB/uT, which reproduces the manual's
 * Table 3-1 (20, 38 and 75 LSB/uT at 50, 100 and 200 cycle counts). */
int32_t     rm3100_field_nt(int32_t raw_sum, uint32_t n, uint32_t cycle_count);

uint32_t    rm3100_selftest(bool report);

extern sensor_dev_t rm3100_sensor_dev;

#endif /* DRIVERS_RM3100_H */
