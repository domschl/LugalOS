#ifndef DRIVERS_OTA_ESP32P4_H
#define DRIVERS_OTA_ESP32P4_H

/* How an update image sits in one of the ESP32-P4's slots, and what the loader
 * (arch/riscv/esp32p4/loader.c) tells the kernel it boots. Shared by the
 * loader, the kernel's update backend (drivers/ota_esp32p4.c), the early
 * mapping (arch/riscv/common/xip_esp32p4.c) and, through flash.manifest,
 * tools/p4flash.py. Freestanding.
 *
 *   slot + 0           the image header (kernel/include/kernel/ota_image.h)
 *   slot + PART0_OFF   part 0: the kernel's RAM half, in the boot ROM's image format
 *   slot + PART1_OFF   part 1: the XIP half, 64 KB-aligned for the flash MMU */

#include <stdint.h>

#define OTA_P4_PART0_OFF      0x1000u
#define OTA_P4_PART1_OFF      0x20000u
#define OTA_P4_PART0_MAX      (OTA_P4_PART1_OFF - OTA_P4_PART0_OFF)

/* At the kernel's entry: a0 = OTA_P4_BOOT_MAGIC, a1 = the XIP half's flash
 * address, a2 = flags, a3 = the slot. Any other a0 (esptool's load-ram) means
 * "no loader": slot A's XIP half, and no update is possible in this boot. */
#define OTA_P4_BOOT_MAGIC     0x44414f4cu       /* "LOAD": LugalOS loader */
#define OTA_P4_FLAG_TRIAL     1u                /* this boot is a trial of a new image */
#define OTA_P4_FLAG_FELL_BACK 2u                /* the last trial did not confirm */

/* The trial's deadline: the RTC watchdog the loader arms resets the chip this
 * long after a trial boot unless the kernel confirms (and disarms it) first. */
#define OTA_P4_TRIAL_S        60u

typedef struct {
    uint32_t magic;
    uint32_t xip_base;
    uint32_t flags;
    uint32_t slot;
} p4_boot_info_t;

#include <stdbool.h>
/* Whether [addr, addr+len) is inside the slot this boot does not run from. */
bool ota_p4_slot_writable(uint32_t addr, uint32_t len);

#endif
