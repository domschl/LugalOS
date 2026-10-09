#ifndef DRIVERS_MCP9808_H
#define DRIVERS_MCP9808_H

#include <stdint.h>
#include <stdbool.h>
#include "drivers/sensor.h"

/* Microchip MCP9808 ±0.25°C High-Accuracy I2C Temperature Sensor.
 *
 * Datasheet: DS25095A
 * Addresses: 0x18 - 0x1F (default 0x18)
 * Manufacturer ID: 0x0054 (Reg 0x06)
 * Device ID: 0x0400 (Reg 0x07, upper byte 0x04)
 */

#define MCP9808_ADDR_DEFAULT    0x18u
#define MCP9808_REG_CONFIG      0x01u
#define MCP9808_REG_T_UPPER     0x02u
#define MCP9808_REG_T_LOWER     0x03u
#define MCP9808_REG_T_CRIT      0x04u
#define MCP9808_REG_T_AMBIENT   0x05u
#define MCP9808_REG_MANUF_ID    0x06u
#define MCP9808_REG_DEV_ID      0x07u
#define MCP9808_REG_RES         0x08u

#define MCP9808_MANUF_ID_VAL    0x0054u
#define MCP9808_DEV_ID_VAL      0x0400u
#define MCP9808_RES_0_0625      0x03u

typedef struct {
    int32_t  temp_c100;     /* Centi-degrees Celsius (2506 is 25.06 °C) */
    uint16_t raw_temp;
    bool     alert_crit;
    bool     alert_upper;
    bool     alert_lower;
} mcp9808_reading_t;

bool     mcp9808_init(void);
bool     mcp9808_is_detected(void);
uint8_t  mcp9808_address(void);
uint8_t  mcp9808_bus(void);
bool     mcp9808_read(mcp9808_reading_t *out);

/* Pure arithmetic conversion: raw 16-bit register to centi-degrees Celsius (0.01 °C) */
int32_t  mcp9808_calc_temp_c100(uint16_t raw_val);
uint32_t mcp9808_selftest(bool report);

extern sensor_dev_t mcp9808_sensor_dev;

#endif /* DRIVERS_MCP9808_H */
