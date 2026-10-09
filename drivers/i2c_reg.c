#include "drivers/i2c_reg.h"
#include "drivers/i2c_bus.h"
#include <string.h>

bool i2c_reg_read_u8_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t *val) {
    if (!val) return false;
    return i2c_xfer_bus(bus, addr, &reg, 1u, val, 1u);
}

bool i2c_reg_read_u8(uint8_t addr, uint8_t reg, uint8_t *val) {
    return i2c_reg_read_u8_bus(0, addr, reg, val);
}

bool i2c_reg_write_u8_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t w[2] = { reg, val };
    return i2c_xfer_bus(bus, addr, w, 2u, NULL, 0u);
}

bool i2c_reg_write_u8(uint8_t addr, uint8_t reg, uint8_t val) {
    return i2c_reg_write_u8_bus(0, addr, reg, val);
}

bool i2c_reg_read_u16_be_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t *val) {
    if (!val) return false;
    uint8_t b[2];
    if (!i2c_xfer_bus(bus, addr, &reg, 1u, b, 2u)) return false;
    *val = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    return true;
}

bool i2c_reg_read_u16_be(uint8_t addr, uint8_t reg, uint16_t *val) {
    return i2c_reg_read_u16_be_bus(0, addr, reg, val);
}

bool i2c_reg_read_u16_le_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t *val) {
    if (!val) return false;
    uint8_t b[2];
    if (!i2c_xfer_bus(bus, addr, &reg, 1u, b, 2u)) return false;
    *val = (uint16_t)(((uint16_t)b[1] << 8) | b[0]);
    return true;
}

bool i2c_reg_read_u16_le(uint8_t addr, uint8_t reg, uint16_t *val) {
    return i2c_reg_read_u16_le_bus(0, addr, reg, val);
}

bool i2c_reg_write_u16_be_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t val) {
    uint8_t w[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };
    return i2c_xfer_bus(bus, addr, w, 3u, NULL, 0u);
}

bool i2c_reg_write_u16_be(uint8_t addr, uint8_t reg, uint16_t val) {
    return i2c_reg_write_u16_be_bus(0, addr, reg, val);
}

bool i2c_reg_write_u16_le_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint16_t val) {
    uint8_t w[3] = { reg, (uint8_t)(val & 0xFFu), (uint8_t)(val >> 8) };
    return i2c_xfer_bus(bus, addr, w, 3u, NULL, 0u);
}

bool i2c_reg_write_u16_le(uint8_t addr, uint8_t reg, uint16_t val) {
    return i2c_reg_write_u16_le_bus(0, addr, reg, val);
}

bool i2c_reg_read_bytes_bus(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len) {
    if (!buf || len == 0) return false;
    return i2c_xfer_bus(bus, addr, &reg, 1u, buf, len);
}

bool i2c_reg_read_bytes(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len) {
    return i2c_reg_read_bytes_bus(0, addr, reg, buf, len);
}

bool i2c_reg_write_bytes_bus(uint8_t bus, uint8_t addr, uint8_t reg, const uint8_t *buf, uint32_t len) {
    if (len == 0) {
        return i2c_xfer_bus(bus, addr, &reg, 1u, NULL, 0u);
    }
    if (!buf || (len + 1u > I2C_XFER_WMAX)) return false;

    uint8_t w[I2C_XFER_WMAX];
    w[0] = reg;
    memcpy(&w[1], buf, len);
    return i2c_xfer_bus(bus, addr, w, len + 1u, NULL, 0u);
}

bool i2c_reg_write_bytes(uint8_t addr, uint8_t reg, const uint8_t *buf, uint32_t len) {
    return i2c_reg_write_bytes_bus(0, addr, reg, buf, len);
}

bool i2c_cmd_write_bus(uint8_t bus, uint8_t addr, const uint8_t *cmd, uint32_t len) {
    if (!cmd && len > 0) return false;
    return i2c_xfer_bus(bus, addr, cmd, len, NULL, 0u);
}

bool i2c_cmd_write(uint8_t addr, const uint8_t *cmd, uint32_t len) {
    return i2c_cmd_write_bus(0, addr, cmd, len);
}

bool i2c_cmd_read_bus(uint8_t bus, uint8_t addr, const uint8_t *cmd, uint32_t cmd_len, uint8_t *buf, uint32_t len) {
    if (!buf && len > 0) return false;
    return i2c_xfer_bus(bus, addr, cmd, cmd_len, buf, len);
}

bool i2c_cmd_read(uint8_t addr, const uint8_t *cmd, uint32_t cmd_len, uint8_t *buf, uint32_t len) {
    return i2c_cmd_read_bus(0, addr, cmd, cmd_len, buf, len);
}
