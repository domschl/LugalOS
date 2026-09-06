/*
 * LugalOS Driver: Embedded Flash Block Device (/flash0/)
 * Provides block device operations backed by Flash ROM image.
 */

#include "drivers/flashdisk.h"
#include "kernel/printk.h"
#include "lugalos_config.h"
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
/* E2, plan/phase27_esp32p4_bringup.md: this board has a filesystem image
 * built (build/esp32p4/flashfs.bin) and nowhere yet to put it.
 *
 * It cannot take the embedded-array form the QEMU targets use: that array is
 * 512 KB and this kernel is loaded into 512 KB of L2MEM in total
 * (linker/esp32p4.ld). It cannot take the RP2350 form either, which needs a
 * flash map, and E6 is the milestone that establishes one.
 *
 * So the size is zero and flashdisk_get_device() below declines. /flash0
 * stays unmounted and `df` says so, which is the accurate report; the
 * alternative -- pointing this at flash that has not been written -- would
 * mount whatever the factory image left there and call it ours. */
static const uint8_t *const g_flash_fs_start = NULL;
static const uint32_t g_flash_fs_size = 0;
#else
extern const uint8_t g_flash_fs_start[];
extern const uint32_t g_flash_fs_size;
#endif

#define FLASH_BLOCK_SIZE 512

static int flashdisk_read_blocks(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    uint32_t max_blocks = g_flash_fs_size / FLASH_BLOCK_SIZE;
    if (lba + count > max_blocks) return -1;
    memcpy(buf, g_flash_fs_start + (lba * FLASH_BLOCK_SIZE), count * FLASH_BLOCK_SIZE);
    return 0;
}

static int flashdisk_write_blocks(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev; (void)buf; (void)lba; (void)count;
    printk("[FlashDisk Error] /flash0/ Embedded Flash ROMDisk is read-only\n");
    return -1;
}

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
