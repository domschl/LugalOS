#ifndef LUGALOS_DRIVERS_VIRTIO_BLK_H
#define LUGALOS_DRIVERS_VIRTIO_BLK_H

#include "drivers/block.h"

int virtio_blk_init(void);
block_dev_t *virtio_blk_get_device(void);

#endif /* LUGALOS_DRIVERS_VIRTIO_BLK_H */
