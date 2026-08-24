/*
 * UART1 as a 9P downlink — N5, plan/phase18_networking_and_auth.md §4.
 *
 * The gateway's cable to a chess or clock board: three wires, SLIP-framed 9P,
 * and the link this project already proved over its console UART (A3a). What
 * is new here is only that it is a *second* UART instance, on pins nothing
 * else claims, with no console duties at all.
 *
 * Deliberately its own small driver rather than a generalisation of
 * drivers/uart_rp2350.c. That file is the console: it carries a channel
 * endpoint, a U-mode task, the p9share demux, ACCESSCTRL grants and a
 * heartbeat LED, all of which exist because the console is shared and
 * contended. This wire is neither. Generalising the big driver to serve one
 * unshared, single-purpose link would have made every one of those concerns
 * conditional -- more code, in the more dangerous file, to express less.
 *
 * The framing is genuinely shared: slip_feed() comes from
 * drivers/uart_net.c, because a SLIP escape state machine written twice is a
 * SLIP escape state machine that differs twice.
 */

#include "drivers/uart_net.h"
#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <string.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_UART1_BASE)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RESETS_BASE       0x40020000UL
#define RESETS_RESET_CLR  (RESETS_BASE + 0x3000 + 0x0)
#define RESETS_RESET_DONE (RESETS_BASE + 0x8)
#define RESETS_UART1_BIT  (1u << 27)

#define IO_BANK0_BASE     0x40028000UL
#define IO_BANK0_CTRL(n)  (IO_BANK0_BASE + 0x004 + (n) * 8)
#define PADS_BANK0_BASE   0x40038000UL
#define PADS_BANK0_PAD(n) (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c)
#define ACCESSCTRL_UART1         (ACCESSCTRL_BASE + 0xa4)
#define ACCESSCTRL_NSP           (1u << 1)
#define ACCESSCTRL_NSU           (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define U1        ((uintptr_t)CONFIG_UART1_BASE)
#define U1_DR     (U1 + 0x00)
#define U1_FR     (U1 + 0x18)
#define U1_IBRD   (U1 + 0x24)
#define U1_FBRD   (U1 + 0x28)
#define U1_LCR_H  (U1 + 0x2C)
#define U1_CR     (U1 + 0x30)

#define FR_RXFE   (1u << 4)   /* receive FIFO empty */
#define FR_TXFF   (1u << 5)   /* transmit FIFO full */

#ifndef CONFIG_UART1_BAUD
#define CONFIG_UART1_BAUD 115200
#endif

static bool g_up;

static bool u1_has_char(void) { return (REG(U1_FR) & FR_RXFE) == 0; }
static uint8_t u1_getc(void)  { return (uint8_t)(REG(U1_DR) & 0xFF); }

static void u1_putc(uint8_t c) {
    int guard = 1000000;
    while ((REG(U1_FR) & FR_TXFF) && --guard > 0) { }
    REG(U1_DR) = c;
}

void uart1_link_init(void) {
    REG(RESETS_RESET_CLR) = RESETS_UART1_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_UART1_BIT) && --timeout > 0);

    REG(IO_BANK0_CTRL(CONFIG_UART1_TX_GPIO)) = 2;      /* F2 = UART */
    REG(PADS_BANK0_PAD(CONFIG_UART1_TX_GPIO)) = 0x56;
    REG(IO_BANK0_CTRL(CONFIG_UART1_RX_GPIO)) = 2;
    REG(PADS_BANK0_PAD(CONFIG_UART1_RX_GPIO)) = 0x56;

    /* Phase 17b's lesson, again and in advance: the peripheral and its pins
     * are opened to Non-secure access here, from M-mode, so that a later
     * U-mode conversion does not begin with a fault. */
    REG(ACCESSCTRL_GPIO_NSMASK0) |= (1u << CONFIG_UART1_TX_GPIO) | (1u << CONFIG_UART1_RX_GPIO);
    REG(ACCESSCTRL_UART1) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_UART1)
                            | ACCESSCTRL_NSP | ACCESSCTRL_NSU;

    REG(U1_CR) = 0;

    /* Divisor computed rather than transcribed. drivers/uart_rp2350.c carries
     * `REG(UARTIBRD) = 81; REG(UARTFBRD) = 24;` with a comment naming 115200
     * -- correct, and silently wrong the day anyone wants a faster downlink.
     * The PL011 divisor is clk_peri / (16 * baud) in 6.6 fixed point, which
     * is exactly (4 * clk_peri) / baud as an integer. */
    {
        uint32_t div64 = (uint32_t)((4ull * 150000000ull) / (uint32_t)CONFIG_UART1_BAUD);
        REG(U1_IBRD) = div64 / 64u;
        REG(U1_FBRD) = div64 % 64u;
    }

    REG(U1_LCR_H) = (3u << 5) | (1u << 4);             /* 8N1, FIFOs on */
    REG(U1_CR) = (1u << 0) | (1u << 8) | (1u << 9);    /* UARTEN | TXE | RXE */

    g_up = true;
    printk("[UART1] 9P downlink on GP%d/GP%d at %d baud (SLIP)\n",
           CONFIG_UART1_TX_GPIO, CONFIG_UART1_RX_GPIO, (int)CONFIG_UART1_BAUD);
}

/* A raw wire test, because "the driver initialised" and "bytes are leaving
 * the pin" are different claims and the first was true while the second was
 * not (2026-08-24).
 *
 * Reports the peripheral's own registers first -- a UART held in reset, or
 * one whose divisor never took, reads back as zeros and no amount of
 * protocol-level debugging will say so -- then sends a plain-text burst and
 * listens for whatever comes the other way. Two boards, one command each, and
 * the direction that is broken is the one that stays silent. */
void uart1_wire_test(unsigned listen_ms) {
    if (!g_up) { printk("[UART1] not initialised\n"); return; }
    if (listen_ms == 0 || listen_ms > 60000) listen_ms = 5000;

    printk("[UART1] base 0x%lx  CR 0x%02lx  LCR_H 0x%02lx  IBRD %lu  FBRD %lu  FR 0x%02lx\n",
           (unsigned long)U1, (unsigned long)REG(U1_CR), (unsigned long)REG(U1_LCR_H),
           (unsigned long)REG(U1_IBRD), (unsigned long)REG(U1_FBRD),
           (unsigned long)REG(U1_FR));
    printk("[UART1] TX GP%d, RX GP%d, %d baud. Sending a burst, then listening %u ms.\n",
           CONFIG_UART1_TX_GPIO, CONFIG_UART1_RX_GPIO, (int)CONFIG_UART1_BAUD, listen_ms);

    static const char msg[] = "\r\nLUGALOS-UART1-WIRE-TEST\r\n";
    for (unsigned i = 0; i < sizeof(msg) - 1; i++) u1_putc((uint8_t)msg[i]);

    uint64_t end = time_get_ms() + listen_ms;
    unsigned got = 0;
    char line[64];
    unsigned n = 0;
    while (time_get_ms() < end) {
        if (!u1_has_char()) continue;
        uint8_t c = u1_getc();
        got++;
        line[n++] = (c >= 32 && c < 127) ? (char)c : '.';
        if (n == sizeof(line) - 1) {
            line[n] = 0;
            printk("[UART1] rx: %s\n", line);
            n = 0;
        }
    }
    if (n) { line[n] = 0; printk("[UART1] rx: %s\n", line); }
    printk("[UART1] %u byte(s) received in %u ms\n", got, listen_ms);
}

/* --- the p9_link ------------------------------------------------------- */

typedef struct {
    bool     escaping;
    uint8_t  frame[P9_MAX_MSIZE];
    uint32_t frame_len;
    bool     frame_ready;
} slip_ctx_t;

static slip_ctx_t g_ctx;

static int uart1_poll(p9_link_t *link) {
    (void)link;
    if (!g_up) return 0;
    if (g_ctx.frame_ready) return 1;

    while (u1_has_char()) {
        if (slip_feed(u1_getc(), g_ctx.frame, sizeof(g_ctx.frame),
                      &g_ctx.frame_len, &g_ctx.escaping)) {
            g_ctx.frame_ready = true;
            return 1;
        }
    }
    return 0;
}

static int uart1_recv(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    if (!g_ctx.frame_ready) return -1;
    uint32_t n = (g_ctx.frame_len < max_len) ? g_ctx.frame_len : max_len;
    memcpy(buf, g_ctx.frame, n);
    g_ctx.frame_ready = false;
    g_ctx.frame_len = 0;
    return (int)n;
}

static int uart1_send(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    if (!g_up || !buf) return -1;
    /* Encoded straight onto the wire, like the console link's own sender:
     * the bytes go out one at a time regardless, so a staging buffer would
     * only hold a copy of them. */
    u1_putc(SLIP_END);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == SLIP_END)      { u1_putc(SLIP_ESC); u1_putc(SLIP_ESC_END); }
        else if (c == SLIP_ESC) { u1_putc(SLIP_ESC); u1_putc(SLIP_ESC_ESC); }
        else                    { u1_putc(c); }
    }
    u1_putc(SLIP_END);
    return (int)len;
}

static p9_link_t g_uart1_link = {
    .name = "uart1",
    .poll = uart1_poll,
    .send_frame = uart1_send,
    .recv_frame = uart1_recv,
    .ctx = NULL,
    /* A cable between two boards on one desk, and the trust argument for it
     * is the same one that lets the local channel skip authentication: an
     * attacker who can reach this wire is already holding the hardware.
     * `p9auth uart1 on` is there for anyone who disagrees. */
    .auth_required = false,
};

p9_link_t *uart1_get_link(void) { return g_up ? &g_uart1_link : NULL; }

#else

void uart1_link_init(void) { }
void uart1_wire_test(unsigned ms) { (void)ms; }
p9_link_t *uart1_get_link(void) { return 0; }

#endif
