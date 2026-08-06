#ifndef DRIVERS_AT24C32_H
#define DRIVERS_AT24C32_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define AT24C32_I2C_ADDR 0x57
#define AT24C32_SIZE_BYTES 4096
#define AT24C32_PAGE_SIZE 32

void at24c32_init(void);
bool at24c32_is_detected(void);
int at24c32_read(uint16_t addr, uint8_t *buf, size_t len);
int at24c32_write(uint16_t addr, const uint8_t *buf, size_t len);

#endif // DRIVERS_AT24C32_H
