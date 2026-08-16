/*
 * LugalOS Hardware Driver: PL011 UART0 + Dual Onboard/External LEDs for RP2350 (Pico 2)
 * Adopted from working bare-metal RP2350 driver (rp2350_cld).
 *
 * Hardware Map (K2, plan/phase7_kernel_config.md -- the numbers themselves
 * live in cmake/board-rp2350.cmake now, this is documentation, not the
 * source of truth):
 *   GP0  : UART0 TX (Function 2)
 *   GP1  : UART0 RX (Function 2)
 *   GP25 : Onboard LED (Function 5 - SIO)
 *   GP16 : External LED (Function 5 - SIO, active-high pulse)
 */

#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "fs/p9_link.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/devirq.h"
#include "kernel/irq.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "arch/trap.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define CLOCKS_BASE             0x40010000UL
#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_DONE       (RESETS_BASE + 0x08)
#define RESETS_ATOMIC_CLEAR     (RESETS_BASE + 0x3000)

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)

#define UART0_BASE              ((uintptr_t)CONFIG_UART0_BASE)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define LED_MASK  ((1u << CONFIG_LED_ONBOARD_GPIO) | (1u << CONFIG_LED_EXT_GPIO))

#include "drivers/usb_cdc.h"

static void delay_cycles(volatile uint32_t n) {
    while (n--) {
        if ((n & 0xFFFF) == 0) {
            usb_cdc_task();
        }
        __asm__ volatile("nop");
    }
}

static inline uint32_t read_mcycle(void) {
    uint32_t c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

void led_on(void) {
    REG(SIO_GPIO_OUT_SET) = LED_MASK;
}

void led_off(void) {
    REG(SIO_GPIO_OUT_CLR) = LED_MASK;
}

void led_blink_phase(int count) {
    for (int i = 0; i < count; i++) {
        led_on();
        delay_cycles(4000000);
        led_off();
        delay_cycles(4000000);
    }
    delay_cycles(8000000);
}

/* Heartbeat on GP16: 0.5 Hz, brief flash.
 *
 * M4.5, plan/phase12_microkernel_migration.md, Part B: this used to be a
 * two-state machine stepped as a side effect of uart_has_char()/
 * uart_getc()'s console-polling loop (a function called gp16_alive_tick(),
 * folded into that poll specifically so a blocking spin here could not
 * starve USB CDC servicing -- see the git history of this file for that
 * fix). That made the heartbeat a proxy for "is anything polling the
 * console" rather than "is the scheduler giving every READY task its
 * turn" -- a program that reads no input for an extended stretch (uspin,
 * for one) would silently stop the heartbeat too, which looks exactly
 * like the scheduler problem this LED exists to help rule out, for a
 * reason that has nothing to do with scheduling.
 *
 * Now a real, independently-scheduled task instead: it blinks at its own
 * rate regardless of what any other task is doing, which is what makes it
 * an actual live, on-the-board check that the scheduler is working --
 * exactly the M4.5 Part A question (does every READY task get a turn),
 * but continuously, visually, and without a serial connection. TASK_PRIO_
 * NORMAL, not TASK_PRIO_INTERRUPT: the point is to reflect ordinary
 * scheduling health, including whether NORMAL-tier tasks get their turn
 * under load, not to guarantee a smooth blink regardless of it -- an
 * INTERRUPT-tier heartbeat would always look healthy and prove nothing.
 * Sleeps by yielding, not spinning, between checks -- a background
 * indicator has no business burning CPU that another READY task could use
 * while it waits out 2 seconds of mostly being off. */
#define HEARTBEAT_PERIOD_MS 2000u  /* 0.5 Hz */
#define HEARTBEAT_ON_MS       40u  /* brief flash, not a duty cycle */

static void heartbeat_sleep_until(uint64_t target_ms) {
    while (time_get_ms() < target_ms) sched_yield();
}

static void heartbeat_task_body(void *arg) {
    (void)arg;
    for (;;) {
        REG(SIO_GPIO_OUT_SET) = (1u << CONFIG_LED_EXT_GPIO);
        heartbeat_sleep_until(time_get_ms() + HEARTBEAT_ON_MS);
        REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_LED_EXT_GPIO);
        heartbeat_sleep_until(time_get_ms() + (HEARTBEAT_PERIOD_MS - HEARTBEAT_ON_MS));
    }
}

/* Called from kernel/main.c, after sched_init() -- unlike uart_init(),
 * which brings the hardware (including this LED's GPIO mux/OE bits) up
 * long before a task table exists. Not fatal if it fails: the board just
 * loses its visual heartbeat, the same as if this milestone had not
 * landed yet. */
int heartbeat_task_start(void) {
    int pid = task_create_sized("heartbeat", heartbeat_task_body, NULL, 1);
    if (pid < 0) return -1;
    task_set_priority(pid, TASK_PRIO_NORMAL);
    return pid;
}

// Raw physical-UART byte access (defined below, after uart_putc()); handed
// to drivers/uart_net.c's A3b demux here in uart_init() so it needs a
// forward declaration.
static bool hw_uart_has_char(void);
static uint8_t hw_uart_getc(void);

/* M2, revised M2.5 (plan/phase12_microkernel_migration.md): PL011 UART0's
 * own IRQ line (RP2350 datasheet §2.1 IRQ table, confirmed against the Pico
 * SDK's hardware/regs/intctrl.h). Wired up in M2, unwired the same
 * milestone after hardware testing showed it corrupting console output, and
 * back now that kernel/printk.c's printk_lock()/printk_unlock() make a
 * whole message atomic across a block rather than just across a spin -- see
 * that file's comment for the full story.
 *
 * TX only, still deliberately: uart_getc()/uart_has_char() below poll TWO
 * independent sources every iteration -- the physical UART and USB CDC
 * (itself software-polled, not interrupt-driven) -- and this tree has no
 * "block with timeout" or "wait on any of N sources" primitive yet. That
 * reasoning is unrelated to the printk fix above and unchanged by it: RX
 * stays exactly as it was until a future milestone gives USB CDC its own
 * interrupt path or this tree gains a real multi-wait primitive. (The
 * heartbeat LED used to be a third thing stepped from this same poll --
 * M4.5, plan/phase12_microkernel_migration.md, Part B gave it its own
 * task instead, so it is no longer part of this reasoning.) */
#define UART0_IRQ 33u

/* At most one TX waiter: every caller now arrives through printk_lock()
 * (kernel/printk.c) already serialized to a single writer, so a second,
 * concurrent blocker here should not happen -- kept as a defensive
 * fallback below, not a load-bearing path, the same shape as
 * drivers/uart_16550.c's equivalent. */
static volatile int g_tx_waiter = -1;

#define UART0_FR    (UART0_BASE + 0x18)
#define UART0_IMSC  (UART0_BASE + 0x38)
#define UART0_FR_TXFF (1u << 5)
#define UART0_IMSC_TXIM (1u << 5)

static void uart_isr(void *ctx) {
    (void)ctx;
    if (!(REG(UART0_FR) & UART0_FR_TXFF) && g_tx_waiter >= 0) {
        REG(UART0_IMSC) &= ~UART0_IMSC_TXIM;
        int pid = g_tx_waiter;
        g_tx_waiter = -1;
        task_unblock(pid);
    }
}

void uart_init(uintptr_t base_addr) {
    (void)base_addr;

    /* 1. Explicitly enable clk_peri and attach it to clk_sys (150 MHz) */
    REG(CLOCKS_BASE + 0x48) = (1u << 11);

    /* 2. Unreset IO_BANK0 (bit 6), PADS_BANK0 (bit 9), UART0 (bit 26) */
    uint32_t unreset_mask = (1u << 6) | (1u << 9) | (1u << 26);
    REG(RESETS_ATOMIC_CLEAR) = unreset_mask;
    
    /* Wait for RESET_DONE status register @ 0x40020008 */
    while ((REG(RESETS_RESET_DONE) & unreset_mask) != unreset_mask);

    /* 3. Configure LEDs (GP25 onboard + GP16 external) for SIO (Function 5) */
    REG(IO_BANK0_CTRL(CONFIG_LED_ONBOARD_GPIO)) = 5;
    REG(IO_BANK0_CTRL(CONFIG_LED_EXT_GPIO)) = 5;
    REG(PADS_BANK0_PAD(CONFIG_LED_ONBOARD_GPIO)) = 0x56;
    REG(PADS_BANK0_PAD(CONFIG_LED_EXT_GPIO)) = 0x56;
    REG(SIO_GPIO_OE_SET) = LED_MASK;

    /* Default GP16 to LOW (OFF in active-high configuration) */
    REG(SIO_GPIO_OUT_CLR) = (1u << CONFIG_LED_EXT_GPIO);

    /* 4. Mux GP0 to UART0 TX (Function 2), GP1 to UART0 RX (Function 2) */
    REG(IO_BANK0_CTRL(CONFIG_UART0_TX_GPIO)) = 2;
    REG(PADS_BANK0_PAD(CONFIG_UART0_TX_GPIO)) = 0x56;

    REG(IO_BANK0_CTRL(CONFIG_UART0_RX_GPIO)) = 2;
    REG(PADS_BANK0_PAD(CONFIG_UART0_RX_GPIO)) = 0x56;

    /* 5. Disable UART before programming baud rate and line control */
    REG(UART0_BASE + 0x30) = 0;

    /* 6. Configure Baud Rate for 150MHz clk_peri -> 115200 baud */
    REG(UART0_BASE + 0x24) = 81;  // UARTIBRD
    REG(UART0_BASE + 0x28) = 24;  // UARTFBRD

    /* 7. 8 bits, no parity, 1 stop bit, enable FIFOs */
    REG(UART0_BASE + 0x2C) = (3u << 5) | (1u << 4); // UARTLCR_H

    /* 8. Enable UART0, Transmit (TXE) & Receive (RXE) */
    REG(UART0_BASE + 0x30) = (1u << 0) | (1u << 8) | (1u << 9); // UARTCR

    /* Signal phase: Hardware & LEDs initialized! */
    led_blink_phase(2);

    uart_demux_init(hw_uart_has_char, hw_uart_getc);

    /* Handler before enable (arch/trap.h): so that if the IRQ somehow fires
     * before this returns, there is a handler to find it rather than an
     * unmasked source with nothing behind it. */
    devirq_attach(UART0_IRQ, uart_isr, NULL);
    arch_irq_enable(UART0_IRQ);

    uart_puts("\r\n[RP2350 Hardware UART0 Online] LugalOS Microkernel Starting...\r\n");
}

/* M2.5, plan/phase12_microkernel_migration.md: safe now that every caller
 * arrives through printk_lock() (kernel/printk.c) or is otherwise the sole
 * writer -- see that file's comment for the full story of what M2's first
 * attempt at this got wrong (real hardware caught `[Sche5d]` where `[Sched]`
 * should have been -- interleaved output, not a PMP failure, despite the
 * symptom initially reading like one). Same continuous-irq_save()
 * requirement as drivers/uart_16550.c's uart_putc(): nothing between the
 * fast-path miss and task_block() can restore interrupts early, or a TX
 * interrupt landing in the gap finds this task still RUNNING (not yet
 * BLOCKED), task_unblock() no-ops, and the ISR has already disabled TXIM
 * having "served" a wakeup nobody was asleep for -- a silent, permanent
 * lost wakeup. */
static void uart_hw_putc(char c) {
    if (!(REG(UART0_FR) & UART0_FR_TXFF)) {
        REG(UART0_BASE + 0x00) = (uint8_t)c;
        return;
    }
    uintptr_t flags = irq_save();
    if (REG(UART0_FR) & UART0_FR_TXFF) {
        if (g_tx_waiter < 0) {
            g_tx_waiter = sched_current_pid();
            REG(UART0_IMSC) |= UART0_IMSC_TXIM;
            task_block();
        } else {
            irq_restore(flags);
            while (REG(UART0_FR) & UART0_FR_TXFF) sched_yield();
            flags = irq_save();
        }
    }
    irq_restore(flags);
    REG(UART0_BASE + 0x00) = (uint8_t)c;
}

// Raw physical-UART byte access (forward-declared above uart_init(), which
// hands these to drivers/uart_net.c's A3b demux) -- kept separate from
// uart_has_char()/uart_getc() below so the demux (once a user opts in via
// `p9share`) can pull straight from the PL011 registers without going
// through the console-ring indirection it is itself responsible for
// providing.
static bool hw_uart_has_char(void) {
    return (REG(UART0_BASE + 0x18) & (1u << 4)) == 0; // physical UART RX FIFO not empty
}

static uint8_t hw_uart_getc(void) {
    return (uint8_t)(REG(UART0_BASE + 0x00) & 0xFF);
}

/* --- M4.5, plan/phase12_microkernel_migration.md, Part B: the uart task ---
 *
 * Finishes what M4 deferred (see this file's own note, since removed, that
 * tracked this separately): the same batched wire protocol
 * drivers/uart_16550.c's "uart" task uses, adapted for two things that file
 * does not have -- a USB CDC mirror on every write, and RX polled rather
 * than ISR-driven (this board's own long-standing reason, unchanged by this
 * conversion: uart_getc()/has_char() must watch two independent sources,
 * the physical UART and software-polled USB CDC, and this tree has no
 * "wait on any of N sources" primitive yet).
 *
 *   'H'      -> has-char query (physical UART OR USB CDC).  resp: 1 byte, 0 or 1.
 *   'R'      -> read (blocks internally, polling both sources, pumping
 *               usb_cdc_task() each iteration -- exactly what uart_getc()'s
 *               own wait loop used to do on the caller's stack, moved here).
 *               resp: 1 byte, the char read; physical UART preferred over
 *               USB CDC when both have one, same order uart_getc() always
 *               used.
 *   'W', ... -> write the req_len-1 bytes following the opcode to BOTH the
 *               physical UART and USB CDC (uart_putc()'s original mirror,
 *               preserved exactly, just batched -- see uart_putc()/
 *               uart_flush() below for the batching half, and
 *               uart_16550.c's own uart task comment for why a whole batch,
 *               not one byte, is what actually fixes IPC volume). resp: empty.
 *
 * Deliberately NOT reachable when the A3b demux is enabled (`p9share`): the
 * facade functions below fall straight to direct hardware access in that
 * case, same as uart_16550.c, since the demux needs unmediated register
 * access to split 9P frame bytes from console bytes and is not itself
 * IPC-aware. */
#define UART_REQ_HASCHAR ((uint8_t)'H')
#define UART_REQ_READ    ((uint8_t)'R')
#define UART_REQ_WRITE   ((uint8_t)'W')

/* Must match (or exceed) uart_putc()'s UART_TX_BATCH_CAP, plus the opcode
 * byte -- the largest single 'W' request the endpoint can accept. */
#define UART_TX_BATCH_CAP 256
#define UART_REQ_CAP (UART_TX_BATCH_CAP + 1)

static uint8_t         g_uart_req[UART_REQ_CAP];
static uint8_t         g_uart_resp[1];
static chan_endpoint_t *g_uart_ep;
static int              g_uart_task_pid = -1;

/* M4.5 verify: counts UART_REQ_WRITE calls actually served -- one per
 * uart_flush() that reached the task, not per character -- see
 * drivers/uart_16550.c's g_uart_write_calls comment for the reasoning. */
static uint32_t g_uart_write_calls;

uint32_t uart_write_call_count(void) { return g_uart_write_calls; }

/* M4.5, found on real hardware: true only while the task's WRITE handler is
 * actually pushing bytes to the physical UART/USB CDC below, false at every
 * other time (including the whole duration of a pending READ, which can run
 * for as long as a human takes to press a key). uart_flush()'s retry loop
 * uses this to tell "the endpoint is busy with a bounded WRITE -- wait it
 * out, never race it" apart from "busy with an unbounded READ -- falling
 * back to direct access right away is the correct, safe behavior" (the READ
 * side never touches the TX-side registers/ring this WRITE handler does, so
 * a concurrent direct-access WRITE cannot collide with it).
 *
 * Without this distinction, the original bounded-retry-then-fallback shape
 * (drivers/uart_16550.c had this first) can let a second WRITE that
 * outlasts its retry budget fall back to direct hardware access *while the
 * task is still transmitting the first one* -- safe on QEMU's near-instant
 * emulated UART (retries essentially never exhaust), reliably reproducible
 * on real 115200-baud hardware, where a full 256-byte batch takes tens of
 * milliseconds to transmit. Observed directly while bringing up this task:
 * a kernel printk() line ("[Sched] Created task #8 'uprog'...") and a
 * just-spawned user program's own stdout ("USPIN_START"), both landing in
 * the batch/hardware access at once, interleaved into byte-level garbage
 * ("[SUSPIN_STARTched] Created task #8 'uprog'..."). */
static volatile bool g_uart_write_in_flight;

static bool uart_task_alive(void) {
    if (g_uart_task_pid < 0) return false;
    int st = sched_task_state(g_uart_task_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

/* This task, and only this task, may call uart_hw_putc()/hw_uart_has_char()/
 * hw_uart_getc() while alive -- everything else reaches the hardware
 * through chan_call("uart", ...) via the facade functions below. Must never
 * call uart_putc()/uart_getc()/uart_has_char() itself (would chan_call()
 * into its own endpoint -- refused by the busy flag, but should simply
 * never happen), and must never call printk()/cprintf() (or anything that
 * takes printk_lock()) from within this loop -- see drivers/uart_16550.c's
 * uart_task_body() for the fuller reasoning on both. */
static void uart_task_body(void *arg) {
    (void)arg;
    while (!g_uart_ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(g_uart_ep);
        if (req_len == 0) { chan_serve_reply(g_uart_ep, 0); continue; }
        switch (g_uart_req[0]) {
            case UART_REQ_HASCHAR:
                g_uart_resp[0] = (hw_uart_has_char() || usb_cdc_has_char()) ? 1 : 0;
                chan_serve_reply(g_uart_ep, 1);
                break;
            case UART_REQ_READ:
                /* Found on real hardware, not predicted: this loop's own
                 * wait can run for as long as a human takes to press a key
                 * -- unlike drivers/uart_16550.c's RX (a true ISR-driven
                 * task_block(), consuming zero scheduler cycles while
                 * idle), this board has no RX interrupt to block on (see
                 * uart_init()'s own comment on why), so waiting here means
                 * actually looping, at whatever tier this task runs at.
                 * Sitting at TASK_PRIO_INTERRUPT for the whole wait
                 * strangled the "p9srv" task (NORMAL tier, services the
                 * ACM1 background 9P link) badly enough to make it
                 * unresponsive for the wait's entire duration -- confirmed
                 * by flashing both ways and watching ACM1 stop answering
                 * Tversion probes only with this loop at INTERRUPT tier.
                 * Dropped to NORMAL just for this loop, restored after, so
                 * WRITE/HASCHAR below (genuinely near-instant) keep the
                 * fast-handoff tier uart_16550.c's own task uses, without
                 * this open-ended wait ever holding it. */
                task_set_priority(sched_current_pid(), TASK_PRIO_NORMAL);
                for (;;) {
                    usb_cdc_task();
                    if (hw_uart_has_char()) { g_uart_resp[0] = hw_uart_getc(); break; }
                    if (usb_cdc_has_char()) { g_uart_resp[0] = usb_cdc_getc(); break; }
                    sched_yield();
                }
                task_set_priority(sched_current_pid(), TASK_PRIO_INTERRUPT);
                chan_serve_reply(g_uart_ep, 1);
                break;
            case UART_REQ_WRITE:
                g_uart_write_calls++;
                g_uart_write_in_flight = true;
                for (uint32_t i = 1; i < req_len; i++) {
                    uart_hw_putc((char)g_uart_req[i]);
                    usb_cdc_putc((char)g_uart_req[i]);
                }
                g_uart_write_in_flight = false;
                chan_serve_reply(g_uart_ep, 0);
                break;
            default:
                chan_serve_reply(g_uart_ep, 0);
                break;
        }
    }
}

/* Called from kernel/main.c, after sched_init() -- unlike uart_init(),
 * which runs long before that and only brings the hardware itself up. Not
 * fatal if it fails: every facade function below falls back to direct
 * hardware access whenever uart_task_alive() is false. */
int uart_task_start(void) {
    int pid = task_create_sized("uart", uart_task_body, NULL, 1);
    if (pid < 0) {
        printk("[UART] Could not start the uart task; console stays on direct hardware access.\n");
        return -1;
    }
    /* TASK_PRIO_INTERRUPT: matches drivers/uart_16550.c's own uart task --
     * a caller that chan_call()s "uart" should not sit behind an arbitrary
     * NORMAL-tier queue for what is meant to be a near-instant handoff. */
    task_set_priority(pid, TASK_PRIO_INTERRUPT);
    if (chan_register_task("uart", pid, g_uart_req, sizeof(g_uart_req),
                           g_uart_resp, sizeof(g_uart_resp)) != 0) {
        printk("[UART] Could not register the uart channel endpoint; falling back to direct hardware access.\n");
        return -1;
    }
    g_uart_ep = chan_lookup("uart");
    g_uart_task_pid = pid;
    printk("[UART] Driver running as task #%d, reachable via chan_call(\"uart\", ...)\n", pid);
    return pid;
}

static int uart_call_with_retry(const uint8_t *req, uint32_t req_len,
                                uint8_t *resp, uint32_t resp_max) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(g_uart_ep, req, req_len, resp, resp_max);
        if (n >= 0) return n;
        sched_yield();
    }
    return -1;
}

/* M4.5: batches characters and sends one chan_call() per chunk (or per
 * uart_flush()) instead of one per character -- same reasoning and same
 * shape as drivers/uart_16550.c's own uart_putc()/uart_flush(), including
 * the while (not if) loop below (see that file's comment for why: without
 * it, a task that finds the batch full, releases the lock to flush, and
 * gets preempted before re-appending would let a second task refill the
 * just-flushed batch in the gap). */
static char     g_tx_batch[UART_TX_BATCH_CAP];
static uint32_t g_tx_batch_len;

void uart_flush(void) {
    char local[UART_TX_BATCH_CAP];
    uintptr_t flags = irq_save();
    uint32_t len = g_tx_batch_len;
    if (len > 0) {
        memcpy(local, g_tx_batch, len);
        g_tx_batch_len = 0;
    }
    irq_restore(flags);
    if (len == 0) return;

    if (uart_demux_is_enabled() || !uart_task_alive()) {
        for (uint32_t i = 0; i < len; i++) {
            uart_hw_putc(local[i]);
            usb_cdc_putc(local[i]);
        }
        return;
    }
    uint8_t req[1 + UART_TX_BATCH_CAP];
    req[0] = UART_REQ_WRITE;
    memcpy(&req[1], local, len);
    uint8_t resp[1];
    /* Not uart_call_with_retry()'s plain bounded retry: see
     * g_uart_write_in_flight's own comment for why a WRITE specifically
     * must never give up and fall back to direct access while another
     * WRITE is actually in flight (a real race on real hardware, not a
     * theoretical one), while still falling back promptly -- not waiting
     * out a human's keystroke -- when the endpoint is merely busy with a
     * pending READ instead. */
    for (;;) {
        int n = chan_call(g_uart_ep, req, 1 + len, resp, sizeof(resp));
        if (n >= 0) return;
        if (!g_uart_write_in_flight) break;
        sched_yield();
    }
    for (uint32_t i = 0; i < len; i++) {
        uart_hw_putc(local[i]);
        usb_cdc_putc(local[i]);
    }
}

void uart_putc(char c) {
    uintptr_t flags = irq_save();
    while (g_tx_batch_len >= UART_TX_BATCH_CAP) {
        irq_restore(flags);
        uart_flush();
        flags = irq_save();
    }
    g_tx_batch[g_tx_batch_len++] = c;
    irq_restore(flags);
}

void uart_debug_putc(char c) {
    uart_hw_putc(c); // kernel debug log: physical UART only, no USB mirror, no batching
}

// The A3b demux only ever applies to the physical UART0 wire -- USB CDC
// ACM0 is a channel of its own with no shared-wire ambiguity, so it keeps
// being checked directly and unconditionally, same as before this
// conversion. Flushed first: an outstanding write (a prompt, most often)
// should be visible before anything checks for or waits on the reply it
// usually precedes -- same reasoning as drivers/uart_16550.c's equivalent.
bool uart_has_char(void) {
    uart_flush();
    usb_cdc_task();
    if (uart_demux_is_enabled()) {
        if (uart_demux_console_has_char()) return true;
        return usb_cdc_has_char();
    }
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_HASCHAR };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1) return resp[0] != 0;
    }
    if (hw_uart_has_char()) return true;
    return usb_cdc_has_char();
}

char uart_getc(void) {
    uart_flush();
    if (uart_demux_is_enabled()) {
        while (!uart_demux_console_has_char()) {
            usb_cdc_task();
            sched_yield();
        }
        return uart_demux_console_getc();
    }
    if (uart_task_alive()) {
        uint8_t req[1] = { UART_REQ_READ };
        uint8_t resp[1];
        if (uart_call_with_retry(req, 1, resp, 1) == 1) return (char)resp[0];
    }
    for (;;) {
        usb_cdc_task();
        if (hw_uart_has_char()) return (char)hw_uart_getc();
        if (usb_cdc_has_char()) return usb_cdc_getc();
        sched_yield();
    }
}

void uart_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_debug_puts(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') uart_debug_putc('\r');
        uart_debug_putc(*s++);
    }
}
