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

/* The ramdisk's real, compile-time-fixed backing storage size in blocks.
 * ramdisk_read()/write() already bound-check every access against this
 * directly (not against block_dev_t.num_blocks, which callers like
 * vfs_mount_ramdisk() can set to whatever a user requests), so a request
 * larger than this can't corrupt memory -- but it can still describe a
 * FAT32 volume that's larger than its real storage, which just makes
 * writes past the real end silently fail. Exposed so callers can clamp the
 * request up front and give the user a clear error instead. */
uint32_t ramdisk_max_blocks(void);

#endif /* LUGALOS_DRIVERS_BLOCK_H */
