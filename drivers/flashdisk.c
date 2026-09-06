/*
 * LugalOS Driver: Embedded Flash Block Device (/flash0/)
 * Provides block device operations backed by Flash ROM image.
 */

#include "drivers/flashdisk.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
#if defined(CONFIG_BOARD_ESP32P4)
#include "drivers/flash_esp32p4.h"
#endif
#include <string.h>

/* Where the FAT32 image lives, and it is not the same answer on every target.
 *
 * I7a, plan/phase21_identity_and_authentication.md §3.3: on RP2350 the image
 * is flashed to its own region (LUGALOS_FLASHFS_BASE, from
 * cmake/flash_layout.cmake, reaching this file as a compile definition) and is
 * read straight out of the XIP window. It is deliberately *not* linked into
 * the binary any more -- it was 512 KB of a ~982 KB image, could not be
 * written, and had to be rewritten on every OS flash despite changing almost
 * never.
 *
 * Everywhere else it stays the embedded C array `tools/create_flash_fs.py`
 * generates. The QEMU targets have no flash map to place a segment in, and for
 * a ROMdisk inside a hosted image, embedding is exactly right rather than a
 * compromise. Both forms are read-only and reach the rest of the system
 * through the identical block_dev_t, so nothing above this file knows which
 * target it is on.
 */
#if defined(LUGALOS_FLASHFS_BASE)
static const uint8_t *const g_flash_fs_start = (const uint8_t *)(uintptr_t)LUGALOS_FLASHFS_BASE;
static const uint32_t g_flash_fs_size = (uint32_t)LUGALOS_FLASHFS_SIZE;
#elif defined(CONFIG_BOARD_ESP32P4)
/* E6, plan/phase27_esp32p4_bringup.md. E2 left this arm with a size of zero
 * and no flash map; E6 supplies both, and one thing besides.
 *
 * **There is no pointer here, because there is no XIP window.** RP2350 reads
 * its filesystem by dereferencing into the flash-mapped address space. On
 * this board the kernel is loaded into L2MEM by `esptool load-ram` and flash
 * is not mapped into the address space at all, so every access is a transfer
 * through drivers/flash_esp32p4.c. The block functions below have a matching
 * arm for exactly that, and the "is it erased?" check at registration becomes
 * a read rather than four dereferences.
 *
 * **And this one is writable**, which no other target's /flash0 is. That is
 * not a P4 luxury: E6's done-condition is a file written from the shell
 * surviving a power cycle, and until now flashdisk_write_blocks() printed
 * "read-only" on every target including RP2350. */
static const uint32_t g_flash_fs_size = (uint32_t)LUGALOS_P4_FLASHFS_SIZE;
#else
extern const uint8_t g_flash_fs_start[];
extern const uint32_t g_flash_fs_size;
#endif

#define FLASH_BLOCK_SIZE 512

static int flashdisk_read_blocks(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    uint32_t max_blocks = g_flash_fs_size / FLASH_BLOCK_SIZE;
    if (lba + count > max_blocks) return -1;
#if defined(CONFIG_BOARD_ESP32P4)
    return flash_p4_read((uint32_t)LUGALOS_P4_FLASHFS_BASE + lba * FLASH_BLOCK_SIZE,
                         buf, count * FLASH_BLOCK_SIZE);
#else
    memcpy(buf, g_flash_fs_start + (lba * FLASH_BLOCK_SIZE), count * FLASH_BLOCK_SIZE);
    return 0;
#endif
}

#if defined(CONFIG_BOARD_ESP32P4)
/* One flash sector, staged in RAM.
 *
 * NOR flash erases in 4 KB sectors and programs within them, while the
 * filesystem writes 512-byte blocks -- so changing one block means reading
 * its whole sector, replacing the 512 bytes, erasing, and writing it all
 * back. There is no way around the read-modify-erase-write; the only choice
 * is where the staging buffer lives, and a 4 KB local would overflow the
 * 4 KB task stacks this board's drivers run on.
 *
 * Static rather than heap: this is the block layer, called from the write
 * path of a filesystem that may itself be servicing a low-memory condition,
 * and an allocation that can fail here turns a slow write into a lost one. It
 * costs 4 KB of .bss against a 276 KB heap. */
static uint8_t g_p4_sector[FLASH_P4_SECTOR_SIZE];

static int flashdisk_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    uint32_t max_blocks = g_flash_fs_size / FLASH_BLOCK_SIZE;
    if (lba + count > max_blocks) return -1;

    const uint8_t *in = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off    = (lba + i) * FLASH_BLOCK_SIZE;
        uint32_t sector = off & ~(FLASH_P4_SECTOR_SIZE - 1u);
        uint32_t within = off - sector;
        uint32_t addr   = (uint32_t)LUGALOS_P4_FLASHFS_BASE + sector;

        if (flash_p4_read(addr, g_p4_sector, FLASH_P4_SECTOR_SIZE) != 0) return -1;

        /* Nothing to do is worth detecting: flash wears out, and a filesystem
         * rewriting an unchanged block is common enough (FAT updates, a
         * directory entry touched twice) that skipping the erase is a real
         * saving rather than a micro-optimisation. */
        if (memcmp(g_p4_sector + within, in, FLASH_BLOCK_SIZE) == 0) {
            in += FLASH_BLOCK_SIZE;
            continue;
        }

        memcpy(g_p4_sector + within, in, FLASH_BLOCK_SIZE);
        if (flash_p4_erase_sector(addr) != 0) return -1;
        if (flash_p4_write(addr, g_p4_sector, FLASH_P4_SECTOR_SIZE) != 0) return -1;
        in += FLASH_BLOCK_SIZE;
    }
    return 0;
}
#else
static int flashdisk_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev; (void)buf; (void)lba; (void)count;
    printk("[FlashDisk Error] /flash0/ Embedded Flash ROMDisk is read-only\n");
    return -1;
}
#endif

static block_dev_t g_flashdisk_dev = {
    .name = "flashdisk0",
    .block_size = FLASH_BLOCK_SIZE,
    .num_blocks = 1024,
    .read_blocks = flashdisk_read_blocks,
    .write_blocks = flashdisk_write_blocks,
};

block_dev_t *flashdisk_get_device(void) {
    /* An image of zero length is not a device. Returning one anyway would
     * hand fat32_init() a block device with num_blocks == 0, and the mount
     * failure it reports names the filesystem rather than the absence. */
    if (g_flash_fs_size == 0) return NULL;

#if defined(CONFIG_BOARD_ESP32P4)
    /* The chip has to answer before it can be a block device. Declining here
     * is the same judgement the zero-size case above makes: a device that
     * cannot be read is not one, and reporting /flash0 unmounted is more
     * accurate than mounting something unreadable. */
    if (!flash_p4_init()) return NULL;

    {
        uint8_t head[4];
        if (flash_p4_read((uint32_t)LUGALOS_P4_FLASHFS_BASE, head, sizeof(head)) != 0) {
            printk("[FlashDisk] /flash0 segment at 0x%06x could not be read\n",
                   (unsigned)LUGALOS_P4_FLASHFS_BASE);
            return NULL;
        }
        if (head[0] == 0xFF && head[1] == 0xFF && head[2] == 0xFF && head[3] == 0xFF) {
            printk("[FlashDisk] /flash0 segment at 0x%06x is erased -- flash "
                   "flashfs.bin there too.\n"
                   "            tools/p4flash.py writes it; the kernel is loaded "
                   "separately into RAM.\n",
                   (unsigned)LUGALOS_P4_FLASHFS_BASE);
        }
    }
#endif

    g_flashdisk_dev.num_blocks = g_flash_fs_size / FLASH_BLOCK_SIZE;

#if defined(LUGALOS_FLASHFS_BASE)
    /* The failure mode I7a's own split introduces, named rather than left to
     * be decoded. The filesystem is a separate UF2 now, so a board flashed
     * with lugalos.uf2 alone -- a fresh board, or anyone following older
     * instructions -- has erased flash here, and FAT32 mounting 0xFF reports
     * something unhelpful about a bad boot sector. Erased NOR reads as all
     * ones and no real FAT32 image starts that way, so this is unambiguous.
     * Checked once at device registration, not per read. */
    if (g_flash_fs_start[0] == 0xFF && g_flash_fs_start[1] == 0xFF &&
        g_flash_fs_start[2] == 0xFF && g_flash_fs_start[3] == 0xFF) {
        printk("[FlashDisk] /flash0 segment at %p is erased -- flash flashfs.uf2 too.\n"
               "            The filesystem is its own image since I7a; lugalos.uf2\n"
               "            no longer carries it. Both live in the same build dir.\n",
               (const void *)g_flash_fs_start);
    }
#endif

    return &g_flashdisk_dev;
}
