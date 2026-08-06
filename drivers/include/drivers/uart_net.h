#ifndef DRIVERS_UART_NET_H
#define DRIVERS_UART_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

#endif // DRIVERS_UART_NET_H
