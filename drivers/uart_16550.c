#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "kernel/devirq.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "arch/trap.h"

static volatile uint8_t *uart_base = (volatile uint8_t *)0x10000000; // QEMU virt UART0 base

/* 16550 Register Offsets */
#define UART_RBR 0 // Receive Buffer Register
#define UART_THR 0 // Transmit Holding Register
#define UART_IER 1 // Interrupt Enable Register
#define UART_LSR 5 // Line Status Register
#define UART_LSR_DR   0x01 // Data Ready
#define UART_LSR_THRE 0x20 // Transmit Holding Register Empty
#define UART_IER_ERBFI 0x01 // Enable Received Data Available interrupt
#define UART_IER_ETBEI 0x02 // Enable Transmitter Holding Register Empty interrupt

/* QEMU virt's ns16550, IRQ 10 -- verified against qemu/include/hw/riscv/virt.h
 * rather than assumed (M2, plan/phase12_microkernel_migration.md). */
#define UART0_IRQ 10u

/* M2, revised M2.5: the ISR-driven wake this file lacked entirely before --
 * both directions. See kernel/printk.c for why TX blocking is safe now.
 *
 * The 16550's RX/TX-ready conditions are level-triggered on FIFO fill
 * state, not edge/latched, so an interrupt source left enabled with nothing
 * waiting on it fires on every return from interrupt for as long as the
 * condition holds -- an interrupt storm, not a hang, but one that starves
 * every other task just as effectively. So IER's bits are enabled only for
 * the duration of an actual wait, by whichever task is about to block, and
 * disabled again by the ISR the moment it has served that one waiter.
 *
 * Single-slot, not a queue: at most one waiter per direction. A second,
 * concurrent blocking call in the same direction falls back to plain
 * polling rather than being silently dropped or queued. */
static volatile int g_rx_waiter = -1;
static volatile int g_tx_waiter = -1;

static void uart_isr(void *ctx) {
    (void)ctx;
    uint8_t lsr = uart_base[UART_LSR];
    if ((lsr & UART_LSR_DR) && g_rx_waiter >= 0) {
        uart_base[UART_IER] &= (uint8_t)~UART_IER_ERBFI;
        int pid = g_rx_waiter;
        g_rx_waiter = -1;
        task_unblock(pid);
    }
    if ((lsr & UART_LSR_THRE) && g_tx_waiter >= 0) {
        uart_base[UART_IER] &= (uint8_t)~UART_IER_ETBEI;
        int pid = g_tx_waiter;
        g_tx_waiter = -1;
        task_unblock(pid);
    }
}

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
    /* Handler before enable (arch/trap.h): so that if the IRQ somehow fires
     * before this returns, there is a handler to find it rather than an
     * unmasked source with nothing behind it. */
    devirq_attach(UART0_IRQ, uart_isr, NULL);
    arch_irq_enable(UART0_IRQ);
}

void uart_putc(char c) {
    if (!uart_base) return;
    if ((uart_base[UART_LSR] & UART_LSR_THRE) != 0) {
        uart_base[UART_THR] = (uint8_t)c;
        return;
    }
    /* Nothing between the fast-path miss and task_block() can restore
     * interrupts early, or a TX interrupt landing in the gap finds this task
     * still RUNNING (not yet BLOCKED), task_unblock() no-ops, and the ISR
     * has already disabled ETBEI having "served" a wakeup nobody was asleep
     * for -- a silent, permanent lost wakeup. */
    uintptr_t flags = irq_save();
    if ((uart_base[UART_LSR] & UART_LSR_THRE) == 0) {
        if (g_tx_waiter < 0) {
            g_tx_waiter = sched_current_pid();
            uart_base[UART_IER] |= UART_IER_ETBEI;
            task_block();
        } else {
            irq_restore(flags);
            while ((uart_base[UART_LSR] & UART_LSR_THRE) == 0) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    uart_base[UART_THR] = (uint8_t)c;
}

// When the A3b demux is enabled (opt-in, via `p9share`), the console can no
// longer read the hardware register directly -- some of what's waiting
// there may be 9P frame bytes. uart_demux_console_has_char() pumps the
// hardware and returns only the console-routed subset.
bool uart_has_char(void) {
    if (uart_demux_is_enabled()) return uart_demux_console_has_char();
    return hw_uart_has_char();
}

char uart_getc(void) {
    if (uart_demux_is_enabled()) {
        while (!uart_demux_console_has_char()) sched_yield();
        return uart_demux_console_getc();
    }
    if (hw_uart_has_char()) return (char)hw_uart_getc();
    uintptr_t flags = irq_save();
    if (!hw_uart_has_char()) {
        if (g_rx_waiter < 0) {
            g_rx_waiter = sched_current_pid();
            uart_base[UART_IER] |= UART_IER_ERBFI;
            task_block();
        } else {
            irq_restore(flags);
            while (!hw_uart_has_char()) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
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

// Low-level driver diagnostics only (see kernel/printk.h's printk_debug()).
void uart_debug_putc(char c) {
    uart_putc(c);
}

void uart_debug_puts(const char *s) {
    uart_puts(s);
}
