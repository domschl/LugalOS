#ifndef DRIVERS_UART_NET_H
#define DRIVERS_UART_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "fs/p9_link.h"

#define SLIP_END     0xC0
#define SLIP_ESC     0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

void uart_net_init(void);

// SLIP Framing Helper Functions
int slip_encode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max);
int slip_decode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max);

// Transmits a SLIP-framed 9P request over UART / USB serial channel and receives 9P response
int uart_net_send_9p(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max);

// High-level 9P RPC over UART serial transport
int uart_net_rpc(const char *write_payload, char *read_out_buf, uint32_t read_max);

// A3's link_uart_slip backend: a p9_link_t that speaks SLIP-framed 9P
// directly over the physical UART (drivers/uart.h's uart_getc()/uart_putc()).
// This is the same wire the interactive console uses, so servicing it is
// mutually exclusive with the shell (see kernel/shell.c's `p9serve` command,
// A3a "headless" mode) -- there is no RX demultiplexing (that's A3b,
// deliberately deferred; see the A3 completion notes in
// plan/phase5_distributed_design.md).
p9_link_t *uart_slip_get_link(void);

#endif // DRIVERS_UART_NET_H
