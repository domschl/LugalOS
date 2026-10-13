#ifndef DRIVERS_SDHOST_ESP32P4_H
#define DRIVERS_SDHOST_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

/* The ESP32-P4's SD/MMC host controller as a shared resource: slot 0 is the microSD card
 * (drivers/sdmmc_esp32p4.c, which owns the controller), slot 1 the ESP32-C6
 * on SDIO (drivers/sdio_esp32p4.c). One command path, one IDMAC, one host
 * clock: every use is bracketed by sdhost_acquire(card) / sdhost_release(),
 * and everything in between acts on that card only. */

void sdhost_acquire(uint32_t card);
void sdhost_release(void);

/* Clocks, reset and the controller's own settings, once; both slots call it.
 * 0, or -1 if the module never came out of reset (no clock). Under the lock. */
int sdhost_controller_init(void);

/* The current card's clock, as close to `khz` as SDMMC_BASE_HZ / (2n) allows
 * (20 MHz undivided); returns what it got, in kHz. Under the lock. */
uint32_t sdhost_set_clock(uint32_t khz);

/* The current card's bus width: 4 bits or 1. Under the lock. */
void sdhost_set_width4(bool four);

/* Response kinds. R5 (SDIO's CMD52/53) is a short, CRC-checked one: R1's
 * shape. R4 (CMD5's OCR) has no CRC: R3's. */
typedef enum {
    SDHOST_RESP_NONE = 0,
    SDHOST_RESP_R1,       /* and R5, R6, R7 */
    SDHOST_RESP_R1B,
    SDHOST_RESP_R2,
    SDHOST_RESP_R3,       /* and R4: no CRC */
} sdhost_resp_t;

/* Command flags beyond the response kind (the controller's CMD bits). */
#define SDHOST_DATA        (1u << 9)    /* a data phase follows */
#define SDHOST_WRITE       (1u << 10)   /* host to card */
#define SDHOST_SEND_INIT   (1u << 15)   /* 80 clocks before this command */
#define SDHOST_STOP_ABORT  (1u << 14)   /* ends a data transfer; does not wait for it */

/* One command; 0, or -1 with the raw status in *status. Under the lock. */
int sdhost_cmd(uint8_t index, uint32_t arg, sdhost_resp_t rt, uint32_t flags,
               uint32_t *resp, uint32_t *status);

/* A DMA descriptor, one per cache line (the CPU and the DMA both write it). */
typedef struct {
    volatile uint32_t des0, des1, des2, des3;
    uint32_t pad[12];
} sdhost_desc_t;

/* Arms the IDMAC on `buf` (cache-line aligned, a whole number of lines; the
 * caller does the cache maintenance) for `len` bytes in `blksz` blocks, then
 * the caller sends the data command and waits. Under the lock. */
void sdhost_dma_arm(sdhost_desc_t *d, uint8_t *buf, uint32_t len, uint32_t blksz);
int  sdhost_dma_wait(uint32_t tag, const char *what);

/* After a data phase that never finished (a card that stopped answering
 * mid-transfer): the FIFO and the IDMAC back to idle and the status cleared,
 * so the next command on either slot starts clean. The card's side of the
 * abort is the caller's. Under the lock. */
void sdhost_data_reset(void);

/* Raw status [31:16]: card n's SDIO interrupt is bit 16+n. Read, and clear. */
uint32_t sdhost_raw_status(void);
void sdhost_clear_status(uint32_t bits);

#endif
