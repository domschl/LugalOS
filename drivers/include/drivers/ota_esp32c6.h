#ifndef DRIVERS_OTA_ESP32C6_H
#define DRIVERS_OTA_ESP32C6_H

/* How an update image sits in one of the ESP32-C6's slots, and what the loader
 * (arch/riscv/esp32c6/loader.c) tells the kernel it boots. Shared by the
 * loader, the kernel's update backend (drivers/ota_esp32c6.c) and, through
 * flash.manifest, tools/c6flash.py. Freestanding.
 *
 *   slot + 0           the image header (kernel/include/kernel/ota_image.h)
 *   slot + PART0_OFF   part 0: the kernel's RAM half, in the boot ROM's image format
 *   slot + PART1_OFF   part 1: the XIP half, 64 KB-aligned for the flash MMU */

#include <stdint.h>

#define OTA_C6_PART0_OFF      0x1000u
#define OTA_C6_PART1_OFF      0x20000u
#define OTA_C6_PART0_MAX      (OTA_C6_PART1_OFF - OTA_C6_PART0_OFF)

/* At the kernel's entry: a0 = OTA_C6_BOOT_MAGIC, a1 = the XIP half's flash
 * address, a2 = flags, a3 = the slot. Any other a0 (the ROM booting a kernel
 * image directly, or esptool's load-ram) means "no loader": slot A's XIP half,
 * and no update is possible in this boot. */
#define OTA_C6_BOOT_MAGIC     0x44414f4cu       /* "LOAD": LugalOS loader */
#define OTA_C6_IDF_BOOT_MAGIC 0x46444945u       /* "EIDF": ESP-IDF 2nd-stage bootloader */
#define OTA_C6_FLAG_TRIAL     1u                /* this boot is a trial of a new image */
#define OTA_C6_FLAG_FELL_BACK 2u                /* the last trial did not confirm */

/* Where esp32c6_xip_map() looks for otadata and ota_0 on an IDF-booted chip,
 * before the partition table can be read (esp-hosted-mcu's
 * slave/partitions.esp32c6.csv). drivers/ota_esp32c6.c checks the real table
 * against them and refuses updates when they differ: on any other table the
 * boot-time trial handling read the wrong sectors. */
#define OTA_C6_IDF_OTADATA_BASE 0x00d000u
#define OTA_C6_IDF_OTA0_BASE    0x010000u
/* "LTRL", little-endian: the seq_label LugalOS writes on an entry it wants tried. */
#define OTA_C6_IDF_TRIAL_LABEL  0x4c52544cu

/* The trial's deadline: the RTC watchdog the loader arms resets the chip this
 * long after a trial boot unless the kernel confirms (and disarms it) first. */
#define OTA_C6_TRIAL_S        60u

typedef struct {
    uint32_t magic;
    uint32_t xip_base;
    uint32_t flags;
    uint32_t slot;
} c6_boot_info_t;

#include <stdbool.h>
bool ota_c6_idf_writable(uint32_t addr, uint32_t len);

#endif
