#ifndef DRIVERS_FLASH_ESP32C6_H
#define DRIVERS_FLASH_ESP32C6_H

#include <stdbool.h>
#include <stdint.h>

/* The ESP32-C6's flash, written while the kernel executes from it
 * (drivers/flash_esp32c6.c). Every call is serialised, and each ROM transfer
 * runs with interrupts masked and the instruction cache suspended, from SRAM.
 *
 * Writes and erases are refused outside the regions the kernel owns: the
 * identity sector, the two boot-record sectors, and the update slot this boot
 * is not running from (and only when the loader booted it -- a kernel loaded
 * by esptool does not know which slot is safe). The loader at offset 0 is
 * never written from here. Addresses are flash offsets. 0 or -1. */
int c6flash_read(uint32_t addr, void *buf, uint32_t len);
int c6flash_erase_sector(uint32_t addr);       /* 4 KB, aligned */
int c6flash_write(uint32_t addr, const void *buf, uint32_t len);
bool c6flash_writable(uint32_t addr, uint32_t len);

#endif
