#ifndef DRIVERS_I2C_REG_H
#define DRIVERS_I2C_REG_H

#include <stdint.h>
#include <stdbool.h>

/* I2C Register & Command Helpers (Layer 1, phase 46 §3).
 *
 * Provides standard register read/write and command transfer operations
 * layered on top of the Category E bus arbiter i2c_xfer() (drivers/i2c_bus.h).
 *
 * Device drivers in M-mode call these functions rather than open-coding byte
 * arrays and buffer marshalling. All transfers honor the bus size limits
 * (I2C_XFER_WMAX and I2C_XFER_RMAX) and repeated-START semantics.
 */

/* Single-byte register read and write */
bool i2c_reg_read_u8(uint8_t addr, uint8_t reg, uint8_t *val);
bool i2c_reg_write_u8(uint8_t addr, uint8_t reg, uint8_t val);

/* 16-bit register read (Big-Endian and Little-Endian) */
bool i2c_reg_read_u16_be(uint8_t addr, uint8_t reg, uint16_t *val);
bool i2c_reg_read_u16_le(uint8_t addr, uint8_t reg, uint16_t *val);

/* 16-bit register write (Big-Endian and Little-Endian) */
bool i2c_reg_write_u16_be(uint8_t addr, uint8_t reg, uint16_t val);
bool i2c_reg_write_u16_le(uint8_t addr, uint8_t reg, uint16_t val);

/* Multi-byte register block read and write */
bool i2c_reg_read_bytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len);
bool i2c_reg_write_bytes(uint8_t addr, uint8_t reg, const uint8_t *buf, uint32_t len);

/* Command transfers (no register prefix, or custom command sequences) */
bool i2c_cmd_write(uint8_t addr, const uint8_t *cmd, uint32_t len);
bool i2c_cmd_read(uint8_t addr, const uint8_t *cmd, uint32_t cmd_len, uint8_t *buf, uint32_t len);

#endif /* DRIVERS_I2C_REG_H */
