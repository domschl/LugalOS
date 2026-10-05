/*
 * USB-Serial/JTAG console, system timer and cycle counter for the standalone
 * ESP32-C6 tools (tools/minimal_esp32c6.c, tools/umode_probe_esp32c6.c).
 * 45.1/45.3, plan/phase45_esp32c6.md. Header-only and static on purpose: these
 * are single-file programs delivered by `esptool load-ram`, not kernel code.
 *
 * See minimal_esp32c6.c for why every wait here is bounded.
 */
#ifndef C6_STANDALONE_H
#define C6_STANDALONE_H

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
__attribute__((unused)) static uint32_t cycles_now(void) { return READ_CSR("0x7e2"); }

static uint32_t systimer_now(void) {
    REG(SYSTIMER_UNIT0_OP) = SYSTIMER_UNIT0_UPDATE;
    for (unsigned n = 0; n < 1000u && !(REG(SYSTIMER_UNIT0_OP) & SYSTIMER_UNIT0_VALID); n++) { }
    return REG(SYSTIMER_UNIT0_LO);
}


#endif
