/* The ESP32-C6's console: the USB-Serial/JTAG peripheral (45.4,
 * plan/phase45_esp32c6.md). The same interface drivers/uart.h gives every other
 * target, so the console, the shell and printk() above it are unchanged.
 *
 * The Waveshare C6-Zero has no UART bridge: its one USB-C connector is wired
 * straight to this peripheral (VID:PID 303a:1001), which is also what the boot
 * ROM prints on and what esptool loads over. One cable, one port, console and
 * reset together.
 *
 * ## This first form is polled
 *
 * No interrupt, no driver task: uart_task_start() answers -1 and the facade
 * functions fall back to direct hardware access, which drivers/uart.h
 * documents as the contract ("not fatal"). The interrupt line exists in the
 * peripheral (INTMTX source 48) and 45.4.2 brings the controller up and moves
 * the receive side onto it; until then a task that waits for a key yields in a
 * loop, like every console in this tree did before M4.
 *
 * ## Transmit is bounded, always
 *
 * With no host reading the port the 64-byte IN FIFO fills and never drains: an
 * unbounded wait would hang the kernel on the first line it prints, and "the
 * kernel never started" and "nobody is listening" are indistinguishable from the
 * far end of a cable (the same trap tools/minimal_esp32p4.c documents for its
 * receive drain, and minimal_esp32c6.c for this one). A byte that finds no room
 * after a bounded wait is dropped; the FIFO is flushed once the host returns.
 *
 * Registers: IDF components/soc/esp32c6/register/soc/usb_serial_jtag_reg.h,
 * confirmed in 45.1 (EP1 at +0, EP1_CONF at +4, bits 0/1/2).
 */

#include "drivers/uart.h"
#include "kernel/sched.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define USJ_EP1(b)       ((b) + 0x0)
#define USJ_EP1_CONF(b)  ((b) + 0x4)
#define USJ_WR_DONE      (1u << 0)     /* write 1: hand the written bytes to the host */
#define USJ_IN_FREE      (1u << 1)     /* the IN FIFO has room for a byte */
#define USJ_OUT_AVAIL    (1u << 2)     /* a received byte is waiting in EP1 */

#define FIFO_BYTES       64u
#define TX_SPIN_LIMIT    200000u

static uintptr_t g_base = CONFIG_USBJTAG_BASE;
static volatile unsigned g_pending;     /* bytes written since the last WR_DONE */
static volatile bool g_host_gone;       /* the last wait timed out: do not spin again until there is room */

static void hw_flush(void) {
    if (g_pending) {
        REG(USJ_EP1_CONF(g_base)) = USJ_WR_DONE;
        g_pending = 0;
    }
}

static void hw_putc(char c) {
    if (g_host_gone) {
        if (!(REG(USJ_EP1_CONF(g_base)) & USJ_IN_FREE)) return;     /* still nobody: one look, no spin */
        g_host_gone = false;
    } else {
        unsigned n = 0;
        while (!(REG(USJ_EP1_CONF(g_base)) & USJ_IN_FREE)) {
            if (++n >= TX_SPIN_LIMIT) { g_host_gone = true; g_pending = 0; return; }
        }
    }
    REG(USJ_EP1(g_base)) = (uint32_t)(uint8_t)c;
    if (++g_pending >= FIFO_BYTES || c == '\n') hw_flush();
}

void uart_init(uintptr_t base_addr) {
    (void)base_addr;            /* CONFIG_UART0_BASE is the chip's UART0, not this port */
}

int uart_task_start(void) { return -1; }

void uart_putc(char c) { hw_putc(c); }

void uart_flush(void) { hw_flush(); }

bool uart_has_char(void) {
    hw_flush();
    return (REG(USJ_EP1_CONF(g_base)) & USJ_OUT_AVAIL) != 0;
}

char uart_getc(void) {
    hw_flush();
    while (!(REG(USJ_EP1_CONF(g_base)) & USJ_OUT_AVAIL)) sched_yield();
    return (char)(REG(USJ_EP1(g_base)) & 0xffu);
}

/* A FIFO cannot be inspected without consuming: kernel/console.c's pump keeps
 * its usual behaviour, as on QEMU and the P4. */
bool uart_peek_interrupt(void) { return false; }

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

/* Console output that must not block, yield or switch: the fatal handler, the
 * scheduler's teardown, interrupt context. hw_putc() is already bounded and
 * never yields, and each critical byte is flushed so a line that does not end
 * in a newline still reaches the host before the machine stops. */
void uart_critical_putc(char c) {
    hw_putc(c);
    hw_flush();
}

void uart_critical_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_critical_putc('\r');
        uart_critical_putc(*s++);
    }
}

void uart_flush_critical(void) { hw_flush(); }

void uart_debug_putc(char c) { hw_putc(c); }

void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_debug_putc('\r');
        uart_debug_putc(*s++);
    }
}

/* No driver task, no interrupts, no batching: every counter is honestly zero. */
uint32_t uart_write_call_count(void) { return 0; }
uint32_t uart_irq_count(void) { return 0; }
uint32_t uart_irq_rx_wakes(void) { return 0; }
uint32_t uart_rx_overruns(void) { return 0; }
uint32_t uart_irq_tx_arms(void) { return 0; }
uint32_t uart_irq_tx_wakes(void) { return 0; }
uint32_t uart_irq_tx_seen(void) { return 0; }
