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

#define SIO_BASE          0xD0000000UL
#define SIO_GPIO_IN       (SIO_BASE + 0x004)
#define SIO_GPIO_OUT_SET  (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR  (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET   (SIO_BASE + 0x038)
#define SIO_GPIO_OE_CLR   (SIO_BASE + 0x040)

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

/* Set a pin's function, and CHECK that it took.
 *
 * Written because it did not. At init the write of FUNCSEL=2 read straight
 * back as 0x1f (NULL, the reset value) while the identical write from a shell
 * command seconds later worked -- so the pin mux was silently refusing writes
 * during early boot, and the driver then spent the rest of its life
 * transmitting into a disconnected pad. Every symptom above that point looked
 * like a cable fault: a UART whose registers are all correct, whose internal
 * loopback passes, and whose bytes go nowhere.
 *
 * A write, a read-back and a retry is cheap; believing a write is not. If it
 * never takes, this says so rather than leaving the caller to infer it from
 * silence. */
static bool set_funcsel(unsigned gpio, uint32_t fn) {
    for (int attempt = 0; attempt < 10; attempt++) {
        REG(IO_BANK0_CTRL(gpio)) = fn;
        if ((REG(IO_BANK0_CTRL(gpio)) & 0x1Fu) == fn) {
            if (attempt) printk("[UART1] GP%u mux took on attempt %d\n", gpio, attempt + 1);
            return true;
        }
        time_delay_us(1000);
    }
    printk("[UART1] GP%u mux REFUSED FUNCSEL %u (reads 0x%08lx) -- the pin is not "
           "connected to the UART\n", gpio, (unsigned)fn,
           (unsigned long)REG(IO_BANK0_CTRL(gpio)));
    return false;
}

void uart1_link_init(void) {
    REG(RESETS_RESET_CLR) = RESETS_UART1_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_UART1_BIT) && --timeout > 0);

    set_funcsel(CONFIG_UART1_TX_GPIO, 2);              /* F2 = UART1 TX */
    REG(PADS_BANK0_PAD(CONFIG_UART1_TX_GPIO)) = 0x56;  /* schmitt, 4mA, IE */

    /* RX gets a pull-UP (0x5A), not the pull-down drivers/uart_rp2350.c uses
     * for its own console RX.
     *
     * A UART line idles high, so an unconnected RX with a pull-down sits at
     * the start-bit level forever and the receiver reads a stream of framing
     * errors -- garbage bytes out of a wire that is not there. With a
     * pull-up, a disconnected RX produces *nothing*, which is both correct
     * and far easier to diagnose: "0 bytes" then means "no cable" rather than
     * "no cable, or a cable, who knows". Found while chasing a downlink that
     * delivered exactly one unprintable byte per burst (2026-08-24). */
    set_funcsel(CONFIG_UART1_RX_GPIO, 2);              /* F2 = UART1 RX */
    REG(PADS_BANK0_PAD(CONFIG_UART1_RX_GPIO)) = 0x5A;  /* schmitt, PUE, 4mA, IE */

    /* **No GPIO_NSMASK grant here, and that is the fix for the bug that made
     * this downlink look like a cable fault for an afternoon.**
     *
     * Marking a pin non-secure in ACCESSCTRL hands it to the non-secure side
     * -- and takes it away from the secure one. This driver runs entirely in
     * M-mode: it has no U-mode task, unlike the console UART, the clock, the
     * ST7735 or the W5500. Granting the pins away therefore bought nothing
     * and cost everything: FUNCSEL was written and verified correct, then the
     * NSMASK write immediately below it made GP8/GP9 read back as 0x1f (NULL)
     * from M-mode and stop accepting writes. The UART then transmitted
     * perfectly into a pad that was no longer connected to it -- registers
     * right, internal loopback passing, not one byte on the wire.
     *
     * The peripheral grant is kept: ACCESSCTRL_UART1 is about UART1's own
     * registers, is harmless from M-mode (the SP/SU bits are preserved by the
     * read-modify-write), and is what a future U-mode conversion will need.
     * Only the pin grant was wrong, and it should be added back at the same
     * time as the U-mode task, not before. */
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
    /* Read back what was just written. A mux that does not stick at boot and
     * does stick later is a very different bug from one that never sticks. */
    printk("[UART1] init readback: GP%d CTRL 0x%08lx, GP%d CTRL 0x%08lx (2 = UART)\n",
           CONFIG_UART1_TX_GPIO, (unsigned long)REG(IO_BANK0_CTRL(CONFIG_UART1_TX_GPIO)),
           CONFIG_UART1_RX_GPIO, (unsigned long)REG(IO_BANK0_CTRL(CONFIG_UART1_RX_GPIO)));
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
    printk("[UART1] mux: GP%d CTRL 0x%08lx PAD 0x%03lx | GP%d CTRL 0x%08lx PAD 0x%03lx\n",
           CONFIG_UART1_TX_GPIO, (unsigned long)REG(IO_BANK0_CTRL(CONFIG_UART1_TX_GPIO)),
           (unsigned long)REG(PADS_BANK0_PAD(CONFIG_UART1_TX_GPIO)),
           CONFIG_UART1_RX_GPIO, (unsigned long)REG(IO_BANK0_CTRL(CONFIG_UART1_RX_GPIO)),
           (unsigned long)REG(PADS_BANK0_PAD(CONFIG_UART1_RX_GPIO)));
    printk("[UART1] TX GP%d, RX GP%d, %d baud. Sending a burst, then listening %u ms.\n",
           CONFIG_UART1_TX_GPIO, CONFIG_UART1_RX_GPIO, (int)CONFIG_UART1_BAUD, listen_ms);

    /* Internal loopback first, which touches no pins at all: the PL011's
     * CR.LBE (bit 7) ties TXD to RXD inside the peripheral. It separates two
     * questions that otherwise look identical from outside --
     *
     *   passes  the clock, divisor, enables, FIFOs and this driver's own
     *           read/write code are all correct, and anything still broken is
     *           the pin mux or the wire;
     *   fails   the fault is in here, and no amount of re-jumpering will help.
     *
     * Worth its few lines: without it, "0 bytes received" on a loopback
     * jumper is ambiguous between a driver bug and a jumper on the wrong two
     * pins, and the two have very different owners. */
    {
        uint32_t saved = REG(U1_CR);
        REG(U1_CR) = saved | (1u << 7);                /* LBE */
        while (u1_has_char()) (void)u1_getc();         /* drain first */

        static const char probe[] = "LOOP";
        for (unsigned i = 0; i < 4; i++) u1_putc((uint8_t)probe[i]);

        uint64_t t_end = time_get_ms() + 100;
        char got[8];
        unsigned g = 0;
        while (time_get_ms() < t_end && g < 4) {
            if (u1_has_char()) got[g++] = (char)u1_getc();
        }
        got[g] = 0;
        printk("[UART1] internal loopback (no pins): sent \"LOOP\", got \"%s\" -- %s\n",
               got, (g == 4) ? "PASS: the peripheral and this driver are fine"
                             : "FAIL: the fault is in this driver's setup");
        REG(U1_CR) = saved;
        while (u1_has_char()) (void)u1_getc();
    }

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

/* Continuity, with the UART taken out of the picture entirely.
 *
 * Drives TX as a plain SIO output and reads RX as a plain SIO input. With a
 * jumper between the two pins, the reads follow the writes; without one, RX
 * sits at whatever its pull says. That distinguishes the last two candidates
 * a failing downlink leaves once internal loopback has passed:
 *
 *   follows      the pins and the wire are fine, so the fault is the UART
 *                mux -- a different pin pair is worth trying;
 *   stuck        the jumper is not on these two pins (GP8 is physical pin
 *                11 and GP9 is pin 12 on a Pico 2, which is exactly the
 *                kind of thing that gets miscounted), or a pin is damaged.
 *
 * Restores the UART mux afterwards, so the link keeps working. */
void uart1_pin_test(void) {
    if (!g_up) { printk("[UART1] not initialised\n"); return; }

    const unsigned tx = CONFIG_UART1_TX_GPIO, rx = CONFIG_UART1_RX_GPIO;

    REG(IO_BANK0_CTRL(tx)) = 5;                    /* SIO */
    REG(PADS_BANK0_PAD(tx)) = 0x56;
    REG(SIO_GPIO_OE_SET) = (1u << tx);
    REG(IO_BANK0_CTRL(rx)) = 5;
    REG(PADS_BANK0_PAD(rx)) = 0x5A;                /* input, pull-up */
    REG(SIO_GPIO_OE_CLR) = (1u << rx);

    unsigned followed = 0;
    for (unsigned i = 0; i < 8; i++) {
        bool high = (i & 1) == 0;
        if (high) REG(SIO_GPIO_OUT_SET) = (1u << tx);
        else      REG(SIO_GPIO_OUT_CLR) = (1u << tx);
        time_delay_us(2000);
        bool got = (REG(SIO_GPIO_IN) & (1u << rx)) != 0;
        printk("[UART1] GP%u driven %s -> GP%u reads %s%s\n",
               tx, high ? "HIGH" : "LOW ", rx, got ? "HIGH" : "LOW ",
               (got == high) ? "  (follows)" : "  <-- does not follow");
        if (got == high) followed++;
    }

    /* Second half: put TX back under the UART, keep RX as a plain input, and
     * watch for edges while the UART transmits. Internal loopback has already
     * proved the peripheral works and the GPIO walk has proved the wire does,
     * so the only question left is which side of the pin mux is not taking --
     * and a transmitting UART that produces no edges on a wire known good
     * answers it. */
    REG(IO_BANK0_CTRL(tx)) = 2;                    /* TX back to UART1 */
    REG(PADS_BANK0_PAD(tx)) = 0x56;
    time_delay_us(1000);

    unsigned edges = 0;
    bool prev = (REG(SIO_GPIO_IN) & (1u << rx)) != 0;
    for (unsigned i = 0; i < 8; i++) u1_putc((uint8_t)0x55);   /* 0x55 = max edges */
    uint64_t t_end = time_get_ms() + 50;
    while (time_get_ms() < t_end) {
        bool now = (REG(SIO_GPIO_IN) & (1u << rx)) != 0;
        if (now != prev) { edges++; prev = now; }
    }
    printk("[UART1] UART transmitting on GP%u, GP%u sampled as GPIO: %u edge(s) -- %s\n",
           tx, rx, edges,
           edges ? "the UART IS driving the pin, so the RX mux is the suspect"
                 : "the UART is NOT driving the pin: the TX mux never took");

    printk("[UART1] %u/8 followed -- %s\n", followed,
           (followed == 8) ? "GP8 and GP9 ARE connected: the wire is good, suspect the UART mux"
                           : "GP8 and GP9 are NOT connected (GP8 = pin 11, GP9 = pin 12)");

    /* Back to UART duty. */
    REG(IO_BANK0_CTRL(tx)) = 2;
    REG(PADS_BANK0_PAD(tx)) = 0x56;
    REG(IO_BANK0_CTRL(rx)) = 2;
    REG(PADS_BANK0_PAD(rx)) = 0x5A;
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
void uart1_pin_test(void) { }
p9_link_t *uart1_get_link(void) { return 0; }

#endif
