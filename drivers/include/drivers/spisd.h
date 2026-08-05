/*
 * LugalOS Hardware Driver: SPI MicroSD Card Driver for RP2350 (Pico 2)
 * Connects physical MicroSD cards via SPI1 (GP10-GP13) to FAT32 /sd0/
 */

#ifndef LUGALOS_DRIVERS_SPISD_H
#define LUGALOS_DRIVERS_SPISD_H

#include "drivers/block.h"

block_dev_t *spisd_get_device(void);

#endif /* LUGALOS_DRIVERS_SPISD_H */
