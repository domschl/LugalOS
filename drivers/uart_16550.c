#include "drivers/uart.h"

static volatile uint8_t *uart_base = (volatile uint8_t *)0x10000000; // QEMU virt UART0 base

/* 16550 Register Offsets */
#define UART_RBR 0 // Receive Buffer Register
#define UART_THR 0 // Transmit Holding Register
#define UART_LSR 5 // Line Status Register
#define UART_LSR_DR   0x01 // Data Ready
#define UART_LSR_THRE 0x20 // Transmit Holding Register Empty

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint8_t *)base_addr;
    }
}

void uart_putc(char c) {
    if (!uart_base) return;
    while ((uart_base[UART_LSR] & UART_LSR_THRE) == 0);
    uart_base[UART_THR] = (uint8_t)c;
}

bool uart_has_char(void) {
    if (!uart_base) return false;
    return (uart_base[UART_LSR] & UART_LSR_DR) != 0;
}

#include "drivers/usb_cdc.h"

char uart_getc(void) {
    while (!uart_has_char()) {
        usb_cdc_task();
    }
    return (char)uart_base[UART_RBR];
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
