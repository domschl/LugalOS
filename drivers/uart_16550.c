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
#include "fs/p9_link.h"

/* p9_link_background_poll() opportunistically services the virtio-console
 * 9P link (drivers/virtio_console.c, A3) from inside this busy-wait -- the
 * same spot usb_cdc_task() is already pumped from. It's a no-op unless a
 * background link was registered, and never blocks, so this costs nothing
 * when idle and needs no real task scheduler (this kernel doesn't have
 * one; see the A3 completion notes in plan/phase5_distributed_design.md). */
char uart_getc(void) {
    while (!uart_has_char()) {
        usb_cdc_task();
        p9_link_background_poll();
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

void uart_debug_putc(char c) {
    uart_putc(c);
}

void uart_debug_puts(const char *s) {
    uart_puts(s);
}
