/*
 * LugalOS Hardware Driver: SPI MicroSD Card Driver for RP2350 (Pico 2)
 * Connects physical MicroSD cards via SPI1 to FAT32 /sd0/ (pins from the
 * board file: GP10-13 on the Pico 2 personas, CS on GP15 on the RP2350-LCD-7)
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

/* 36.2, plan/phase36_rp2350_lcd7_terminal.md: sequential read throughput
 * through block_dev_t (so including the sdblk task's IPC), with the sample
 * size printed beside the rate. Read-only. The second `sdbench`: the first is
 * sdmmc_bench_report() on the ESP32-P4, and the output format matches so the
 * two boards' figures sit side by side. Two implementations, so noted and not
 * extracted (drivers/README.md). */
void spisd_bench_report(uint32_t kilobytes);

// M4.5 verify: how many batched-block chan_call()s the sdblk task has
// served since boot -- see drivers/spisd_rp2350.c's g_blk_calls comment.
uint32_t blk_task_call_count(void);

#endif /* LUGALOS_DRIVERS_SPISD_H */
