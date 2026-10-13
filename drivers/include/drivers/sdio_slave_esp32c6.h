#ifndef DRIVERS_SDIO_SLAVE_ESP32C6_H
#define DRIVERS_SDIO_SLAVE_ESP32C6_H

#include <stdint.h>
#include <stdbool.h>

/* The ESP32-C6's SDIO slave, toward the P4 on the NANO and the LCD-7B: LLP's
 * SDIO binding, slave side (kernel/include/kernel/llp_sdio.h says what the two
 * sides agree on). Polled, one caller: the LLP pump. */

/* Pads, clock, the receive ring; then tells the host the card is ready.
 * 0, or -1 (no memory). Idempotent. */
int sdio_slave_start(void);

/* Once per poll: finishes a send the host has read, and handles a channel
 * reset the host asked for. True when such a reset happened since the last
 * call (everything queued in either direction is gone). */
bool sdio_slave_poll(void);

/* The next whole packet from the host, copied to `buf`. Its length; 0 when
 * none is complete; -1 when it was longer than `cap` (dropped). */
int sdio_slave_recv(uint8_t *buf, uint32_t cap);

/* True when the previous packet has been read and a new one can go. */
bool sdio_slave_send_ready(void);

/* The two send buffers (i = 0, 1) of LLP_SDIO_PKT_MAX bytes: the only memory
 * the slave's DMA may read, so the only memory sdio_slave_send() takes. NULL
 * before sdio_slave_start(). */
uint8_t *sdio_slave_tx_buf(unsigned i);

/* Offers `len` bytes (a multiple of 4, at most LLP_SDIO_PKT_MAX) of one of the
 * send buffers to the host. The buffer stays the driver's until
 * sdio_slave_send_ready() is true again. 0, or -1 when the previous packet is
 * still out (or `buf` is not a send buffer). */
int sdio_slave_send(const uint8_t *buf, uint32_t len);

/* The radio's link state, mirrored in the shared registers. */
void sdio_slave_set_link(uint8_t up, int8_t rssi);

/* Edge timing: 0 = send and sample on the rising edge (the default), 1 = send
 * rising/sample falling, 2 = send falling/sample rising, 3 = both falling. */
void sdio_slave_set_timing(uint32_t t);

/* `sdio` on the C6. */
void sdio_slave_report(void);

#endif
