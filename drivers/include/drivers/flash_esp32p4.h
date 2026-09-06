#ifndef LUGALOS_DRIVERS_FLASH_ESP32P4_H
#define LUGALOS_DRIVERS_FLASH_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

/* SPI flash on the ESP32-P4, E6 (plan/phase27_esp32p4_bringup.md).
 *
 * Reads and writes go through the boot ROM's own flash routines rather than a
 * driver written against the MSPI controller. That is the same choice RP2350
 * makes with its bootrom, for the same reason: the ROM code is what the chip
 * vendor uses to program its own parts, it already knows this chip's timing
 * and command set, and re-deriving it would be a large amount of new code
 * whose failure mode is a corrupted flash rather than a wrong number.
 *
 * Unlike RP2350 there is no XIP window here: this kernel is loaded into L2MEM
 * and flash is not mapped into the address space at all, so every read is a
 * transfer rather than a dereference. Callers get a function, not a pointer. */

/* Attaches and identifies the chip. Safe to call more than once; reports what
 * it found. Returns false if the chip does not answer or does not match the
 * size the flash map was built against. */
bool flash_p4_init(void);

bool flash_p4_ready(void);

/* Total chip size in bytes, or 0 before a successful init. */
uint32_t flash_p4_size(void);

/* Reads anywhere on the chip. Reading is not destructive and the factory
 * image is worth being able to inspect, so reads are not restricted to the
 * writable region. Returns 0 on success. */
int flash_p4_read(uint32_t addr, void *buf, uint32_t len);

/* Erases the 4 KB sector containing `addr`, and programs `len` bytes.
 *
 * Both refuse any address below LUGALOS_P4_FLASH_WRITABLE_FLOOR. That guard
 * is the reason this board's factory image survives E6: the region this
 * project owns starts ~0.9 MB above the highest byte any factory partition
 * claims, and a write outside it is a bug rather than a policy choice, so it
 * is refused in the driver rather than avoided by callers. See
 * cmake/flash_layout_esp32p4.cmake for where the numbers come from.
 *
 * Return 0 on success, -1 on refusal or hardware error. */
int flash_p4_erase_sector(uint32_t addr);
int flash_p4_write(uint32_t addr, const void *buf, uint32_t len);

#define FLASH_P4_SECTOR_SIZE 4096u

#endif /* LUGALOS_DRIVERS_FLASH_ESP32P4_H */
