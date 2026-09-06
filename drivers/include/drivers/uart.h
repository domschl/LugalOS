#ifndef LUGALOS_DRIVERS_UART_H
#define LUGALOS_DRIVERS_UART_H

#include <stdint.h>
#include <stdbool.h>

void uart_init(uintptr_t base_addr);

// M4/M4.5, plan/phase12_microkernel_migration.md: starts the dedicated uart
// task, which becomes the sole owner of the hardware for uart_putc()/
// uart_getc()/uart_has_char() below. Must run after sched_init() -- unlike
// uart_init(), which only brings the hardware itself up and runs long
// before a task table exists. Returns the task's pid, or -1 if it could
// not be started (not fatal: the facade functions below fall back to
// direct hardware access whenever the task is not alive). Both
// drivers/uart_16550.c (QEMU) and drivers/uart_rp2350.c (real hardware,
// converted in M4.5 Part B -- the extra USB CDC mirror and the A3b demux
// bypass are that file's own, not duplicated here) implement this.
int uart_task_start(void);

void uart_putc(char c);
char uart_getc(void);
bool uart_has_char(void);

/* True if an interrupt (Ctrl-C) is sitting unread in the console device,
 * WITHOUT consuming anything. kernel/console.c's pump uses this to latch an
 * interrupt even when its own ring is full and it must stop draining -- see
 * console_pump() for the failure this exists to prevent. Where a device
 * cannot be inspected without consuming (a plain UART FIFO), this answers
 * false and the pump behaves as it always did. */
bool uart_peek_interrupt(void);
void uart_puts(const char *s);

// M4/M4.5 verify: how many batched-write chan_call()s the uart task has
// served since boot -- see drivers/uart_16550.c's g_uart_write_calls
// comment.
uint32_t uart_write_call_count(void);

// How many times this driver's interrupt handler has run since boot.
//
// The counterpart to uart_write_call_count(), and added for the same kind of
// reason: a console that answers keystrokes proves nothing about *how* it
// noticed them, because every one of these drivers keeps a sched_yield()
// polling fallback underneath the ISR for the paths that cannot block. The
// two are indistinguishable from the far end of the cable. A count that
// grows is the evidence.
//
// Written on ESP32-P4 bringup (E3, plan/phase27_esp32p4_bringup.md), where
// the question was live -- the interrupt controller was new that day -- and
// implemented on all three console drivers so `uartstats` means the same
// thing on every target rather than one.
uint32_t uart_irq_count(void);

// Of those, how many woke a task that was blocked waiting to *read*.
//
// The number that actually answers "is the console interrupt-driven?", which
// the total does not: a driver whose TX path blocks on an interrupt and whose
// RX path polls produces a healthy-looking irq count and still spins on every
// keystroke. Splitting them is what makes E3's done-condition -- "a UART RX
// interrupt reaches a handler" -- checkable rather than plausible.
//
// drivers/uart_rp2350.c returns 0 and always will: its ISR serves TX only,
// because that board's RX arrives through two paths (the PL011 and the USB
// CDC mirror) and the console pump polls both. That is a real difference
// between the drivers, so it is reported rather than smoothed over.
uint32_t uart_irq_rx_wakes(void);

// Sends whatever uart_putc() has batched but not yet transmitted. See
// drivers/uart_16550.c's uart_putc()/uart_flush() for why writes are
// batched at all (one chan_call() per line of output, not one per
// character) and for the two places this must be called explicitly
// (printk_unlock()'s outermost unlock, and before this driver itself
// blocks waiting for the next keystroke) rather than relying only on the
// batch filling up. A no-op wherever nothing is pending.
void uart_flush(void);

// Console output from a context that must not block, yield, or switch:
// scheduler teardown, interrupt handlers, and fatal paths.
//
// Everything else in this header can block. uart_putc() batches and
// uart_flush() reaches the console through chan_call() -> task_block();
// uart_debug_putc(), despite the name, still ends in a blocking primitive
// that calls task_block() when a task exists and sched_yield() when one
// does not. Either is fatal in the three contexts above: a task blocking in
// the middle of task_exit() switches away with its bookkeeping half done, a
// blocking call from the timer interrupt is a deadlock, and a fatal handler
// that blocks never prints the dump that would explain it.
//
// This spins on the transmit FIFO for a bounded number of iterations and
// then **drops the byte**. Dropping is the point: output is worth less than
// forward progress on these paths, and a console that is wedged or absent
// must not be able to stop the kernel. See kernel/printk.h's
// printk_critical(), which is the only intended caller.
void uart_critical_putc(char c);
void uart_critical_puts(const char *s);

// Drain whatever uart_putc() has batched, using the same bounded, never
// blocking writes as uart_critical_putc().
//
// Ordering, not throughput, is why this exists. Ordinary output sits in a
// per-hart batch until uart_flush() sends it; critical output goes straight
// at the hardware. Without draining first, a critical line overtakes text
// that was printed before it -- which is not merely untidy, it makes the
// console a misleading record of what happened, and it broke four tests that
// wait for output in the order it was produced.
void uart_flush_critical(void);

// Kernel debug log output (used by printk()) only: physical UART, never
// mirrored to a USB CDC console. Keeps kernel/driver diagnostics off the
// interactive shell's USB data channel, which uart_putc()/uart_puts() do
// mirror to.
void uart_debug_putc(char c);
void uart_debug_puts(const char *s);

#endif /* LUGALOS_DRIVERS_UART_H */
