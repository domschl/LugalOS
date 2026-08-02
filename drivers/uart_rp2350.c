#include "drivers/uart.h"

/* RP2350 UART0 Base Address */
#define RP2350_UART0_BASE 0x40034000

static volatile uint32_t *rp2350_uart = (volatile uint32_t *)RP2350_UART0_BASE;

#define UARTDR    0x00 // Data Register
#define UARTFR    0x06 // Flag Register (0x18 in byte offset)

void uart_rp2350_init(void) {
    /* RP2350 UART init placeholder */
}

/* Fallback stub when building for RP2350 board target */
void uart_rp2350_putc(char c) {
    (void)c;
    /* TX logic using RP2350 UART registers */
}
