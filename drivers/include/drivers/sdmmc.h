/*
 * LugalOS Hardware Driver: the ESP32-P4's SD/MMC host controller.
 *
 * The microSD slot on the Waveshare ESP32-P4-NANO, on SDMMC slot 0's IO_MUX
 * pads (GPIO39-44), presented to the rest of the system as the FAT32 volume
 * at /sd0/. 35.1-35.5, plan/phase35_esp32p4_sdmmc.md.
 *
 * The RP2350 analogue is drivers/spisd.h, and the two are deliberately NOT a
 * shared driver: that one speaks SD's SPI mode over a PL022, this one speaks
 * SD's native 4-bit protocol to a Synopsys host controller. Category A of
 * plan/hardware_seams.md -- the register half is never shared.
 */

#ifndef LUGALOS_DRIVERS_SDMMC_H
#define LUGALOS_DRIVERS_SDMMC_H

#include "drivers/block.h"

/* The card, initialised on first call and remembered afterwards (the same
 * lazy pattern spisd_get_device() uses, and for the same reason: a MicroSD
 * card is optional and removable, so a board with an empty slot must not pay
 * for it at boot). NULL when there is no card, when it would not initialise,
 * or when CONFIG_ENABLE_SDMMC is off for this build. */
block_dev_t *sdmmc_get_device(void);

/* Starts the "sdblk" driver task -- the same endpoint name, wire protocol and
 * batching the RP2350's spisd and QEMU's virtio_blk register, so /proc, the
 * `blkstats` command and the tests do not have to know which board they are
 * on. Must run after sched_init(); sdmmc_get_device()'s read_blocks()/
 * write_blocks() work before this (direct hardware access) and keep working
 * exactly the same way if it fails. Returns the pid, or -1. */
int sdmmc_task_start(void);

/* Requests the sdblk task has actually served since boot. Nonzero and growing
 * distinguishes "the task is serving" from "every caller silently fell back
 * to direct access" -- see drivers/spisd.h, which declares the same symbol on
 * the board where spisd_rp2350.c defines it. */
uint32_t blk_task_call_count(void);

/* The `sdinfo` shell command: what is in the slot, what it identified as, and
 * at what clock and bus width this host is talking to it. A table a human
 * asked for, so it goes to the console -- `emac scan`'s shape, and for the
 * same reason: a claim the hardware re-checks on every run rather than a
 * comment. */
void sdmmc_info_report(void);

/* The `sdbench` shell command: sequential read throughput over `kilobytes`
 * (0 or out of range means 1 MB), through block_dev_t so the number is the one
 * a filesystem would see. Read-only. */
void sdmmc_bench_report(uint32_t kilobytes);

#endif /* LUGALOS_DRIVERS_SDMMC_H */
