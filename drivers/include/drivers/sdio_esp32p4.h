#ifndef DRIVERS_SDIO_ESP32P4_H
#define DRIVERS_SDIO_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

/* The P4's SDIO host on the SD/MMC controller's slot 1, toward the board's
 * ESP32-C6. See
 * drivers/sdio_esp32p4.c. */

#define SDIO_XFER_MAX 4096u     /* one CMD53, through one DMA descriptor */

typedef struct {
    bool     present;
    uint32_t stage;             /* how far the last probe got; 9 = up */
    uint32_t last_status;       /* raw controller status at a failed step */
    uint32_t ocr, functions, memory, rca;
    uint8_t  cccr_rev, sd_rev, caps;
    uint32_t cis;
    uint16_t vendor, device;    /* the CIS's MANFID tuple */
    bool     fn1_ready;
    uint32_t block_size, width, khz, errors;
    bool     high_speed;        /* CCCR EHS set: the card times for above 25 MHz */
} sdio_info_t;

/* Brings the card up and leaves it selected, function 1 enabled, at `khz`
 * (0 = the board's CONFIG_SDIO_FREQ_KHZ). 0, or -1 (sdio_info() says where). */
int sdio_probe(uint32_t khz);
const sdio_info_t *sdio_info(void);

int sdio_cmd52(bool write, uint32_t fn, uint32_t addr, uint8_t in, uint8_t *out);
int sdio_cmd53(bool write, uint32_t fn, uint32_t addr, uint8_t *data, uint32_t len);

/* Pulls the C6's CHIP_PU low for `hold_ms`, then lets it go (open-drain). */
void sdio_c6_reset(uint32_t hold_ms);

void sdio_report(void);

/* LLP's SDIO binding, host side (kernel/include/kernel/llp_sdio.h). One
 * caller, the LLP pump. */
int  sdio_llp_open(void);              /* probe, reset the channel; 0 when an LLP slave answered */
int  sdio_llp_poll(void);              /* one status read; 0 up, 1 up and restarted, -1 down */
bool sdio_llp_up(void);
/* The next poll asks the slave to reset the channel (LLP_SDIO_SLV_RESET). */
void sdio_llp_resync(void);
uint32_t sdio_llp_readable(void);      /* bytes the slave offers, as last polled */
bool sdio_llp_room(uint32_t len);      /* a packet of `len` fits the slave's free buffers */
int  sdio_llp_write(const uint8_t *pkt, uint32_t len);   /* 0; -2 no room; -1 error */
int  sdio_llp_read(uint8_t *buf, uint32_t cap);         /* bytes read; 0 none; -1 error */
void sdio_llp_report(void);
/* The task woken by the card interrupt (the slave's state changed), and
 * whether one came since the last poll. */
void sdio_llp_set_waker(int pid);
bool sdio_llp_irq_pending(void);
/* Repeated CMD53 reads of `len` bytes at `addr` of `fn` (0 = one block). */
void sdio_bench(uint32_t kilobytes, uint32_t fn, uint32_t addr, uint32_t len);

#endif
