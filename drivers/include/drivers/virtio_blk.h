#ifndef LUGALOS_DRIVERS_VIRTIO_BLK_H
#define LUGALOS_DRIVERS_VIRTIO_BLK_H

#include "drivers/block.h"

int virtio_blk_init(void);
block_dev_t *virtio_blk_get_device(void);

/* starts the "blk"
 * driver task. Must run after sched_init(); virtio_blk_get_device()'s
 * read_blocks()/write_blocks() work before this runs too (direct MMIO
 * access, same as before this milestone) and keep working exactly the
 * same way if it fails. Returns the task's pid, or -1 if it could not be
 * started or no virtio-blk device was ever found. */
int virtio_blk_task_start(void);

// Verify: how many batched-block chan_call()s the blk task has
// served since boot -- see drivers/virtio_blk.c's g_blk_calls comment.
uint32_t blk_task_call_count(void);

#endif /* LUGALOS_DRIVERS_VIRTIO_BLK_H */
