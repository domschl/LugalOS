/* The K210's console: UARTHS at 0x3800_0000, whose pins are hardwired to
 * the board's USB-serial link (Maix Bit: CH552; FPIOA function 14 TX / 15
 * RX, fixed pad wiring per the Maix Bit schematic). The same interface
 * drivers/uart.h gives every other target, so console, shell and printk()
 * above it are unchanged.
 *
 * ## The register frame is K210's own, NOT SiFive's
 *
 * It looks like the SiFive core and is described as one, but the layout
 * differs in exactly the positions that matter (the vendor's own table,
 * kendryte-standalone-sdk lib/drivers/include/uarths.h):
 *
 *   +0x00 txdata   bit31 = fifo full          (read = pop, write = push)
 *   +0x04 rxdata   bit31 = fifo empty         (READ POPS -- see below)
 *   +0x08 txctrl   bit0 txen, bits18:16 txcnt
 *   +0x0C rxctrl   bit0 rxen, bits18:16 rxcnt
 *   +0x10 ie       bit0 txwm, bit1 rxwm
 *   +0x14 ip       bit0 txwm, bit1 rxwm      (hardware level, RW on this die)
 *   +0x18 div      divisor, BROM leaves 0x0d38 = 115200
 *
 * Two traps here cost a full day, both measured on the Maix Bit:
 *
 * 1. Writing 0 to +0x0C because a SiFive map calls it "ie" clears RXEN
 *    and kills the receiver -- TX keeps working perfectly, the FIFO is
 *    simply never filled, and every "uart rx broken" symptom follows
 *    while nothing in the register values looks wrong.
 * 2. A rxdata read POPS the receive FIFO (blob-proven: a raw lw loop
 *    echoes exactly one character per byte received). So rxdata cannot
 *    double as a "is anything there?" peek the way an LSR read does on
 *    16550-derived parts: uart_has_char() asking the FIFO would consume
 *    the very byte it is asking about. The truth lives in ip.rxwm, which
 *    this core drives as a level: set while the FIFO holds more than
 *    rxcnt (0) bytes, clear when it drains.
 *
 * ## This form is polled
 *
 * No interrupt, no driver task: uart_task_start() answers -1 and the
 * facade functions fall back to direct hardware access, which drivers/uart.h
 * documents as the contract ("not fatal"). Moving receive onto PLIC
 * source 33 is 65.6's interrupt work; until then uart_init() must make
 * source 33 *silent*, which it did not need before because the BROM
 * leaves ie.rxwm set and the enable armed -- one keystroke then floods
 * an unhandled level interrupt (the trap layer masks such sources on
 * first sight now, arch/riscv/common/trap.c, but the driver should not
 * rely on that).
 *
 * ## The baud is the boot ROM's
 *
 * The BROM configures this UART at 115200 (divisor 0x0d38 with a ~390 MHz
 * parent) and the ISP that loaded us shares the same wires at the same
 * rate, so uart_init() deliberately does NOT touch the divisor: every
 * clock-tree claim in the databook post-dates the value that actually
 * works, and a "correct" recomputation against the wrong parent clock
 * silently detaches the console. Re-deriving it belongs to 65.6, which
 * can verify against a host-side 115200 receiver.
 *
 * ## Transmit is bounded, always
 *
 * hw_putc() spins a bounded number of times and drops the byte rather
 * than waiting forever (drivers/uart.h's uart_critical_putc comment is
 * the contract; the CH552's FIFO can stall if nobody drains the USB side).
 */

#include "drivers/uart.h"
#include "kernel/sched.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define UARTHS_BASE   ((uintptr_t)CONFIG_UART0_BASE)
#define UART_TXDATA   REG(UARTHS_BASE + 0x00u)
#define UART_RXDATA   REG(UARTHS_BASE + 0x04u)
#define UART_TXCTRL   REG(UARTHS_BASE + 0x08u)
#define UART_RXCTRL   REG(UARTHS_BASE + 0x0Cu)
#define UART_IE       REG(UARTHS_BASE + 0x10u)
#define UART_IP       REG(UARTHS_BASE + 0x14u)
#define UART_DIV      REG(UARTHS_BASE + 0x18u)

#define UART_FULL     (1u << 31)
#define UART_EMPTY    (1u << 31)
#define IP_RXWM       (1u << 1)
#define RXCTRL_RXEN   (1u << 0)

/* PLIC (K210 memory map, enumerated by the 65.1 probe): source 33 is
 * UARTHS; its enable bit is word 33/32 = 1 (0x0C002004), bit 1. */
#define UARTHS_SRC     33u
#define PLIC_PRIORITY33 REG(0x0C000000u + 4u * UARTHS_SRC)
#define PLIC_ENABLE1   REG(0x0C002000u + 4u)

#define TX_SPIN_LIMIT 200000u   /* a full 64-byte FIFO drains in ~5.6 ms at
                                 * 115200; this is far past that, so a drop
                                 * here means nobody is draining at all */

/* single task: written from console writers; the critical path writes it
 * from any context by design (drivers/uart.h) -- a torn full-bit read only
 * drops or duplicates one byte on a dead link. */
static void hw_putc(char c) {
    for (uint32_t i = 0; i < TX_SPIN_LIMIT; i++) {
        if (!(UART_TXDATA & UART_FULL)) {
            UART_TXDATA = (uint32_t)(unsigned char)c;
            return;
        }
    }
    /* dropped: the only honest reading is that nothing reads the wire. */
}

void uart_init(uintptr_t base_addr) {
    (void)base_addr;    /* base is fixed (UARTHS is the only console path);
                         * divisor untouched -- see the file header */

    /* Say what we mean about every bit, in the order the quirks want.
     *
     * ie first (the real one, +0x10): polled reception must not arm the
     * PLIC. Then clear ip -- a level the hardware drives, but the BROM
     * leaves its own rxwm latch up, and with the enable bit it had set
     * that is one unhandled IRQ 33 waiting per keystroke. Then rxctrl:
     * rxen on with rxcnt 0 ("any byte is a level"), because nothing else
     * in this boot path guarantees it and a lost rxen is a silent dead
     * receiver. Drain last: with rxen confirmed, the drain is the driver's
     * property from here on. */
    UART_IE     = 0u;
    UART_IP     = 0u;
    UART_RXCTRL = RXCTRL_RXEN;
    while (!(UART_RXDATA & UART_EMPTY)) (void)UART_RXDATA;

    /* And belt-and-braces at the PLIC: clear the BROM's enable and
     * priority for source 33 so this wire cannot interrupt even if the
     * uart core's level rises. The trap layer masks unhandled level
     * sources as a backstop (arch/riscv/common/trap.c), but a clean
     * driver leaves nothing to catch. */
    PLIC_ENABLE1 &= ~(1u << (UARTHS_SRC % 32u));
    PLIC_PRIORITY33 = 0u;
}

int uart_task_start(void) { return -1; }    /* polled form; see header */

void uart_putc(char c) {
    hw_putc(c);         /* raw, like every target's uart_putc(): the CRLF
                         * for lines happens in uart_puts()/the klog sink,
                         * never here -- double \n expansion is visible and
                         * this driver copies that contract exactly */
}

void uart_flush(void) { /* unbuffered: nothing pending, ever */ }

bool uart_has_char(void) {
    /* ip.rxwm, never a rxdata read -- the read pops (file header §2). */
    return (UART_IP & IP_RXWM) != 0u;
}

char uart_getc(void) {
    for (;;) {
        if (UART_IP & IP_RXWM) {
            uint32_t r = UART_RXDATA;   /* pops exactly the byte rxwm counts */
            if (!(r & UART_EMPTY)) return (char)(r & 0xffu);
        }
        sched_yield();
    }
}

/* A FIFO cannot be inspected without consuming: kernel/console.c's pump
 * keeps its usual behaviour, as on QEMU, the P4, and the C6. */
bool uart_peek_interrupt(void) { return false; }

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

/* --- drivers/uart.h facade extras ------------------------------------- */

/* The critical path is "never worse than the ordinary one" (drivers/uart.h);
 * with no interrupt and no buffering here, that is simply hw_putc -- which
 * is bounded by construction, so it cannot hold the console hostage the way
 * an interrupt-mode driver's spin could. */
void uart_critical_putc(char c) { hw_putc(c); }
void uart_flush_critical(void) { /* unbuffered */ }

void uart_debug_putc(char c) { hw_putc(c); }
void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) hw_putc(*s++);
}

/* Polled driver: there are no interrupts to count. The shell's `uartinfo`
 * reports zeros, which is the truth rather than a gap. */
uint32_t uart_write_call_count(void) { return 0; }
uint32_t uart_irq_count(void) { return 0; }
uint32_t uart_irq_rx_wakes(void) { return 0; }
uint32_t uart_irq_tx_wakes(void) { return 0; }
uint32_t uart_irq_tx_arms(void) { return 0; }
uint32_t uart_irq_tx_seen(void) { return 0; }
uint32_t uart_rx_overruns(void) { return 0; }
