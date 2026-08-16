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

/* M4.5, plan/phase12_microkernel_migration.md, Part B: direct-hardware
 * access (bounds clamp + page-boundary write chunking, unchanged), used by
 * at24c32_read()/write() above whenever the shared "i2c" task (see
 * drivers/i2c_rtc.h) is not alive, and by that task's body when it is. Not
 * meant for other callers -- go through at24c32_read()/write(). */
int at24c32_hw_read(uint16_t addr, uint8_t *buf, size_t len);
int at24c32_hw_write(uint16_t addr, const uint8_t *buf, size_t len);

#endif // DRIVERS_AT24C32_H
