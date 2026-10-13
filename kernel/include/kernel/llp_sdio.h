#ifndef KERNEL_LLP_SDIO_H
#define KERNEL_LLP_SDIO_H

/* LLP's SDIO binding (doc/dev/protocols/llp.md §7.2): what the P4's host
 * driver (drivers/sdio_esp32p4.c) and the C6's slave driver
 * (drivers/sdio_slave_esp32c6.c) must agree on. Both sides include this file
 * and nothing else of each other.
 *
 * The wire is the C6 SDIO slave's packet mode, as Espressif's own hosts use
 * it ("ESP SDIO slave protocol", esp_serial_slave_link):
 *
 * * Function 1 addresses below 0x200 are the slave's SLCHOST registers.
 * * The FIFO: a CMD53 to address LLP_SDIO_FIFO_END - n moves a packet of n
 *   bytes ("requested length"). A transfer longer than n (a whole block, a
 *   byte count rounded up) is zero-filled on reads and cut off on writes.
 * * Host -> slave: the slave loads receive buffers of LLP_SDIO_BUF bytes
 *   and counts them in TOKEN (cumulative, 12 bits). A packet of n bytes uses
 *   ceil(n / LLP_SDIO_BUF) of them; the host never writes more than the count
 *   allows. That is the flow control: LLP's FLOW is not set.
 * * Slave -> host: PKT_LEN counts the bytes the slave has offered
 *   (cumulative, 20 bits); the host reads the difference to what it has read.
 *   The slave offers one packet at a time.
 *
 * A packet carries one or more LLP frames back to back, each (header +
 * payload) padded with zeros to a multiple of 4 bytes, at most LLP_SDIO_PKT_MAX
 * bytes in all. No LLP CRC: the bus has its own. */

#include <stdint.h>

#define LLP_SDIO_FN           1u
#define LLP_SDIO_FIFO_END     0x1F800u
#define LLP_SDIO_BUF          512u      /* one slave receive buffer */
#define LLP_SDIO_PKT_MAX      4608u     /* 9 buffers: the largest frame (4 + 4096) fits */
#define LLP_SDIO_ALIGN(n)     (((n) + 3u) & ~3u)

/* Slave registers, function 1 addresses. */
#define LLP_SDIO_REG_TOKEN    0x044u    /* [27:16] receive buffers loaded, mod 4096 */
#define LLP_SDIO_REG_INT_ST   0x058u
#define LLP_SDIO_REG_PKT_LEN  0x060u    /* [19:0] bytes offered, mod 2^20 */
#define LLP_SDIO_REG_SHARED   0x06Cu    /* the shared bytes below: CONF_W0, CONF_W1 */
#define LLP_SDIO_REG_SLV_INT  0x08Du    /* host -> slave interrupt bits */
#define LLP_SDIO_REG_INT_CLR  0x0D4u

/* What the host reads in one CMD53 per poll: TOKEN through the shared bytes. */
#define LLP_SDIO_STATUS_ADDR  LLP_SDIO_REG_TOKEN
#define LLP_SDIO_STATUS_LEN   (LLP_SDIO_REG_SHARED + 8u - LLP_SDIO_REG_TOKEN)

#define LLP_SDIO_TOKEN(v)     (((v) >> 16) & 0xFFFu)
#define LLP_SDIO_TOKEN_MOD    4096u
#define LLP_SDIO_LEN_MASK     0xFFFFFu

/* The shared bytes, written by the slave. */
#define LLP_SDIO_SH_MAGIC0    0         /* 'L' */
#define LLP_SDIO_SH_MAGIC1    1         /* 'L' */
#define LLP_SDIO_SH_MAGIC2    2         /* 'P': an LLP slave is running */
#define LLP_SDIO_SH_GEN       3         /* channel generation: new after every reset, never 0 */
#define LLP_SDIO_SH_LINK      4         /* the radio's Wi-Fi link, as LLP's STATE */
#define LLP_SDIO_SH_RSSI      5         /* ... and its RSSI, dBm (i8) */
#define LLP_SDIO_SH_NBUF      6         /* receive buffers in the slave's ring */
#define LLP_SDIO_SH_BUF64     7         /* LLP_SDIO_BUF / 64 */

/* Host -> slave interrupt bits. */
#define LLP_SDIO_SLV_RESET    0x01u     /* the host (re)starts the channel: drop
                                         * everything, reset both counters, new gen */

#endif
