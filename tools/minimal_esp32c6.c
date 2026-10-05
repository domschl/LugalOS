/*
 * Standalone Minimal Hardware Test for ESP32-C6 (Waveshare ESP32-C6-Zero)
 *
 * 45.1, plan/phase45_esp32c6.md. The counterpart of tools/minimal_esp32p4.c,
 * for the same reason: the smallest thing that proves the board is alive, so
 * every later failure has a known-good to be compared against.
 *
 * Actions:
 *   1. Banner on the USB-Serial/JTAG console (the C6-Zero's USB-C port)
 *   2. misa / mtvec / mhartid / mstatus straight off the silicon
 *   3. The CPU clock, measured: the cycle counter against the 16 MHz system timer
 *   4. A heartbeat, forever
 *
 * ## Console
 *
 * The C6-Zero has no UART bridge: its USB-C connector goes straight to the
 * chip's USB-Serial/JTAG peripheral (VID:PID 303a:1001), which is also what
 * the ROM prints on and what esptool loads over. By the time we run it is
 * enumerated and attached, so writing to its EP1 FIFO is sufficient.
 *
 * **Every wait here is bounded.** With no host reading the port the FIFO never
 * drains; an unbounded wait would hang the program silently, and "our code
 * never ran" and "nobody is listening" would be indistinguishable from the far
 * end of a cable -- the same trap tools/minimal_esp32p4.c documents for its
 * RX drain. On timeout a byte is dropped, and the FIFO is flushed once the host
 * returns.
 *
 * Register offsets: IDF components/soc/esp32c6/register/soc/
 * {usb_serial_jtag_reg.h, systimer_reg.h}, read 2026-10-05. Not yet checked
 * against the TRM (that is 45.4's job, where more of them matter).
 *
 * ## Where this runs
 *
 * HP SRAM, via `esptool load-ram`; flash is not written. A reset restores
 * whatever is in flash. tools/c6run.py does the loading.
 */

#include <stdint.h>
#include <stdbool.h>

#define REG(addr) (*(volatile uint32_t *)(addr))

#define USJ_BASE            0x6000F000UL
#define USJ_EP1_REG         (USJ_BASE + 0x0)      /* write: one byte into the IN FIFO */
#define USJ_EP1_CONF_REG    (USJ_BASE + 0x4)
#define USJ_WR_DONE         (1u << 0)             /* write 1: hand the FIFO to the host */
#define USJ_IN_EP_DATA_FREE (1u << 1)             /* FIFO has room for a byte */

#define SYSTIMER_BASE       0x6000A000UL
#define SYSTIMER_CONF_REG   (SYSTIMER_BASE + 0x00)
#define SYSTIMER_UNIT0_OP   (SYSTIMER_BASE + 0x04)
#define SYSTIMER_UNIT0_LO   (SYSTIMER_BASE + 0x44)
#define SYSTIMER_CLK_EN     (1u << 31)
#define SYSTIMER_UNIT0_WORK_EN (1u << 30)
#define SYSTIMER_UNIT0_UPDATE  (1u << 30)
#define SYSTIMER_UNIT0_VALID   (1u << 29)

/* Longest we wait for one byte of FIFO space before dropping it. */
#define TX_SPIN_LIMIT       200000u

/* The FIFO is 64 bytes; flush at that mark so a long line is not one byte of
 * silent loss after the 64th. */
#define FIFO_BYTES          64u

static unsigned tx_pending;
static bool     host_gone;      /* last wait timed out: do not wait again for a while */

static void usj_flush(void) {
    if (tx_pending) {
        REG(USJ_EP1_CONF_REG) = USJ_WR_DONE;
        tx_pending = 0;
    }
}

static void usj_putc(char c) {
    if (host_gone) {
        /* Retry cheaply: one look, no spin. The host may have come back. */
        if (!(REG(USJ_EP1_CONF_REG) & USJ_IN_EP_DATA_FREE)) return;
        host_gone = false;
    } else {
        unsigned n = 0;
        while (!(REG(USJ_EP1_CONF_REG) & USJ_IN_EP_DATA_FREE)) {
            if (++n >= TX_SPIN_LIMIT) { host_gone = true; tx_pending = 0; return; }
        }
    }
    REG(USJ_EP1_REG) = (uint32_t)(uint8_t)c;
    if (++tx_pending >= FIFO_BYTES || c == '\n') usj_flush();
}

static void puts_(const char *s) {
    while (*s) {
        if (*s == '\n') usj_putc('\r');
        usj_putc(*s++);
    }
    usj_flush();
}

static void puthex(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    usj_putc('0'); usj_putc('x');
    for (int shift = 28; shift >= 0; shift -= 4)
        usj_putc(digits[(v >> shift) & 0xFu]);
}

static void putdec(uint32_t v) {
    char b[11]; int i = 0;
    do { b[i++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (i) usj_putc(b[--i]);
}

#define READ_CSR(name) ({ uint32_t __v; __asm__ volatile ("csrr %0, " name : "=r"(__v)); __v; })

/* The C6's core has no standard mcycle/cycle CSR: `csrr mcycle` is an illegal
 * instruction here (found 2026-10-05 -- the first load trapped at it and the
 * ROM dumped PC/RA/SP). Espressif's own performance counter is at CSRs
 * 0x7e0 (PCER: which events count, bit 0 = cycles), 0x7e1 (PCMR: bit 0 =
 * counting enabled) and 0x7e2 (PCCR: the count); IDF's
 * components/riscv/include/riscv/rv_utils.h names them. */
#define CSR_PCER 0x7e0
#define CSR_PCMR 0x7e1
#define CSR_PCCR 0x7e2
static void cycles_start(void) {
    __asm__ volatile ("csrw 0x7e0, %0" :: "r"(1u));
    __asm__ volatile ("csrw 0x7e1, %0" :: "r"(1u));
}
static uint32_t cycles_now(void) { return READ_CSR("0x7e2"); }

static uint32_t systimer_now(void) {
    REG(SYSTIMER_UNIT0_OP) = SYSTIMER_UNIT0_UPDATE;
    for (unsigned n = 0; n < 1000u && !(REG(SYSTIMER_UNIT0_OP) & SYSTIMER_UNIT0_VALID); n++) { }
    return REG(SYSTIMER_UNIT0_LO);
}

static void report(void) {
    puts_("\n[C6_MINIMAL] ESP32-C6 alive, running from HP SRAM.\n");

    puts_("  misa    = "); puthex(READ_CSR("misa"));    puts_("\n");
    puts_("  mtvec   = "); puthex(READ_CSR("mtvec"));   puts_("\n");
    puts_("  mhartid = "); puthex(READ_CSR("mhartid")); puts_("\n");
    puts_("  mstatus = "); puthex(READ_CSR("mstatus")); puts_("\n");

    /* CPU clock: count the cycle counter across a fixed 100 ms of systimer
     * time, assuming the documented 16 MHz tick (XTAL/2.5). If the tick
     * assumption is wrong this reports a wrong MHz, which is a finding about
     * the tick and gets recorded as one. Integer division truncates: 159 means
     * "just under 160". */
    uint32_t t0 = systimer_now(), c0 = cycles_now();
    while ((uint32_t)(systimer_now() - t0) < 1600000u) { }
    uint32_t cycles = cycles_now() - c0;
    puts_("  cpu     = "); putdec(cycles / 100000u); puts_(" MHz (cycle counter over 100 ms of systimer @16 MHz)\n");
}

void minimal_main(void) {
    /* The ROM leaves the system timer's clock on in download mode, but
     * "leaves" is exactly the kind of inheritance E2 on the P4 refused; turn
     * it on explicitly. */
    REG(SYSTIMER_CONF_REG) |= SYSTIMER_CLK_EN | SYSTIMER_UNIT0_WORK_EN;
    cycles_start();

    /* This program is delivered by `esptool load-ram`, which owns the port
     * until the instant it jumps to us, so the first report is usually lost.
     * Repeat it: every 5th beat, so a monitor attached a few seconds later
     * still sees the CSRs. (Same device as tools/minimal_esp32p4.c's
     * unconditional heartbeat.) */
    uint32_t beats = 0;
    report();
    while (1) {
        uint32_t t = systimer_now();
        while ((uint32_t)(systimer_now() - t) < 16000000u) { }   /* ~1 s */
        puts_("[C6_MINIMAL] alive, beat "); puthex(++beats); puts_("\n");
        if (beats % 5u == 0) report();
    }
}
