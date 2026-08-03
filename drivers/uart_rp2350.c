/*
 * LugalOS Hardware Driver: PL011 UART Engine for RP2350 (Raspberry Pi Pico 2)
 * Controls UART0 on GPIO0 (TXD, Pico Pin 1) and GPIO1 (RXD, Pico Pin 2) at 115,200 baud.
 */

#include "drivers/uart.h"

#define RESETS_BASE        0x40020000
#define RESETS_RESET       (*(volatile uint32_t *)(RESETS_BASE + 0x00))
#define RESETS_RESET_DONE  (*(volatile uint32_t *)(RESETS_BASE + 0x08))

#define IO_BANK0_BASE      0x40028000
#define GPIO0_CTRL         (*(volatile uint32_t *)(IO_BANK0_BASE + 0x004))
#define GPIO1_CTRL         (*(volatile uint32_t *)(IO_BANK0_BASE + 0x00c))

#define UART0_BASE_DEFAULT 0x40034000
static volatile uint32_t *uart_base = (volatile uint32_t *)UART0_BASE_DEFAULT;

#define UARTDR    (0x00 / 4)
#define UARTFR    (0x18 / 4)
#define UARTIBRD  (0x24 / 4)
#define UARTFBRD  (0x28 / 4)
#define UARTLCR_H (0x2c / 4)
#define UARTCR    (0x30 / 4)

#define UARTFR_TXFF (1 << 5)
#define UARTFR_RXFE (1 << 4)

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint32_t *)base_addr;
    }

    /* Release reset for IO_BANK0, PADS_BANK0, and UART0 peripherals */
    uint32_t mask = (1 << 22) | (1 << 11) | (1 << 8); // UART0 | IO_BANK0 | PADS_BANK0
    RESETS_RESET &= ~mask;
    while ((RESETS_RESET_DONE & mask) != mask);

    /* Configure GPIO0 as UART0 TX (func 2) and GPIO1 as UART0 RX (func 2) */
    GPIO0_CTRL = 2;
    GPIO1_CTRL = 2;

    /* Disable UART0 before configuration */
    uart_base[UARTCR] = 0;

    /* Set 115,200 Baud Rate (assuming 12 MHz reference clock from bootrom default) */
    /* 12,000,000 / (16 * 115200) = 6.5104 => IBRD = 6, FBRD = 33 */
    uart_base[UARTIBRD] = 6;
    uart_base[UARTFBRD] = 33;

    /* 8 bits, FIFO enable (WLEN 8 = 0x60, FEN = 0x10) */
    uart_base[UARTLCR_H] = 0x70;

    /* Enable UART, TX, and RX (UARTEN = bit 0, TXE = bit 8, RXE = bit 9) */
    uart_base[UARTCR] = (1 << 0) | (1 << 8) | (1 << 9);
}

void uart_putc(char c) {
    if (!uart_base) return;
    while (uart_base[UARTFR] & UARTFR_TXFF);
    uart_base[UARTDR] = (uint8_t)c;
}

bool uart_has_char(void) {
    if (!uart_base) return false;
    return (uart_base[UARTFR] & UARTFR_RXFE) == 0;
}

char uart_getc(void) {
    while (!uart_has_char());
    return (char)(uart_base[UARTDR] & 0xFF);
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}
