#include "drivers/block.h"
#include "kernel/printk.h"
#include <string.h>

#define RAMDISK_BLOCK_SIZE 512
#if defined(CONFIG_BOARD_RP2350)
#define RAMDISK_NUM_BLOCKS 128 // 64 KB Total RAMDisk size for RP2350 SRAM
#else
#define RAMDISK_NUM_BLOCKS 1024 // 512 KB Total RAMDisk size
#endif


static uint8_t ramdisk_storage[RAMDISK_NUM_BLOCKS * RAMDISK_BLOCK_SIZE];

static int ramdisk_read(block_dev_t *dev, void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!buf || lba + count > RAMDISK_NUM_BLOCKS) return -1;
    memcpy(buf, &ramdisk_storage[lba * RAMDISK_BLOCK_SIZE], count * RAMDISK_BLOCK_SIZE);
    return 0;
}

static int ramdisk_write(block_dev_t *dev, const void *buf, uint32_t lba, uint32_t count) {
    (void)dev;
    if (!buf || lba + count > RAMDISK_NUM_BLOCKS) return -1;
    memcpy(&ramdisk_storage[lba * RAMDISK_BLOCK_SIZE], buf, count * RAMDISK_BLOCK_SIZE);
    return 0;
}

static block_dev_t ramdisk_device = {
    .name = "ramdisk0",
    .block_size = RAMDISK_BLOCK_SIZE,
    .num_blocks = RAMDISK_NUM_BLOCKS,
    .read_blocks = ramdisk_read,
    .write_blocks = ramdisk_write
};

block_dev_t *ramdisk_get_device(void) {
    return &ramdisk_device;
}
