/*
 * LugalOS Driver: Embedded Flash Block Device (/flash0/)
 * Provides block device operations backed by Flash ROM image.
 */

#include "drivers/flashdisk.h"
#include "kernel/printk.h"
#include <string.h>

extern const uint8_t g_flash_fs_start[];
extern const uint32_t g_flash_fs_size;

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
    g_flashdisk_dev.num_blocks = g_flash_fs_size / FLASH_BLOCK_SIZE;
    return &g_flashdisk_dev;
}
