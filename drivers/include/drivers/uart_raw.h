#ifndef DRIVERS_UART_RAW_H
#define DRIVERS_UART_RAW_H

#include <stdbool.h>
#include <stdint.h>

/* UART0 as a raw byte channel, for a link protocol that owns the wire (LLP
 * toward a radio on the RP2350 terminal's header H7; drivers/uart_rp2350.c).
 * Once opened, received bytes go into `ring` (the caller's, `cap` bytes) from
 * the UART's interrupt, and nothing else uses UART0: console output goes to
 * the USB console only, and UART0's console input reads as empty. Not closed again: the wire
 * stays the link's until a reset. */
int  uart_raw_open(uint8_t *ring, uint32_t cap, uint32_t baud);
/* The task to task_unblock() whenever bytes arrive (-1: none). */
void uart_raw_set_waker(int pid);
bool uart_raw_active(void);
/* Received bytes are waiting to be read. */
bool uart_raw_pending(void);
/* Up to `max` received bytes; 0 when none, -1 when not open. */
int  uart_raw_read(uint8_t *buf, uint32_t max);
/* All of `buf`, yielding while the FIFO is full; -1 when not open. */
int  uart_raw_write(const uint8_t *buf, uint32_t len);
/* Bytes the ring had no room for; bytes the FIFO overran (the interrupt was late). */
void uart_raw_counts(uint32_t *drops, uint32_t *overruns);

#endif
