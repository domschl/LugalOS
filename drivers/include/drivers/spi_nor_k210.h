#ifndef DRIVERS_SPI_NOR_K210_H
#define DRIVERS_SPI_NOR_K210_H

#include <stdbool.h>
#include <stdint.h>

/* The K210's system NOR flash (drivers/spi_nor_k210.c).
 *
 * The Maix Bit's GD25LQ sits on dedicated pads behind the SPI3 SSIC at
 * 0x54000000 — not on the FPIOA-muxable SPI0, and the pads carry no
 * other function (plan/phase65 §1.2/§1.5). There is no XIP window in
 * any boot mode this project uses: every byte travels as an SPI
 * transaction, which is what makes this board's /flash0 look like the
 * P4's rather than the RP2350's from above.
 *
 * All calls are serialised by one ylock and safe from task context;
 * never from a trap handler, because WIP polling is a busy wait (a
 * sector erase is tens of milliseconds, a page program sub-millisecond).
 * Addresses are flash offsets. Return 0 on success, -1 on refusal or
 * WIP timeout. */
int  spi_nor_k210_init(void);          /* id the part; idempotent */
bool spi_nor_k210_ready(void);         /* init ran and a part answered */
uint32_t spi_nor_k210_capacity(void);  /* bytes; 0 before init */
const char *spi_nor_k210_part(void);   /* "gd25lq128" etc, "unknown" before */

int spi_nor_k210_read(uint32_t addr, void *buf, uint32_t len);
/* Program into already-erased cells (page program, 256-byte boundaries
 * handled here). Crossing a not-erased region reads back garbage, the
 * way every NOR does. */
int spi_nor_k210_write(uint32_t addr, const void *buf, uint32_t len);
int spi_nor_k210_erase_sector(uint32_t addr);   /* 4 KB, aligned */
int spi_nor_k210_erase_block(uint32_t addr);    /* 64 KB, aligned */

#endif
