/* The ESP32-C6's console: the USB-Serial/JTAG peripheral. The same interface drivers/uart.h gives every other
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
 * loop, like every console in this tree did previously.
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
#include "kernel/lock.h"
#include "drivers/usbjtag_esp32c6.h"
#include "arch/esp32c6_intr.h"
#include "arch/trap.h"
#include "kernel/devirq.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

#define USJ_EP1(b)       ((b) + 0x0)
#define USJ_EP1_CONF(b)  ((b) + 0x4)
#define USJ_WR_DONE      (1u << 0)     /* write 1: hand the written bytes to the host */
#define USJ_IN_FREE      (1u << 1)     /* the IN FIFO has room for a byte */
#define USJ_OUT_AVAIL    (1u << 2)     /* a received byte is waiting in EP1 */

#define FIFO_BYTES       64u
#define TX_SPIN_LIMIT    200000u

/* accepted: g_pending is the console writers', except that the critical path
 * (uart_critical_putc()) writes it from any context, by design: a critical
 * message may interleave with console bytes, and a torn count only flushes a
 * packet early or late. g_base is constant. */
static uintptr_t g_base = CONFIG_USBJTAG_BASE;
static volatile unsigned g_pending;     /* bytes written since the last WR_DONE */

static void hw_flush(void) {
    if (g_pending) {
        REG(USJ_EP1_CONF(g_base)) = USJ_WR_DONE;
        g_pending = 0;
    }
}

/* --- The console on UART0 too ---
 *
 * The USB console is silent exactly when it is needed most: before the host
 * has the port open, after a reset re-enumerates it, and when the board hangs.
 * UART0 has neither problem, and the boot ROM has already set it up at 115200
 * for its own banner. So every console byte also goes to UART0, and every
 * byte UART0 receives is console input beside the USB's. While a USB host
 * reads, UART0 is a copy and a byte that finds its FIFO full is dropped; once
 * nobody reads the USB (g_host_gone), UART0 is the console and waits for
 * room -- a bounded wait, since a UART drains whether or not anything is
 * attached (128 bytes at 115200 is 11 ms). On the LCD-7B's C6 UART0 is the
 * only console: its header H4 carries IO9, GND and UART0, not the USB pair.
 * LLP's frames go to the USB only. Pins: the board file's
 * CONFIG_UART0_TX/RX_GPIO (GPIO16/17, the ROM's U0TXD/U0RXD, with the ROM's
 * pull-up on RX, so an open header reads idle). */
#define UART0_FIFO      (CONFIG_UART0_BASE + 0x00u)
#define UART0_INT_RAW   (CONFIG_UART0_BASE + 0x04u)
#define UART0_INT_CLR   (CONFIG_UART0_BASE + 0x10u)
#define UART0_RXFIFO_OVF (1u << 4)
#define UART0_RXFIFO_FULL (1u << 0)
#define UART0_RXFIFO_TOUT (1u << 8)
#define UART0_INT_ENA   (CONFIG_UART0_BASE + 0x0cu)
#define UART0_CONF1     (CONFIG_UART0_BASE + 0x24u)    /* RX FIFO full threshold, 7:0 */
#define UART0_TOUT_CONF (CONFIG_UART0_BASE + 0x64u)    /* RX timeout: enable bit 0, threshold 11:2 (bit times) */
#define UART0_CLKDIV    (CONFIG_UART0_BASE + 0x14u)   /* integer 11:0, fraction 23:20 */
#define UART0_REG_UPDATE (CONFIG_UART0_BASE + 0x98u)
#define UART0_STATUS    (CONFIG_UART0_BASE + 0x1cu)
#define UART0_RXCNT(v)  ((v) & 0xffu)
#define UART0_TXCNT(v)  (((v) >> 16) & 0xffu)
#define UART0_TX_ROOM   120u
#define UART0_SPIN_LIMIT 2000000u

/* accepted: as g_pending above. */
static volatile bool g_host_gone;       /* the last USB wait timed out: do not spin again until there is room */
/* LLP on UART0 instead of the USB (usbjtag_llp_use_uart()): UART0 is then LLP's
 * alone -- no console copy goes out on it, and everything it receives is LLP's.
 * single task: set once, under g_mux_lock, by LLP's start, before UART0's
 * interrupt is attached. */
static volatile bool g_llp_on_uart;

static void uart0_put(char c) {
    if (g_host_gone) {
        unsigned n = 0;
        while (UART0_TXCNT(REG(UART0_STATUS)) >= UART0_TX_ROOM)
            if (++n >= UART0_SPIN_LIMIT) return;
    } else if (UART0_TXCNT(REG(UART0_STATUS)) >= UART0_TX_ROOM) {
        return;
    }
    REG(UART0_FIFO) = (uint32_t)(uint8_t)c;
}

static bool uart0_rx_avail(void) { return UART0_RXCNT(REG(UART0_STATUS)) != 0u; }
static uint8_t uart0_rx_get(void) { return (uint8_t)(REG(UART0_FIFO) & 0xffu); }

/* One byte to the USB FIFO only. */
static void usj_putc(char c) {
    /* "Nobody is reading" must still *commit* what is in the FIFO (WR_DONE).
     * It used to drop the count without it, and a FIFO full of uncommitted
     * bytes never drains -- IN_FREE stays clear, every later byte takes the
     * one-look path, and the console is dead for good while the kernel runs
     * on. That is what a cold boot did (49.4, 2026-10-12): a power-on reset
     * re-enumerates the port, the host takes about a second to configure it,
     * and the first lines of the boot log filled the FIFO before anyone read.
     * A warm reset over USB keeps the host reading and never showed it. */
    if (g_host_gone) {
        if (!(REG(USJ_EP1_CONF(g_base)) & USJ_IN_FREE)) {           /* still nobody: one look, no spin */
            REG(USJ_EP1_CONF(g_base)) = USJ_WR_DONE;                /* and the FIFO stays committed */
            return;
        }
        g_host_gone = false;
    } else {
        unsigned n = 0;
        while (!(REG(USJ_EP1_CONF(g_base)) & USJ_IN_FREE)) {
            if (++n >= TX_SPIN_LIMIT) {
                REG(USJ_EP1_CONF(g_base)) = USJ_WR_DONE;
                g_host_gone = true;
                g_pending = 0;
                return;
            }
        }
    }
    REG(USJ_EP1(g_base)) = (uint32_t)(uint8_t)c;
    if (++g_pending >= FIFO_BYTES || c == '\n') hw_flush();
}

/* One console byte: both wires, or only the USB while UART0 is LLP's. */
static void hw_putc(char c) {
    usj_putc(c);
    if (!g_llp_on_uart) uart0_put(c);
}

/* An LLP byte to UART0: a UART drains whether or not anything listens, so this
 * waits for room (bounded) rather than drop part of a frame. */
static void uart0_put_llp(uint8_t b) {
    unsigned n = 0;
    while (UART0_TXCNT(REG(UART0_STATUS)) >= UART0_TX_ROOM)
        if (++n >= UART0_SPIN_LIMIT) return;
    REG(UART0_FIFO) = (uint32_t)b;
}

/* --- Sharing the port with LLP ---
 *
 * The C6-Zero has one cable. To be a host's network card over it, LLP's
 * serial frames (SLIP: END ... END, doc/llp.md §7.1) share the port with the
 * console, as `p9share` shares a UART with 9P:
 *
 * * **In**, every byte is routed: from a SLIP_END to the next one, to LLP;
 *   anything outside a frame, to the console. Typed text never contains 0xC0
 *   (it is not valid UTF-8 either).
 * * **Out**, LLP's pump is the only writer to the FIFO while LLP is armed.
 *   Console output from every other task goes into a queue the pump writes
 *   between frames -- so no console byte can land inside a frame, and nothing
 *   masks interrupts for the length of one (the radio's interrupt path runs on
 *   this core). Fatal-path output (uart_critical_putc) stays direct: when the
 *   machine is going down, a mangled frame is the lesser loss.
 *
 * Off by default: until `llp radio` arms it, this driver is byte for byte what
 * it was. */

#define LLP_RX_CAP   4096u
#define CON_RX_CAP   256u
#define CON_TX_CAP   2048u

typedef struct { uint8_t *buf; uint32_t cap, head, tail; uint32_t drops; } bring_t;

/* guarded by: g_mux_lock with interrupts masked (one core), which also keeps
 * uart0_isr() out; that handler is g_llp_rx's other producer. */
static uint8_t g_llp_rx_mem[LLP_RX_CAP], g_con_rx_mem[CON_RX_CAP], g_con_tx_mem[CON_TX_CAP];
static bring_t g_llp_rx = { g_llp_rx_mem, LLP_RX_CAP, 0, 0, 0 };
static bring_t g_con_rx = { g_con_rx_mem, CON_RX_CAP, 0, 0, 0 };
static bring_t g_con_tx = { g_con_tx_mem, CON_TX_CAP, 0, 0, 0 };
static volatile bool g_llp_armed;
static bool g_in_frame;
static spinlock_t g_mux_lock = SPINLOCK_INIT;

static void br_put(bring_t *r, uint8_t c) {
    if (r->head - r->tail >= r->cap) { r->drops++; return; }
    r->buf[r->head++ % r->cap] = c;
}
static bool br_get(bring_t *r, uint8_t *c) {
    if (r->head == r->tail) return false;
    *c = r->buf[r->tail++ % r->cap];
    return true;
}

/* interrupt-shared: counted by uart0_isr() and by drain_rx_locked() with
 * interrupts masked. */
static uint32_t g_uart_overflows;           /* RX FIFO overflows while UART0 is LLP's */

/* UART0 for LLP is drained in its interrupt: the FIFO holds 128 bytes, 11 ms
 * at 115200, and the pump is one task among the radio's -- polled, frames of
 * a few hundred bytes were lost (measured on the RP2350 terminal's H7: half of
 * the 1500-byte ones). The interrupt fires at 32 bytes waiting, or when the
 * line has been idle for 10 bit times with anything waiting. Single core: the
 * readers take g_mux_lock with interrupts off, so this never runs inside them. */
static void uart0_isr(void *ctx) {
    (void)ctx;
    uint32_t raw = REG(UART0_INT_RAW);
    if (raw & UART0_RXFIFO_OVF) g_uart_overflows++;
    while (uart0_rx_avail()) br_put(&g_llp_rx, uart0_rx_get());
    REG(UART0_INT_CLR) = UART0_RXFIFO_FULL | UART0_RXFIFO_TOUT | UART0_RXFIFO_OVF;
}

static void uart0_rx_interrupt_on(void) {
    REG(UART0_CONF1) = (REG(UART0_CONF1) & ~0xffu) | 32u;
    REG(UART0_TOUT_CONF) = (REG(UART0_TOUT_CONF) & ~0xfffu) | (10u << 2) | 1u;
    REG(UART0_REG_UPDATE) = 1u;
    for (unsigned n = 0; (REG(UART0_REG_UPDATE) & 1u) && n < 100000u; n++) { }
    REG(UART0_INT_CLR) = 0xffffffffu;
    if (esp32c6_intmtx_route(ESP32C6_SRC_UART0, ESP32C6_IRQ_UART0) != 0) return;
    devirq_attach(ESP32C6_IRQ_UART0, uart0_isr, NULL);
    REG(UART0_INT_ENA) = UART0_RXFIFO_FULL | UART0_RXFIFO_TOUT | UART0_RXFIFO_OVF;
    arch_irq_enable(ESP32C6_IRQ_UART0);
}

/* Everything the host has sent, routed. Caller holds g_mux_lock. */
static void drain_rx_locked(void) {
    if (g_llp_on_uart) {
        /* Two wires, one job each: the USB is the console, UART0 is LLP. */
        while (REG(USJ_EP1_CONF(g_base)) & USJ_OUT_AVAIL) br_put(&g_con_rx, (uint8_t)(REG(USJ_EP1(g_base)) & 0xffu));
        if (REG(UART0_INT_RAW) & UART0_RXFIFO_OVF) { g_uart_overflows++; REG(UART0_INT_CLR) = UART0_RXFIFO_OVF; }
        while (uart0_rx_avail()) br_put(&g_llp_rx, uart0_rx_get());
        return;
    }
    while (REG(USJ_EP1_CONF(g_base)) & USJ_OUT_AVAIL) {
        uint8_t c = (uint8_t)(REG(USJ_EP1(g_base)) & 0xffu);
        if (c == 0xC0) {                      /* SLIP_END: opens a frame, or closes the open one */
            br_put(&g_llp_rx, c);
            g_in_frame = !g_in_frame;
        } else if (g_in_frame) {
            br_put(&g_llp_rx, c);
        } else {
            br_put(&g_con_rx, c);
        }
    }
    while (uart0_rx_avail()) br_put(&g_con_rx, uart0_rx_get());   /* UART0 never carries LLP */
}

/* UART0's divider for `baud`, scaled from the one the boot ROM left for 115200:
 * the clock behind it need not be known. Taken once, before any change. */
static uint32_t g_div16_115200;   /* accepted: taken lazily, the same value by every taker */

static bool uart0_set_baud(uint32_t baud) {
    if (!g_div16_115200) {
        uint32_t v = REG(UART0_CLKDIV);
        g_div16_115200 = ((v & 0xfffu) << 4) | ((v >> 20) & 0xfu);
    }
    if (baud == 0) return false;
    uint32_t d16 = (uint32_t)(((uint64_t)g_div16_115200 * 115200u + baud / 2u) / baud);
    if (d16 < 16u || (d16 >> 4) > 0xfffu) return false;
    REG(UART0_CLKDIV) = (REG(UART0_CLKDIV) & ~((0xfffu) | (0xfu << 20))) | (d16 >> 4) | ((d16 & 0xfu) << 20);
    REG(UART0_REG_UPDATE) = 1u;
    for (unsigned n = 0; (REG(UART0_REG_UPDATE) & 1u) && n < 100000u; n++) { }
    return true;
}

int usbjtag_llp_use_uart(uint32_t baud) {
    if (!uart0_set_baud(baud)) return -1;
    uintptr_t f = spin_lock_irqsave(&g_mux_lock);
    g_llp_on_uart = true;
    spin_unlock_irqrestore(&g_mux_lock, f);
    uart0_rx_interrupt_on();
    return 0;
}

bool usbjtag_llp_on_uart(void) { return g_llp_on_uart; }

/* A host on the USB sends a start-of-frame every millisecond; with none in
 * 20 ms, nothing is attached (or nothing has configured the port). */
#define USJ_INT_RAW(b)  ((b) + 0x8)
#define USJ_INT_CLR(b)  ((b) + 0x14)
#define USJ_SOF         (1u << 1)

bool usbjtag_host_present(void) {
    REG(USJ_INT_CLR(g_base)) = USJ_SOF;
    for (int i = 0; i < 20; i++) {
        task_sleep_ms(1);
        if (REG(USJ_INT_RAW(g_base)) & USJ_SOF) return true;
    }
    return false;
}

void usbjtag_llp_arm(bool on) {
    uintptr_t f = spin_lock_irqsave(&g_mux_lock);
    g_llp_armed = on;
    g_in_frame = false;
    spin_unlock_irqrestore(&g_mux_lock, f);
}

int usbjtag_llp_read(uint8_t *buf, uint32_t max) {
    uintptr_t f = spin_lock_irqsave(&g_mux_lock);
    drain_rx_locked();
    uint32_t n = 0;
    while (n < max && br_get(&g_llp_rx, &buf[n])) n++;
    spin_unlock_irqrestore(&g_mux_lock, f);
    return (int)n;
}

/* The pump's turn at the FIFO: console text first (it was waiting), then the
 * frame, then flush. Only the pump calls this, so the FIFO has one writer. */
static void con_tx_flush(void) {
    for (;;) {
        uint8_t c;
        uintptr_t f = spin_lock_irqsave(&g_mux_lock);
        bool have = br_get(&g_con_tx, &c);
        spin_unlock_irqrestore(&g_mux_lock, f);
        if (!have) break;
        hw_putc((char)c);
    }
}

int usbjtag_llp_write(const uint8_t *buf, uint32_t len) {
    con_tx_flush();
    for (uint32_t i = 0; i < len; i++) {
        if (g_llp_on_uart) uart0_put_llp(buf[i]);
        else usj_putc((char)buf[i]);
    }
    hw_flush();
    return (int)len;
}

/* A frame streamed byte by byte (llp_serial_emit()): the first byte of a frame
 * is its SLIP_END, which is where waiting console text goes out first. */
static bool g_streaming;   /* single task: LLP's sender */

void usbjtag_llp_putc(uint8_t b) {
    if (!g_streaming) { con_tx_flush(); g_streaming = true; }
    if (g_llp_on_uart) uart0_put_llp(b);
    else usj_putc((char)b);
}

void usbjtag_llp_end(void) {
    hw_flush();
    g_streaming = false;
}

void usbjtag_llp_service(void) {
    con_tx_flush();
    hw_flush();
}

void usbjtag_llp_counts(uint32_t *llp_rx_drops, uint32_t *con_rx_drops, uint32_t *con_tx_drops) {
    *llp_rx_drops = g_llp_rx.drops + g_uart_overflows;
    *con_rx_drops = g_con_rx.drops;
    *con_tx_drops = g_con_tx.drops;
}

void uart_init(uintptr_t base_addr) {
    (void)base_addr;            /* CONFIG_UART0_BASE is the chip's UART0, not this port */
}

int uart_task_start(void) { return -1; }

void uart_putc(char c) {
    if (g_llp_armed) {
        uintptr_t f = spin_lock_irqsave(&g_mux_lock);
        br_put(&g_con_tx, (uint8_t)c);
        spin_unlock_irqrestore(&g_mux_lock, f);
        return;
    }
    hw_putc(c);
}

void uart_flush(void) { if (!g_llp_armed) hw_flush(); }

bool uart_has_char(void) {
    if (g_llp_armed) {
        uintptr_t f = spin_lock_irqsave(&g_mux_lock);
        drain_rx_locked();
        bool have = g_con_rx.head != g_con_rx.tail;
        spin_unlock_irqrestore(&g_mux_lock, f);
        return have;
    }
    hw_flush();
    return (REG(USJ_EP1_CONF(g_base)) & USJ_OUT_AVAIL) != 0 || uart0_rx_avail();
}

char uart_getc(void) {
    if (g_llp_armed) {
        for (;;) {
            uint8_t c;
            uintptr_t f = spin_lock_irqsave(&g_mux_lock);
            drain_rx_locked();
            bool have = br_get(&g_con_rx, &c);
            spin_unlock_irqrestore(&g_mux_lock, f);
            if (have) return (char)c;
            sched_yield();
        }
    }
    hw_flush();
    for (;;) {
        if (REG(USJ_EP1_CONF(g_base)) & USJ_OUT_AVAIL) return (char)(REG(USJ_EP1(g_base)) & 0xffu);
        if (uart0_rx_avail()) return (char)uart0_rx_get();
        sched_yield();
    }
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
