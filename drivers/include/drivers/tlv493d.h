#ifndef DRIVERS_TLV493D_H
#define DRIVERS_TLV493D_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Infineon TLV493D-A1B6 3D magnetic sensor.
 *
 * Bx, By, Bz in 12 bits at 0.098 mT/LSB (+-130 mT linear range) and a die
 * temperature. The chip sleeps in power-down between samples: a sample writes
 * master-controlled mode, waits out one conversion, reads, and writes power-down
 * again, so the average supply current is the 7 nA of power-down.
 * The sensor offers no ID register; detection is the address answering a
 * 10-byte read after the I2C reset.
 */

#define TLV493D_ADDR         0x5Eu   /* ADDR pin level at the reset picks 0x5E or 0x1F */

typedef struct {
    int32_t bx_nt, by_nt, bz_nt;   /* nT, signed */
    int32_t temp_c10;              /* die temperature, 0.1 degC */
} tlv493d_reading_t;

bool        tlv493d_init(void);
bool        tlv493d_is_detected(void);
uint8_t     tlv493d_address(void);
const char *tlv493d_part_name(void);

bool tlv493d_read(tlv493d_reading_t *out);

/* Pure integer conversions from the 12-bit register fields. */
int32_t tlv493d_field_nt(int16_t raw12);
int32_t tlv493d_temp_c10(int16_t raw12);

/* Odd parity over the four write bytes, set in bit 7 of byte 1. */
void tlv493d_set_parity(uint8_t w[4]);

uint32_t tlv493d_selftest(bool report);

bool        tlv493d_cached(tlv493d_reading_t *out, uint32_t *age_s);
uint32_t    tlv493d_read_count(void);
uint32_t    tlv493d_fail_count(void);
const char *tlv493d_last_failure(void);

void tlv493d_print_status(void);

extern sensor_dev_t tlv493d_sensor_dev;

#endif /* DRIVERS_TLV493D_H */
