#ifndef DRIVERS_MHZ19B_H
#define DRIVERS_MHZ19B_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* MH-Z19B NDIR Infrared Carbon Dioxide (CO2) Sensor.
 *
 * Senses:
 * - Carbon Dioxide (CO2) in 1 ppm (NDIR optical absorption, 0-2000 or 0-5000 ppm)
 * - Internal optical chamber temperature in 0.01 °C (centi-degrees)
 *
 * Hardware connection:
 * - 4.5V - 5.5V DC on Vin (connect to Pico Pin 40 VBUS)
 * - 3.3V TTL UART at 9600 8N1 (GP8 TX, GP9 RX on RP2350)
 */

#define MHZ19B_WARMUP_PERIOD_S  180u  /* 3 minutes (datasheet Table 1) */

typedef struct {
    int32_t  co2_ppm;           /* Carbon dioxide in 1 ppm */
    int32_t  temp_c100;         /* Chamber temperature in 0.01 °C */
    bool     is_warming_up;     /* Preheating phase (first 3 minutes) */
    uint16_t warmup_remaining_s;
} mhz19b_reading_t;

/* Initializes UART and probes the sensor with a 0x86 command */
bool        mhz19b_init(void);
bool        mhz19b_is_detected(void);
const char *mhz19b_part_name(void);
bool        mhz19b_is_warming_up(uint32_t *remaining_s);

/* Reads current CO2 concentration and temperature */
bool        mhz19b_read(mhz19b_reading_t *out);

/* Cache and diagnostics */
bool        mhz19b_cached(mhz19b_reading_t *out, uint32_t *age_s);
uint32_t    mhz19b_read_count(void);
uint32_t    mhz19b_fail_count(void);
const char *mhz19b_last_failure(void);

/* Commands */
bool        mhz19b_set_abc(bool enable);
bool        mhz19b_calibrate_zero(void);

/* Arithmetic and packet verification selftest */
uint32_t    mhz19b_selftest(bool report);

/* Diagnostic and status report */
void        mhz19b_print_status(void);

/* Sensor Device Contract instance (Category D) */
extern sensor_dev_t mhz19b_sensor_dev;

#endif /* DRIVERS_MHZ19B_H */
