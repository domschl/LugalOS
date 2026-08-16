#ifndef DRIVERS_AT24C32_H
#define DRIVERS_AT24C32_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define AT24C32_I2C_ADDR 0x57
#define AT24C32_SIZE_BYTES 4096
#define AT24C32_PAGE_SIZE 32

/* M5 Phase 3, plan/phase12_microkernel_migration.md: the largest single
 * EEPROM read/write the "i2c" task's chan_call() wire protocol carries in
 * one message -- far smaller than the whole AT24C32_SIZE_BYTES address
 * space, so a U-mode task's buffers stay syscall-sized. drivers/at24c32.c's
 * at24c32_read()/at24c32_write() loop internally in chunks this size for
 * anything larger, transparently to every caller. Shared here (not
 * separately defined in drivers/i2c_rtc.c, which sizes its own endpoint
 * buffers from it) so the client and server side of the wire protocol
 * cannot silently drift out of agreement on the cap. */
#define AT24C32_CHUNK_MAX 128u

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
