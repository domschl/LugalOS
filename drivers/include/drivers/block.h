#ifndef LUGALOS_DRIVERS_BLOCK_H
#define LUGALOS_DRIVERS_BLOCK_H

#include <stdint.h>

typedef struct block_dev {
    const char *name;
    uint32_t block_size;   // Default 512 bytes
    uint32_t num_blocks;   // e.g., 1024 blocks = 512 KB
    int (*read_blocks)(struct block_dev *dev, void *buf, uint32_t lba, uint32_t count);
    int (*write_blocks)(struct block_dev *dev, const void *buf, uint32_t lba, uint32_t count);
} block_dev_t;

block_dev_t *ramdisk_get_device(void);
block_dev_t *virtio_blk_get_device(void);

#endif /* LUGALOS_DRIVERS_BLOCK_H */
