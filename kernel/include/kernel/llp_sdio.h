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
 *   bytes ("requested length"), split as IDF's host splits it: whole blocks
 *   while a block or more remains (each addressed FIFO_END - remaining), then
 *   the rest in byte mode, rounded up to 4. A packet rounded up to whole
 *   blocks in one CMD53 is not this slave's: it stops answering commands.
 * * Host -> slave: the slave loads receive buffers of LLP_SDIO_BUF bytes
 *   and counts them in TOKEN (cumulative, 12 bits). A packet of n bytes uses
 *   ceil(n / LLP_SDIO_BUF) of them; the host never writes more than the count
 *   allows. That is the flow control: LLP's FLOW is not set.
 * * Slave -> host: PKT_LEN counts the bytes the slave has offered
 *   (cumulative, 20 bits); the host reads the difference to what it has read.
 *   The slave offers one packet at a time.
 *
 * Slave -> host interrupt: the slave raises LLP_SDIO_INT_CHANGED (its
 *   to-host bit 0, the SDIO card interrupt on DAT1) *after* each change the
 *   host polls for -- a packet offered, receive buffers loaded, a channel
 *   reset. The host enables it in INT_ENA, clears it in INT_CLR *before* it
 *   reads the status, so a change during the read raises it again. A host
 *   that never enables it polls, as before.
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
#define LLP_SDIO_REG_INT_CLR  0x0D4u    /* write 1 to clear INT_ST's bits */
#define LLP_SDIO_REG_INT_ENA  0x0DCu    /* which of them raise the card interrupt */

#define LLP_SDIO_INT_CHANGED  0x01u     /* the slave's state changed: poll */

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
