#include "drivers/uart.h"
#include "drivers/uart_net.h"

static volatile uint8_t *uart_base = (volatile uint8_t *)0x10000000; // QEMU virt UART0 base

/* 16550 Register Offsets */
#define UART_RBR 0 // Receive Buffer Register
#define UART_THR 0 // Transmit Holding Register
#define UART_LSR 5 // Line Status Register
#define UART_LSR_DR   0x01 // Data Ready
#define UART_LSR_THRE 0x20 // Transmit Holding Register Empty

// Raw hardware byte access, handed to drivers/uart_net.c's A3b demux at
// init time. Kept separate from uart_has_char()/uart_getc() below so the
// demux (when a user opts into it via `p9share`) can pull straight from the
// hardware register without going through the console-ring indirection it
// is itself responsible for providing.
static bool hw_uart_has_char(void) {
    if (!uart_base) return false;
    return (uart_base[UART_LSR] & UART_LSR_DR) != 0;
}

static uint8_t hw_uart_getc(void) {
    return uart_base[UART_RBR];
}

void uart_init(uintptr_t base_addr) {
    if (base_addr != 0) {
        uart_base = (volatile uint8_t *)base_addr;
    }
    uart_demux_init(hw_uart_has_char, hw_uart_getc);
}

void uart_putc(char c) {
    if (!uart_base) return;
    while ((uart_base[UART_LSR] & UART_LSR_THRE) == 0);
    uart_base[UART_THR] = (uint8_t)c;
}

// When the A3b demux is enabled (opt-in, via `p9share`), the console can no
// longer read the hardware register directly -- some of what's waiting
// there may be 9P frame bytes. uart_demux_console_has_char() pumps the
// hardware and returns only the console-routed subset; disabled (the
// default), this is exactly the old direct register check.
bool uart_has_char(void) {
    if (uart_demux_is_enabled()) return uart_demux_console_has_char();
    return hw_uart_has_char();
}

#include "drivers/usb_cdc.h"
#include "fs/p9_link.h"
#include "kernel/sched.h"

/* p9_link_background_poll() opportunistically services whatever background
 * 9P links are registered (drivers/virtio_console.c, A3; the A3b UART demux
 * link once `p9share` is active) from inside this busy-wait -- the same
 * spot usb_cdc_task() is already pumped from. It's a no-op unless a
 * background link was registered, and never blocks, so this costs nothing
 * when idle and needs no real task scheduler (this kernel doesn't have
 * one; see the A3 completion notes in plan/phase5_distributed_design.md). */
char uart_getc(void) {
    while (!uart_has_char()) {
        usb_cdc_task();
        p9_link_background_poll();
        /* B2: the console blocking on a keystroke is the single longest wait
         * in the system, so it is the most important yield point in the tree
         * -- without it every other task would be starved for as long as the
         * user is thinking. No-op until sched_init() has run. */
        sched_yield();
    }
    if (uart_demux_is_enabled()) return uart_demux_console_getc();
    return (char)hw_uart_getc();
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
