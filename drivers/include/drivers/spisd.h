/*
 * LugalOS Hardware Driver: SPI MicroSD Card Driver for RP2350 (Pico 2)
 * Connects physical MicroSD cards via SPI1 (GP10-GP13) to FAT32 /sd0/
 */

#ifndef LUGALOS_DRIVERS_SPISD_H
#define LUGALOS_DRIVERS_SPISD_H

#include "drivers/block.h"

block_dev_t *spisd_get_device(void);

/* M4.5, plan/phase12_microkernel_migration.md, Part B: starts the "sdblk"
 * driver task. Must run after sched_init(); spisd_get_device()'s
 * read_blocks()/write_blocks() work before this runs too (direct hardware
 * access, same as before this milestone) and keep working exactly the same
 * way if it fails. Returns the task's pid, or -1 if it could not be
 * started, or if CONFIG_ENABLE_SPISD is off for this board persona. */
int spisd_task_start(void);

// M4.5 verify: how many batched-block chan_call()s the sdblk task has
// served since boot -- see drivers/spisd_rp2350.c's g_blk_calls comment.
uint32_t blk_task_call_count(void);

#endif /* LUGALOS_DRIVERS_SPISD_H */
