/* PIO-USB host engine -- 36.7, plan/phase36_rp2350_lcd7_terminal.md §3.2.
 *
 * A full-speed USB host port made from two PIO blocks and the second core:
 *
 *   PIO0 SM0  TX: NRZI encoding and bit stuffing at 48 MHz (4 clocks a bit),
 *             fed a byte at a time; it ends each packet with an EOP by
 *             itself when its FIFO runs dry.
 *   PIO1 SM1  edge detector at 96 MHz: finds each bit's centre, triggers
 *             the decoder, and flags start of packet and EOP.
 *   PIO1 SM0  NRZI decoder: turns the triggers into bytes, dropping stuff
 *             bits, autopushed 8 at a time.
 *   core 1    the engine below: SOF every millisecond, bus reset, and one
 *             transaction at a time from the ring core 0 fills. It runs from
 *             RAM with interrupts off and never touches flash, so a flash
 *             write on core 0 does not disturb it (§3.2.1), and nothing else
 *             can delay the ACK it owes a device ~1.3 us after a DATA packet.
 *
 * The three PIO programs are sekigon-gonnoc's Pico-PIO-USB (usb_tx.pio's
 * usb_tx_fs, usb_rx.pio's usb_nrzi_decoder and usb_edge_detector),
 * assembled with the Pico SDK's pioasm and embedded as instruction words
 * below. The C engine is written for this tree. Pico-PIO-USB is:
 *
 *   MIT License
 *
 *   Copyright (c) 2021 sekigon-gonnoc
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included
 *   in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Register facts (RP2350 datasheet; offsets as used by drivers/lcd7_rp2350.c):
 *   PIO0 0x50200000, PIO1 0x50300000: CTRL 0x000 (SM_ENABLE [3:0],
 *     SM_RESTART [7:4], CLKDIV_RESTART [11:8]), FSTAT 0x004 (RXEMPTY
 *     [11:8], TXFULL [19:16]), TXF0 0x010, RXF0 0x020, IRQ 0x030 (write 1
 *     to clear), INSTR_MEM 0x048, SMn at 0x0c8 + 0x18n: CLKDIV, EXECCTRL,
 *     SHIFTCTRL, ADDR, INSTR, PINCTRL; GPIOBASE 0x168. Atomic aliases:
 *     +0x1000 XOR, +0x2000 SET, +0x3000 CLR.
 *   EXECCTRL: SIDE_EN 30, JMP_PIN [28:24], WRAP_TOP [16:12], WRAP_BOTTOM
 *     [11:7]. SHIFTCTRL: FJOIN_RX 31, FJOIN_TX 30, PULL_THRESH [29:25],
 *     PUSH_THRESH [24:20], OUT_SHIFTDIR 19, IN_SHIFTDIR 18, AUTOPULL 17,
 *     AUTOPUSH 16. PINCTRL: SIDESET_COUNT [31:29], SET_COUNT [28:26],
 *     OUT_COUNT [25:20], IN_BASE [19:15], SIDESET_BASE [14:10], SET_BASE
 *     [9:5], OUT_BASE [4:0].
 *   RESETS: PIO0 bit 11, PIO1 bit 12. FUNCSEL PIO0 = 6. IO_BANK0 CTRL
 *     INOVER [17:16] (1 = invert). Pads: ISO 8, IE 6, DRIVE [5:4] (3 =
 *     12 mA), PDE 2, SCHMITT 1, SLEWFAST 0.
 *   SIO GPIO_HI_IN 0xd0000008: GPIO 32..47. TIMER0 TIMERAWL 0x400b0028 (us).
 *
 * Both USB pins are input-inverted, as the RX programs expect: every read of
 * them here -- PIO or SIO -- sees the complement of the line.
 */

#include "drivers/piousb.h"
#include "drivers/usb_crc.h"
#include "kernel/hart.h"
#include "kernel/lock.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_PIOUSB_DP_GPIO)

#if CONFIG_ENABLE_SMP
#error "piousb: the USB engine owns core 1, which an SMP persona gives to the scheduler"
#endif
_Static_assert(CONFIG_PIOUSB_DM_GPIO == CONFIG_PIOUSB_DP_GPIO + 1,
               "piousb: D- must follow D+ (the programs drive and read them as a pair)");
_Static_assert(CONFIG_PIOUSB_DP_GPIO >= 32 && CONFIG_PIOUSB_DM_GPIO <= 47,
               "piousb: the pins must lie in PIO's GPIOBASE=16 window and in SIO's GPIO_HI");
_Static_assert(CONFIG_CLK_SYS_HZ % 48000000u == 0,
               "piousb: the TX program needs an integer divider to 48 MHz");

#define REG(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define ALIAS_SET 0x2000u
#define ALIAS_CLR 0x3000u

#define PIO0_BASE        0x50200000UL
#define PIO1_BASE        0x50300000UL
#define PIO_CTRL(b)      ((b) + 0x000)
#define PIO_FSTAT(b)     ((b) + 0x004)
#define PIO_TXF(b, sm)   ((b) + 0x010 + 4u * (sm))
#define PIO_RXF(b, sm)   ((b) + 0x020 + 4u * (sm))
#define PIO_IRQ(b)       ((b) + 0x030)
#define PIO_INSTR_MEM(b, i) ((b) + 0x048 + 4u * (i))
#define PIO_SM(b, sm)    ((b) + 0x0c8 + 0x18u * (sm))
#define SM_CLKDIV(b, sm)    (PIO_SM(b, sm) + 0x00)
#define SM_EXECCTRL(b, sm)  (PIO_SM(b, sm) + 0x04)
#define SM_SHIFTCTRL(b, sm) (PIO_SM(b, sm) + 0x08)
#define SM_INSTR(b, sm)     (PIO_SM(b, sm) + 0x10)
#define SM_PINCTRL(b, sm)   (PIO_SM(b, sm) + 0x14)
#define PIO_GPIOBASE(b)  ((b) + 0x168)

#define TX_PIO   PIO0_BASE
#define TX_SM    0u
#define RX_PIO   PIO1_BASE
#define DEC_SM   0u
#define EDGE_SM  1u
#define DEC_OFF  0u      /* usb_nrzi_decoder, 15 words */
#define EDGE_OFF 15u     /* usb_edge_detector, 17 words: fills PIO1 */

#define FSTAT_RXEMPTY(sm) (1u << (8 + (sm)))
#define FSTAT_TXFULL(sm)  (1u << (16 + (sm)))
#define FJOIN_RX          (1u << 31)

/* The programs' IRQ flags (usb_tx.pio / usb_rx.pio). */
#define TX_COMP  (1u << 0)
#define TX_EOP   (1u << 1)
#define RX_EOP   (1u << 2)
#define RX_ALL   ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4))

/* Instructions core 1 executes directly (encoded with pioasm). */
#define I_SET_PINS_SE0   0xe000u
#define I_SET_PINS_J     0xe001u
#define I_SET_PINDIRS_OUT 0xe083u
#define I_SET_PINDIRS_IN 0xe080u
#define I_SET_X_0        0xe020u
#define I_MOV_OSR_NOTNULL 0xa0ebu
#define I_JMP(a)         ((uint32_t)(a))

/* 36.8: low speed behind a full-speed hub. Every packet the host sends such a
 * device goes out as a full-speed PRE (SYNC + PID 0x3C, no EOP), then at
 * 1.5 Mbit/s with full-speed polarity; the hub opens its low-speed port for
 * it, and passes the device's replies back the same way. The TX program's
 * EOP section (words 4-7) is swapped for Pico-PIO-USB's usb_tx_fs_pre for the
 * PRE itself -- written as immediates below, not a const table, which would
 * be .rodata in flash. */
#define PID_PRE          0x3Cu
#define TX_DIV_FS        ((CONFIG_CLK_SYS_HZ / 48000000u) << 16)
#define TX_DIV_LS        ((CONFIG_CLK_SYS_HZ / 6000000u) << 16)
#define EDGE_DIV_FS      ((uint32_t)((((uint64_t)CONFIG_CLK_SYS_HZ * 256u / 96000000u) >> 8) << 16) | \
                          (uint32_t)((((uint64_t)CONFIG_CLK_SYS_HZ * 256u / 96000000u) & 0xffu) << 8))
#define EDGE_DIV_LS      ((CONFIG_CLK_SYS_HZ / 12000000u) << 16)
_Static_assert(CONFIG_CLK_SYS_HZ % 12000000u == 0, "piousb: low speed needs an integer divider to 6 and 12 MHz");

#define PIN_DP   (CONFIG_PIOUSB_DP_GPIO - 16u)   /* PIO pin numbers, GPIOBASE 16 */
#define PIN_DM   (CONFIG_PIOUSB_DM_GPIO - 16u)

#define RESETS_RESET_CLR  (0x40020000UL + 0x3000)
#define RESETS_RESET_DONE (0x40020000UL + 0x8)
#define RESET_PIO0        (1u << 11)
#define RESET_PIO1        (1u << 12)
#define IO_BANK0_CTRL(n)  (0x40028004UL + (n) * 8u)
#define PADS_BANK0(n)     (0x40038004UL + (n) * 4u)
#define FUNCSEL_PIO0      6u
#define INOVER_INVERT     (1u << 16)
#define PAD_USB           ((1u << 6) | (3u << 4) | (1u << 2) | (1u << 1) | 1u)
#define SIO_GPIO_HI_IN    0xd0000008UL
#define TIMER0_TIMERAWL   (0x400B0000UL + 0x28)

#define CPU_MHZ (CONFIG_CLK_SYS_HZ / 1000000u)

/* PID bytes, as sent (the PID nibble plus its complement). */
#define PID_OUT   0xE1u
#define PID_IN    0x69u
#define PID_SOF   0xA5u
#define PID_SETUP 0x2Du
#define PID_DATA0 0xC3u
#define PID_DATA1 0x4Bu
#define PID_ACK   0xD2u
#define PID_NAK   0x5Au
#define PID_STALL 0x1Eu
#define USB_SYNC  0x80u

/* --- The programs (see the header comment for their origin) ------------- */

/* usb_tx_fs: .side_set 2 opt; wrap 2..21; IRQ 0 = complete, 1 = EOP start. */
static const uint16_t k_tx_prog[22] = {
    0xf445, 0xe083, 0x00ea, 0xa142, 0xd301, 0xa342, 0xb442, 0xe380,
    0xc020, 0x0000, 0x6021, 0x002e, 0x1482, 0xa242, 0xf845, 0x00f1,
    0x0104, 0x6021, 0x0035, 0x188f, 0xa242, 0xf445,
};
/* usb_nrzi_decoder: wrap 0..14. */
static const uint16_t k_dec_prog[15] = {
    0xe046, 0x20c4, 0x00c9, 0x006e, 0x0027, 0x4061, 0x000e, 0x40e1,
    0x0081, 0x006e, 0x002d, 0x4021, 0x0081, 0x4061, 0xa029,
};
/* usb_edge_detector: wrap 3..9, entered at 1; its jumps are relocated by
 * EDGE_OFF when loaded. IRQ 2 = EOP, 3 = start, 4 = decoder trigger. */
static const uint16_t k_edge_prog[17] = {
    0xc022, 0x00c1, 0xc103, 0xc104, 0x00cc, 0x00cc, 0x00cc, 0x00cc,
    0x00cc, 0x00cc, 0xa226, 0x0040, 0xa0c3, 0x4001, 0xc004, 0x00ca,
    0x0005,
};

/* --- State shared between the cores -------------------------------------
 *
 * drivers/piousb.h's piousb_shared_t: the ring (the request and completion
 * rings of §3.2 folded into one, which saves copying a 76-byte slot across)
 * and the status. Padded to exactly 512 bytes on a 512-byte boundary, so a
 * PMP grant of it covers nothing else. .bss, in SRAM. */
#define RING PIOUSB_RING
static union {
    piousb_shared_t s;
    uint8_t         raw[512];
} g_usb_mem __attribute__((aligned(512)));
_Static_assert(sizeof(piousb_shared_t) <= 512, "piousb: the shared block outgrew its 512-byte grant");
#define g_ring g_usb_mem.s
#define g_st   g_usb_mem.s.st

piousb_shared_t *piousb_shared(void) { return &g_usb_mem.s; }

void piousb_shared_region(uintptr_t *base, uintptr_t *size) {
    *base = (uintptr_t)&g_usb_mem;
    *size = sizeof(g_usb_mem);
}
static uint16_t        g_crc_tbl[256];
static ylock_t         g_xfer_lock;
static bool            g_inited;

/* --- Core 1: the engine --------------------------------------------------
 *
 * Everything from here to core1_main() runs on core 1 from SRAM (.ramfunc),
 * and must stay that way: no calls outside .ramfunc (no libc, no libgcc, no
 * struct copies the compiler turns into memcpy), no switch jump tables
 * (-fno-jump-tables for this file), no constant tables in .rodata.
 * tools/check_core1_ram.py fails the build on any of them. */
#define ENG  __attribute__((section(".ramfunc"), noinline))
#define ENGI static inline __attribute__((always_inline))

#define ST ((volatile piousb_status_t *)&g_st)

static uint32_t g_release_cyc;        /* when tx_packet() let the SM go */
static bool     g_ls;                 /* the current transaction is low speed */
static uint16_t g_frame;
static uint32_t g_tx_stuck;

ENGI uint32_t cyc(void) {
    uint32_t c;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(c));
    return c;
}

ENGI uint32_t now_us(void) { return REG(TIMER0_TIMERAWL); }

ENGI void fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* Sends one packet (SYNC first) and returns once its EOP has begun, which is
 * when a reply can start arriving. The SM sits at `irq wait 0` between
 * packets; clearing that flag with bytes already in the FIFO sends them. */
ENG static bool tx_packet(const uint8_t *p, uint32_t n) {
    uint32_t t0 = cyc();
    while (!(REG(PIO_IRQ(TX_PIO)) & TX_COMP)) {
        if (cyc() - t0 > 30u * CPU_MHZ) { g_tx_stuck++; return false; }
    }
    /* Two bytes ahead, not a full FIFO: the prefill sits in the gap between
     * this packet and the one before it (token -> DATA), and eight polled
     * writes cost ~7 bit times of it. Two are 21 bit times of lead for the
     * loop below, which refills far faster than the SM drains. */
    uint32_t i = 0;
    while (i < n && i < 2u) REG(PIO_TXF(TX_PIO, TX_SM)) = p[i++];
    REG(PIO_IRQ(TX_PIO)) = TX_COMP | TX_EOP;
    g_release_cyc = cyc();
    /* 8 FIFO bytes are 5.3 us of slack; a byte leaves every 96 cycles. */
    t0 = cyc();
    while (i < n) {
        if (!(REG(PIO_FSTAT(TX_PIO)) & FSTAT_TXFULL(TX_SM))) REG(PIO_TXF(TX_PIO, TX_SM)) = p[i++];
        else if (cyc() - t0 > 200u * CPU_MHZ) { g_tx_stuck++; return false; }
    }
    t0 = cyc();
    while (!(REG(PIO_IRQ(TX_PIO)) & TX_EOP)) {
        if (cyc() - t0 > 200u * CPU_MHZ) { g_tx_stuck++; return false; }
    }
    return true;
}

/* The decoder is stopped while we transmit (it would decode our own
 * packets), and reset to a clean state before each reply. */
ENG static void rx_prepare(void) {
    REG(PIO_CTRL(RX_PIO) + ALIAS_CLR) = 1u << DEC_SM;
    REG(SM_SHIFTCTRL(RX_PIO, DEC_SM) + 0x1000u) = FJOIN_RX;   /* XOR twice: */
    REG(SM_SHIFTCTRL(RX_PIO, DEC_SM) + 0x1000u) = FJOIN_RX;   /* clears the FIFO */
    REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << (4 + DEC_SM);   /* SM_RESTART */
    REG(SM_INSTR(RX_PIO, DEC_SM)) = I_JMP(DEC_OFF);
    REG(SM_INSTR(RX_PIO, DEC_SM)) = I_SET_X_0;
    REG(SM_INSTR(RX_PIO, DEC_SM)) = I_MOV_OSR_NOTNULL;
}

/* Called as our last packet's EOP begins. The edge detector watches the
 * wire the whole time, so it sees our own packet end, raises its EOP flag
 * and waits there. Clearing the flag before it has been raised would leave
 * it waiting through the device's reply -- so wait for it first (it comes
 * within a bit time or two). The reference implementation gets this for
 * free from a slower call path; this one is fast enough to lose the race. */
ENG static void rx_start(void) {
    uint32_t t0 = cyc();
    uint32_t wait = (g_ls ? 25u : 3u) * CPU_MHZ;   /* an LS bit is 8 FS bits */
    while (!(REG(PIO_IRQ(RX_PIO)) & RX_EOP) && cyc() - t0 < wait) { }
    uint32_t w = cyc() - t0;
    if (!(REG(PIO_IRQ(RX_PIO)) & RX_EOP)) ST->rx_eop_misses++;
    else if (w > ST->rx_eop_wait_max_cycles) ST->rx_eop_wait_max_cycles = w;
    REG(PIO_IRQ(RX_PIO)) = RX_ALL;
    REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << DEC_SM;
}

ENGI void rx_stop(void) { REG(PIO_CTRL(RX_PIO) + ALIAS_CLR) = 1u << DEC_SM; }

ENGI bool rx_byte(uint32_t *b) {
    if (REG(PIO_FSTAT(RX_PIO)) & FSTAT_RXEMPTY(DEC_SM)) return false;
    *b = REG(PIO_RXF(RX_PIO, DEC_SM)) >> 24;
    return true;
}

/* Receives one packet into buf[] (SYNC and PID stripped, CRC bytes kept).
 * Returns the PID, or 0 on timeout. *n is the byte count after the PID;
 * *crc the running CRC16 over them, which is USB_CRC16_RESIDUAL for a good
 * DATA packet. *eop_cyc: when the end of the packet was seen. */
ENG static uint32_t rx_packet(uint8_t *buf, uint32_t max, uint32_t *n, uint32_t *crc, uint32_t *eop_cyc) {
    uint32_t t0 = cyc(), idx = 0, pid = 0, c = 0xffffu, b;
    uint32_t limit = (g_ls ? 80u : 20u) * CPU_MHZ;   /* SYNC due within ~1 us (FS) */
    for (;;) {
        if (rx_byte(&b)) {
            if (idx == 1) pid = b;
            else if (idx >= 2) {
                c = (c >> 8) ^ g_crc_tbl[(c ^ b) & 0xffu];
                if (idx - 2 < max) buf[idx - 2] = (uint8_t)b;
            }
            idx++;
            limit = (g_ls ? 1200u : 120u) * CPU_MHZ;  /* a 64-byte FS packet is ~50 us */
            continue;
        }
        if (REG(PIO_IRQ(RX_PIO)) & RX_EOP) {
            if (rx_byte(&b)) {                  /* one may have landed since */
                if (idx >= 2) {
                    c = (c >> 8) ^ g_crc_tbl[(c ^ b) & 0xffu];
                    if (idx - 2 < max) buf[idx - 2] = (uint8_t)b;
                } else if (idx == 1) pid = b;
                idx++;
            }
            *eop_cyc = cyc();
            break;
        }
        if (cyc() - t0 > limit) break;
    }
    ST->dbg_rx_flags = REG(PIO_IRQ(RX_PIO)) & 0xffu;
    ST->dbg_rx_bytes = idx;
    if (!(REG(PIO_IRQ(RX_PIO)) & RX_EOP)) idx = 0;      /* timed out */
    rx_stop();
    *n = idx >= 2 ? idx - 2 : 0;
    *crc = c;
    return idx >= 2 ? pid : 0;
}

ENGI bool pid_valid(uint32_t pid) { return ((pid >> 4) ^ (pid & 0xfu)) == 0xfu; }

ENG static void send_token(uint32_t pid, uint32_t addr, uint32_t ep, uint8_t *tok) {
    uint32_t v = (addr & 0x7fu) | ((ep & 0xfu) << 7);
    uint32_t crc = 0x1f;
    for (uint32_t i = 0; i < 11; i++) {
        uint32_t bit = (v >> i) & 1u;
        crc = ((crc ^ bit) & 1u) ? ((crc >> 1) ^ 0x14u) : (crc >> 1);
    }
    crc ^= 0x1fu;
    tok[0] = USB_SYNC; tok[1] = (uint8_t)pid;
    tok[2] = (uint8_t)v; tok[3] = (uint8_t)((crc << 3) | (v >> 8));
    tx_packet(tok, 4);
}

/* [SYNC, DATAx, payload, CRC16 lo, hi] into pkt; returns its length. */
ENG static uint32_t build_data(uint8_t *pkt, bool data1, const uint8_t *d, uint32_t n) {
    uint32_t c = 0xffffu;
    pkt[0] = USB_SYNC;
    pkt[1] = data1 ? PID_DATA1 : PID_DATA0;
    for (uint32_t i = 0; i < n; i++) {
        pkt[2 + i] = d[i];
        c = (c >> 8) ^ g_crc_tbl[(c ^ d[i]) & 0xffu];
    }
    c ^= 0xffffu;
    pkt[2 + n] = (uint8_t)c;
    pkt[3 + n] = (uint8_t)(c >> 8);
    return n + 4;
}

ENGI void count(uint32_t status) {
    volatile piousb_status_t *st = ST;
    st->xfers++;
    if (status == PIOUSB_OK) st->acks++;
    else if (status == PIOUSB_NAK) st->naks++;
    else if (status == PIOUSB_STALL) st->stalls++;
    else if (status == PIOUSB_TIMEOUT) st->timeouts++;
    else if (status == PIOUSB_CRC) st->crc_errors++;
    else if (status == PIOUSB_TOGGLE) st->toggles++;
    else st->pid_errors++;
}

ENGI uint32_t handshake_status(uint32_t pid) {
    if (pid == 0) return PIOUSB_TIMEOUT;
    if (pid == PID_ACK) return PIOUSB_OK;
    if (pid == PID_NAK) return PIOUSB_NAK;
    if (pid == PID_STALL) return PIOUSB_STALL;
    return PIOUSB_PROTOCOL;
}

/* The edge detector at a new rate: stopped, re-clocked, started again where
 * it was -- as Pico-PIO-USB does it (send_pre(), restore_fs_bus()). */
ENGI void edge_restart(uint32_t div) {
    REG(PIO_CTRL(RX_PIO) + ALIAS_CLR) = 1u << EDGE_SM;
    REG(SM_CLKDIV(RX_PIO, EDGE_SM)) = div;
    REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << EDGE_SM;
}

ENGI bool tx_parked(uint32_t us) {
    uint32_t t0 = cyc();
    while (!(REG(PIO_IRQ(TX_PIO)) & TX_COMP)) {
        if (cyc() - t0 > us * CPU_MHZ) { g_tx_stuck++; return false; }
    }
    return true;
}

/* A PRE at full speed, then the bus set for one low-speed packet: TX at
 * 6 MHz (1.5 Mbit/s), the edge detector at 12 MHz. Before *every* host
 * packet to a low-speed device -- token, DATA and ACK alike. */
ENG static void tx_pre(void) {
    if (!tx_parked(100)) return;
    REG(PIO_CTRL(TX_PIO) + ALIAS_CLR) = 1u << TX_SM;
    REG(SM_CLKDIV(TX_PIO, TX_SM)) = TX_DIV_FS;
    REG(PIO_INSTR_MEM(TX_PIO, 4)) = 0xd701;     /* irq nowait 1 side J [3] */
    REG(PIO_INSTR_MEM(TX_PIO, 5)) = 0xe080;     /* set pindirs, 0 */
    REG(PIO_INSTR_MEM(TX_PIO, 6)) = 0xa042;     /* nop */
    REG(PIO_INSTR_MEM(TX_PIO, 7)) = 0xa042;     /* nop */
    REG(PIO_CTRL(TX_PIO) + ALIAS_SET) = 1u << TX_SM;
    REG(PIO_TXF(TX_PIO, TX_SM)) = USB_SYNC;
    REG(PIO_TXF(TX_PIO, TX_SM)) = PID_PRE;
    REG(PIO_IRQ(TX_PIO)) = TX_COMP | TX_EOP;
    (void)tx_parked(20);
    REG(PIO_CTRL(TX_PIO) + ALIAS_CLR) = 1u << TX_SM;
    REG(PIO_INSTR_MEM(TX_PIO, 4)) = 0xd301;     /* irq nowait 1 side SE0 [3] */
    REG(PIO_INSTR_MEM(TX_PIO, 5)) = 0xa342;     /* nop [3] */
    REG(PIO_INSTR_MEM(TX_PIO, 6)) = 0xb442;     /* nop side J */
    REG(PIO_INSTR_MEM(TX_PIO, 7)) = 0xe380;     /* set pindirs, 0 [3] */
    REG(SM_CLKDIV(TX_PIO, TX_SM)) = TX_DIV_LS;
    REG(PIO_CTRL(TX_PIO) + ALIAS_SET) = 1u << TX_SM;
    edge_restart(EDGE_DIV_LS);
}

/* Back to full speed after a low-speed transaction, before the next SOF. */
ENG static void restore_fs(void) {
    (void)tx_parked(1000);
    REG(PIO_CTRL(TX_PIO) + ALIAS_CLR) = 1u << TX_SM;
    REG(SM_CLKDIV(TX_PIO, TX_SM)) = TX_DIV_FS;
    REG(PIO_CTRL(TX_PIO) + ALIAS_SET) = 1u << TX_SM;
    edge_restart(EDGE_DIV_FS);
    g_ls = false;
}

/* SETUP and OUT: token, DATA packet, then the device's handshake. */
ENG static void do_out(piousb_xfer_t *x, bool setup) {
    uint8_t tok[4], pkt[PIOUSB_MAX_PACKET + 4], hs[4];
    uint32_t len = x->len > PIOUSB_MAX_PACKET ? PIOUSB_MAX_PACKET : x->len;
    uint32_t plen = build_data(pkt, setup ? false : x->data1, x->data, len);
    uint32_t n, crc, eop;
    g_ls = x->low_speed != 0;
    rx_prepare();
    if (g_ls) tx_pre();
    send_token(setup ? PID_SETUP : PID_OUT, x->addr, setup ? 0 : x->ep, tok);
    if (g_ls) tx_pre();
    tx_packet(pkt, plen);
    rx_start();
    uint32_t pid = rx_packet(hs, sizeof(hs), &n, &crc, &eop);
    x->pid = (uint8_t)pid;
    x->rx_len = 0;
    x->status = (uint8_t)handshake_status(pid);
    if (g_ls) restore_fs();
}

/* IN: token, then a DATA packet we ACK at once -- the one timing-critical
 * moment in the protocol (the device gives up after 16-18 bit times). */
ENG static void do_in(piousb_xfer_t *x) {
    /* Built on the stack, not a static const: that would be .rodata, in
     * flash, which this code must never read. */
    uint8_t tok[4], ackp[2], rbuf[PIOUSB_MAX_PACKET + 2];
    uint32_t n, crc, eop = 0;
    ackp[0] = USB_SYNC;
    ackp[1] = PID_ACK;
    g_ls = x->low_speed != 0;
    rx_prepare();
    if (g_ls) tx_pre();
    send_token(PID_IN, x->addr, x->ep, tok);
    rx_start();
    uint32_t pid = rx_packet(rbuf, sizeof(rbuf), &n, &crc, &eop);
    x->pid = (uint8_t)pid;
    x->rx_len = 0;
    if (pid == PID_DATA0 || pid == PID_DATA1) {
        if (n < 2 || crc != USB_CRC16_RESIDUAL) { x->status = PIOUSB_CRC; if (g_ls) restore_fs(); return; }
        if (g_ls) tx_pre();
        tx_packet(ackp, 2);
        if (g_ls) restore_fs();
        uint32_t ta = g_release_cyc - eop;
        if (ta > ST->turnaround_max_cycles) ST->turnaround_max_cycles = ta;
        x->rx_len = (uint16_t)(n - 2 > PIOUSB_MAX_PACKET ? PIOUSB_MAX_PACKET : n - 2);
        for (uint32_t i = 0; i < x->rx_len; i++) x->data[i] = rbuf[i];
        x->status = (pid == (x->data1 ? PID_DATA1 : PID_DATA0)) ? PIOUSB_OK : PIOUSB_TOGGLE;
        return;
    }
    x->status = (uint8_t)(pid_valid(pid) || pid == 0 ? handshake_status(pid) : PIOUSB_PROTOCOL);
    if (g_ls) restore_fs();
}

/* With len == 0: our token alone. With len == 8: a whole SETUP stage --
 * token, DATA0 with x->data, and whatever comes back -- the RX side kept
 * running throughout and re-armed after each of our EOPs, as a bus analyser
 * would see it. */
ENG static void do_loopback(piousb_xfer_t *x) {
    uint8_t tok[4], pkt[12], d[8];
    uint32_t b, n = 0;
    bool whole = (x->len == 8);
    for (uint32_t i = 0; i < 8; i++) d[i] = x->data[i];
    uint32_t plen = build_data(pkt, false, d, 8);
    g_ls = x->low_speed != 0;
    rx_prepare();
    if (g_ls) tx_pre();                         /* then decode our LS token */
    REG(PIO_IRQ(RX_PIO)) = RX_ALL;
    REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << DEC_SM;
    send_token(PID_SETUP, x->addr, x->ep, tok);
    if (whole && g_ls) {
        /* Low speed: our token and DATA0 behind PREs, then whatever the
         * device says back. Our DATA0 is not read back (tx_packet() cannot
         * drain RX meanwhile); the reply is what this is for. */
        uint32_t t1 = cyc();
        while (!(REG(PIO_IRQ(RX_PIO)) & RX_EOP) && cyc() - t1 < 40u * CPU_MHZ) {
            if (rx_byte(&b) && n < PIOUSB_MAX_PACKET) x->data[n++] = (uint8_t)b;
        }
        while (rx_byte(&b)) if (n < PIOUSB_MAX_PACKET) x->data[n++] = (uint8_t)b;
        rx_prepare();
        tx_pre();
        tx_packet(pkt, plen);
        rx_start();
        if (n < PIOUSB_MAX_PACKET) x->data[n++] = 0xEE;      /* marks "our packets end here" */
    } else if (whole) {
        uint32_t t1 = cyc();
        while (!(REG(PIO_IRQ(RX_PIO)) & RX_EOP) && cyc() - t1 < 3u * CPU_MHZ) { }
        while (rx_byte(&b)) if (n < PIOUSB_MAX_PACKET) x->data[n++] = (uint8_t)b;
        rx_prepare();                           /* fresh NRZI state per packet */
        REG(PIO_IRQ(RX_PIO)) = RX_ALL;
        REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << DEC_SM;
        /* tx_packet(), inline, draining RX as it goes: the RX FIFO holds
         * only 8 bytes, and a 12-byte packet must not be clipped by it. */
        while (!(REG(PIO_IRQ(TX_PIO)) & TX_COMP)) { }
        uint32_t i = 0;
        while (i < plen && !(REG(PIO_FSTAT(TX_PIO)) & FSTAT_TXFULL(TX_SM))) REG(PIO_TXF(TX_PIO, TX_SM)) = pkt[i++];
        uint32_t pre = i;
        REG(PIO_IRQ(TX_PIO)) = TX_COMP | TX_EOP;
        t1 = cyc();
        while (cyc() - t1 < 15u * CPU_MHZ) {
            if (i < plen && !(REG(PIO_FSTAT(TX_PIO)) & FSTAT_TXFULL(TX_SM))) REG(PIO_TXF(TX_PIO, TX_SM)) = pkt[i++];
            if (rx_byte(&b) && n < PIOUSB_MAX_PACKET) x->data[n++] = (uint8_t)b;
            if (i == plen && (REG(PIO_IRQ(RX_PIO)) & RX_EOP)) break;
        }
        while (rx_byte(&b)) if (n < PIOUSB_MAX_PACKET) x->data[n++] = (uint8_t)b;
        x->addr = (uint8_t)pre;                 /* report how much the prefill took */
        rx_prepare();
        REG(PIO_IRQ(RX_PIO)) = RX_ALL;
        REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << DEC_SM;
    }
    uint32_t t0 = cyc();
    while (cyc() - t0 < (g_ls ? 150u : 20u) * CPU_MHZ) {
        if (rx_byte(&b) && n < PIOUSB_MAX_PACKET) x->data[n++] = (uint8_t)b;
    }
    if (g_ls) restore_fs();
    rx_stop();
    x->rx_len = (uint16_t)n;
    x->pid = (uint8_t)(REG(PIO_IRQ(RX_PIO)) & 0xffu);   /* the RX flags, for the record */
    x->status = PIOUSB_OK;
}

ENG static void send_sof(void) {
    uint8_t tok[4];
    g_frame = (uint16_t)((g_frame + 1u) & 0x7ffu);
    send_token(PID_SOF, g_frame & 0x7fu, (uint32_t)g_frame >> 7, tok);
    ST->sof_count++;
    ST->frame = g_frame;
}

/* The line, un-inverted: bit 0 D+, bit 1 D-. J (full-speed idle) is 1. */
ENGI uint32_t line_state(void) {
    uint32_t in = ~REG(SIO_GPIO_HI_IN) >> (CONFIG_PIOUSB_DP_GPIO - 32u);
    return in & 3u;
}

ENG static void line_check(void) {
    volatile piousb_status_t *st = ST;
    uint32_t ls = line_state();
    if (!st->connected) {
        if (ls == 1u || ls == 2u) {
            st->connected = true;
            st->full_speed = (ls == 1u);
            st->attaches++;
        }
    } else if (ls == 0u) {
        uint32_t t0 = cyc();
        while (cyc() - t0 < 3u * CPU_MHZ) { }  /* SE0 > 2.5 us is a detach */
        if (line_state() == 0u) {
            st->connected = false;
            st->enabled = false;
            st->detaches++;
        }
    }
}

ENGI void complete(void) {
    fence();
    g_ring.completed = g_ring.completed + 1u;
}

ENG static void __attribute__((noreturn)) engine(void) {
    volatile piousb_status_t *st = ST;
    uint32_t next_sof = now_us() + 1000u;
    bool in_reset = false;
    uint32_t reset_end = 0;
    st->running = true;
    for (;;) {
        st->loops++;
        uint32_t now = now_us();
        if (in_reset) {
            if ((int32_t)(now - reset_end) < 0) continue;
            st->dbg_reset_line = line_state();
            REG(SM_INSTR(TX_PIO, TX_SM)) = I_SET_PINDIRS_IN;
            uint32_t t0 = cyc();
            while (cyc() - t0 < 5u * CPU_MHZ) { }
            uint32_t ls = line_state();
            piousb_xfer_t *x = &g_ring.slot[g_ring.completed & (RING - 1u)];
            st->connected = (ls == 1u || ls == 2u);
            st->full_speed = (ls == 1u);
            /* A low-speed device at the root needs keep-alives rather than
             * SOFs, and tokens at 1.5 Mbit/s: not built (the keyboard's hub
             * is full speed), so the port stays disabled. */
            st->enabled = st->connected && st->full_speed;
            x->speed = st->full_speed ? 1 : 0;
            x->status = st->enabled ? PIOUSB_OK : PIOUSB_NODEV;
            in_reset = false;
            complete();
            next_sof = now_us() + 1000u;
            if (st->enabled) send_sof();
            continue;
        }
        if ((int32_t)(now - next_sof) >= 0) {
            if ((int32_t)(now - next_sof) > 50) st->late_frames++;
            next_sof += 1000u;
            if ((int32_t)(now - next_sof) >= 0) next_sof = now + 1000u;
            line_check();
            if (st->enabled) send_sof();
            continue;
        }
        if (g_ring.submitted == g_ring.completed) continue;
        fence();
        piousb_xfer_t *x = &g_ring.slot[g_ring.completed & (RING - 1u)];
        if (x->op == PIOUSB_OP_RESET) {
            st->enabled = false;
            st->resets++;
            REG(SM_INSTR(TX_PIO, TX_SM)) = I_SET_PINS_SE0;
            REG(SM_INSTR(TX_PIO, TX_SM)) = I_SET_PINDIRS_OUT;
            reset_end = now + 20000u;               /* 20 ms of SE0 (>= 10 ms) */
            in_reset = true;
            continue;
        }
        if (!st->enabled) {
            x->status = PIOUSB_NODEV;
            x->rx_len = 0;
            complete();
            continue;
        }
        /* Keep every transaction inside its frame: the longest (a 64-byte
         * IN) is ~60 us on the wire, and the SOF must not be pushed late. */
        if ((int32_t)(next_sof - now) < (x->low_speed ? 400 : 150)) continue;
        if (x->op == PIOUSB_OP_SETUP) do_out(x, true);
        else if (x->op == PIOUSB_OP_OUT) do_out(x, false);
        else if (x->op == PIOUSB_OP_IN) do_in(x);
        else if (x->op == PIOUSB_OP_LOOPBACK) do_loopback(x);
        else x->status = PIOUSB_PROTOCOL;
        count(x->status);
        complete();
    }
}

/* Core 1's entry on this persona (boot_header.S calls it; kernel/smp.c's
 * own core1_main is SMP-only). mcycle is inhibited at reset, per core. */
__attribute__((section(".ramfunc"), noreturn)) void core1_main(void) {
    __asm__ __volatile__("csrw 0x320, zero");   /* mcountinhibit: count */
    engine();
}

/* --- Core 0 ------------------------------------------------------------- */

static void load(uintptr_t pio, unsigned at, const uint16_t *prog, unsigned n, unsigned reloc) {
    for (unsigned i = 0; i < n; i++) {
        uint32_t w = prog[i];
        if ((w & 0xe000u) == 0) w = (w & ~0x1fu) | (((w & 0x1fu) + reloc) & 0x1fu);  /* JMP */
        REG(PIO_INSTR_MEM(pio, at + i)) = w;
    }
}

static void sm_start_at(uintptr_t pio, unsigned sm, unsigned pc) {
    REG(PIO_CTRL(pio) + ALIAS_SET) = (1u << (4 + sm)) | (1u << (8 + sm));  /* restart, clkdiv restart */
    REG(SM_INSTR(pio, sm)) = I_JMP(pc);
}

int piousb_init(void) {
    ylock_init(&g_xfer_lock);
    usb_crc16_table(g_crc_tbl);

    REG(RESETS_RESET_CLR) = RESET_PIO0 | RESET_PIO1;
    for (int i = 0; i < 100000 && (REG(RESETS_RESET_DONE) & (RESET_PIO0 | RESET_PIO1)) != (RESET_PIO0 | RESET_PIO1); i++) { }
    REG(PIO_CTRL(TX_PIO)) = 0;
    REG(PIO_CTRL(RX_PIO)) = 0;
    REG(PIO_GPIOBASE(TX_PIO)) = 16;
    REG(PIO_GPIOBASE(RX_PIO)) = 16;

    /* Pull-downs are the host's side of attach detection; the device pulls
     * D+ (full speed) or D- (low speed) up. */
    for (unsigned g = CONFIG_PIOUSB_DP_GPIO; g <= CONFIG_PIOUSB_DM_GPIO; g++) {
        REG(PADS_BANK0(g)) = PAD_USB;
        REG(IO_BANK0_CTRL(g)) = INOVER_INVERT | FUNCSEL_PIO0;
    }

    load(TX_PIO, 0, k_tx_prog, 22, 0);
    load(RX_PIO, DEC_OFF, k_dec_prog, 15, DEC_OFF);
    load(RX_PIO, EDGE_OFF, k_edge_prog, 17, EDGE_OFF);

    /* TX: 48 MHz exactly, side-set/set/out on D+ and D-, 8-bit autopull
     * shifting right (USB is LSB first), FIFOs joined for 8 bytes of slack. */
    REG(SM_CLKDIV(TX_PIO, TX_SM)) = (CONFIG_CLK_SYS_HZ / 48000000u) << 16;
    REG(SM_EXECCTRL(TX_PIO, TX_SM)) = (1u << 30) | (21u << 12) | (2u << 7);
    REG(SM_SHIFTCTRL(TX_PIO, TX_SM)) = (1u << 30) | (8u << 25) | (1u << 19) | (1u << 17);
    REG(SM_PINCTRL(TX_PIO, TX_SM)) = (3u << 29) | (2u << 26) | (2u << 20) | (PIN_DP << 15) |
                                     (PIN_DP << 10) | (PIN_DP << 5) | PIN_DP;
    REG(SM_INSTR(TX_PIO, TX_SM)) = I_SET_PINS_J;
    sm_start_at(TX_PIO, TX_SM, 0);

    /* Decoder: full speed, jmp pin D+, 8-bit autopush shifting right, RX
     * FIFOs joined. Left stopped; rx_prepare()/rx_start() run it. */
    REG(SM_CLKDIV(RX_PIO, DEC_SM)) = 1u << 16;
    REG(SM_EXECCTRL(RX_PIO, DEC_SM)) = (PIN_DP << 24) | ((DEC_OFF + 14u) << 12) | (DEC_OFF << 7);
    REG(SM_SHIFTCTRL(RX_PIO, DEC_SM)) = (1u << 31) | (8u << 20) | (1u << 18) | (1u << 16);
    REG(SM_PINCTRL(RX_PIO, DEC_SM)) = PIN_DP << 15;
    sm_start_at(RX_PIO, DEC_SM, DEC_OFF);
    REG(SM_INSTR(RX_PIO, DEC_SM)) = I_SET_X_0;
    REG(SM_INSTR(RX_PIO, DEC_SM)) = I_MOV_OSR_NOTNULL;

    /* Edge detector: 96 MHz (1.5 at 144 MHz: fractional, as in the
     * reference, whose own clock makes it 1.25), jmp pin D-, in pin D+. */
    {
        uint64_t div256 = ((uint64_t)CONFIG_CLK_SYS_HZ * 256u) / 96000000u;
        REG(SM_CLKDIV(RX_PIO, EDGE_SM)) = (uint32_t)((div256 >> 8) << 16) | (uint32_t)((div256 & 0xffu) << 8);
    }
    REG(SM_EXECCTRL(RX_PIO, EDGE_SM)) = (PIN_DM << 24) | ((EDGE_OFF + 9u) << 12) | ((EDGE_OFF + 3u) << 7);
    REG(SM_SHIFTCTRL(RX_PIO, EDGE_SM)) = 8u << 20;
    REG(SM_PINCTRL(RX_PIO, EDGE_SM)) = PIN_DP << 15;
    sm_start_at(RX_PIO, EDGE_SM, EDGE_OFF + 1u);

    REG(PIO_CTRL(TX_PIO) + ALIAS_SET) = 1u << TX_SM;
    REG(PIO_CTRL(RX_PIO) + ALIAS_SET) = 1u << EDGE_SM;

    if (!smp_launch_core1_engine()) {
        printk("[USB] core 1 did not answer the launch handshake; no USB host\n");
        return -1;
    }
    uint64_t deadline = time_get_us() + 100000;
    while (!ST->running && time_get_us() < deadline) { }
    if (!ST->running) {
        printk("[USB] core 1 launched but the engine never started\n");
        return -1;
    }
    g_inited = true;
    printk("[USB] PIO-USB host on GP%u/%u, engine on core 1 (PIO0 TX, PIO1 RX)\n",
           (unsigned)CONFIG_PIOUSB_DP_GPIO, (unsigned)CONFIG_PIOUSB_DM_GPIO);
    return 0;
}

int piousb_xfer(piousb_xfer_t *x, uint32_t timeout_us) {
    if (!g_inited || !ST->running) return x->status = PIOUSB_ENGINE;
    if (g_ring.owner != PIOUSB_OWNER_KERNEL) return x->status = PIOUSB_BUSY;
    ylock_acquire(&g_xfer_lock);
    uint64_t deadline = time_get_us() + timeout_us;
    /* A slot is free once its previous transaction completed -- including
     * one an earlier caller gave up waiting for. */
    uint32_t ticket = g_ring.submitted;
    while (ticket - g_ring.completed >= RING) {
        if (time_get_us() >= deadline) {
            ylock_release(&g_xfer_lock);
            return x->status = PIOUSB_ENGINE;
        }
        sched_yield();
    }
    piousb_xfer_t *s = &g_ring.slot[ticket & (RING - 1u)];
    memcpy(s, x, sizeof(*s));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_ring.submitted = ticket + 1u;
    while ((int32_t)(g_ring.completed - ticket) <= 0) {
        if (time_get_us() >= deadline) {
            ylock_release(&g_xfer_lock);
            return x->status = PIOUSB_ENGINE;
        }
        sched_yield();
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    memcpy(x, s, sizeof(*x));
    ylock_release(&g_xfer_lock);
    return x->status;
}

void piousb_status(piousb_status_t *st) {
    memcpy(st, (const void *)&g_st, sizeof(*st));
    st->core1_stack_used = smp_core1_stack_used();
}

static const char *status_name(unsigned s) {
    static const char *const names[] = { "ok", "NAK", "STALL", "timeout", "CRC error",
                                         "bad PID", "no device", "toggle mismatch", "engine", "busy: the kbd task owns the port" };
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

/* A control read at `addr`: SETUP, IN data stage (DATA1, DATA0, ...), and an
 * OUT zero-length status stage. NAKs are retried for up to a second. */
static int control_in(uint8_t addr, const uint8_t setup[8], uint8_t *buf, uint16_t want,
                      uint8_t mps, uint16_t *got) {
    piousb_xfer_t x;
    int st = PIOUSB_TIMEOUT;
    *got = 0;
    for (int tries = 0; tries < 3; tries++) {
        memset(&x, 0, sizeof(x));
        x.op = PIOUSB_OP_SETUP; x.addr = addr; x.len = 8;
        memcpy(x.data, setup, 8);
        st = piousb_xfer(&x, 100000);
        if (st == PIOUSB_OK) break;
    }
    if (st != PIOUSB_OK) return st;
    bool toggle = true;
    for (int naks = 0; *got < want && naks < 1000;) {
        memset(&x, 0, sizeof(x));
        x.op = PIOUSB_OP_IN; x.addr = addr; x.data1 = toggle;
        st = piousb_xfer(&x, 100000);
        if (st == PIOUSB_NAK || st == PIOUSB_TIMEOUT || st == PIOUSB_TOGGLE || st == PIOUSB_CRC) {
            naks++;
            continue;
        }
        if (st != PIOUSB_OK) return st;
        uint16_t n = x.rx_len;
        if (n > want - *got) n = (uint16_t)(want - *got);
        memcpy(buf + *got, x.data, n);
        *got = (uint16_t)(*got + n);
        toggle = !toggle;
        if (x.rx_len < mps) break;              /* short packet: the end */
    }
    for (int naks = 0; naks < 1000; naks++) {
        memset(&x, 0, sizeof(x));
        x.op = PIOUSB_OP_OUT; x.addr = addr; x.data1 = 1; x.len = 0;
        st = piousb_xfer(&x, 100000);
        if (st != PIOUSB_NAK && st != PIOUSB_TIMEOUT) break;
    }
    return st;
}

void piousb_probe(void) {
    piousb_status_t s;
    piousb_status(&s);
    if (!g_inited || !s.running) {
        cprintf("usbprobe: the USB engine is not running\n");
        return;
    }
    if (!s.connected) {
        cprintf("usbprobe: nothing attached to the PIO-USB port (GP%u/%u)\n",
                (unsigned)CONFIG_PIOUSB_DP_GPIO, (unsigned)CONFIG_PIOUSB_DM_GPIO);
        return;
    }
    piousb_xfer_t x;
    memset(&x, 0, sizeof(x));
    x.op = PIOUSB_OP_RESET;
    int st = piousb_xfer(&x, 200000);
    if (st == PIOUSB_BUSY) {
        cprintf("usbprobe: the kbd task owns the port -- `kbd` shows what it found\n");
        return;
    }
    if (st != PIOUSB_OK) {
        cprintf("usbprobe: bus reset: %s (%s-speed device)\n", status_name((unsigned)st),
                x.speed ? "full" : "low");
        return;
    }
    task_sleep_ms(20);                          /* reset recovery, SOFs running */

    static const uint8_t get_dev8[8]  = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00 };
    static const uint8_t get_dev18[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00 };
    uint8_t d[18];
    uint16_t got = 0;
    st = control_in(0, get_dev8, d, 8, 8, &got);
    if (st != PIOUSB_OK || got < 8) {
        cprintf("usbprobe: GET_DESCRIPTOR(device, 8) at address 0: %s, %u bytes\n",
                status_name((unsigned)st), (unsigned)got);
        /* Is the device answering anything at all? An IN to its control
         * endpoint must get a NAK, a STALL or data from any live device. */
        memset(&x, 0, sizeof(x));
        x.op = PIOUSB_OP_IN; x.addr = 0; x.data1 = 1;
        int in = piousb_xfer(&x, 100000);
        piousb_status(&s);
        cprintf("usbprobe: a bare IN to address 0: %s (PID 0x%02x); RX flags 0x%02lx, %lu bytes\n",
                status_name((unsigned)in), x.pid, (unsigned long)s.dbg_rx_flags,
                (unsigned long)s.dbg_rx_bytes);
        return;
    }
    uint8_t mps = d[7];
    st = control_in(0, get_dev18, d, 18, mps, &got);
    if (st != PIOUSB_OK || got < 18) {
        cprintf("usbprobe: GET_DESCRIPTOR(device, 18) at address 0: %s, %u bytes\n",
                status_name((unsigned)st), (unsigned)got);
        return;
    }
    cprintf("usbprobe: device descriptor:");
    for (unsigned i = 0; i < 18; i++) cprintf(" %02x", d[i]);
    cprintf("\nusbprobe: USB %x.%02x, class %02x/%02x/%02x%s, VID:PID %04x:%04x, bcdDevice %x.%02x\n",
            d[3], d[2], d[4], d[5], d[6], d[4] == 0x09 ? " (hub)" : "",
            (unsigned)(d[8] | (d[9] << 8)), (unsigned)(d[10] | (d[11] << 8)), d[13], d[12]);
    cprintf("usbprobe: bMaxPacketSize0 %u, %u configuration(s), full speed\n", mps, d[17]);
    piousb_status(&s);
    cprintf("usbprobe: turnaround max %lu cycles (%lu ns) from a DATA packet's EOP to our ACK\n",
            (unsigned long)s.turnaround_max_cycles,
            (unsigned long)(s.turnaround_max_cycles * 1000u / CPU_MHZ));
}

void piousb_loopback(void) {
    static const uint8_t get_dev8[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00 };
    piousb_xfer_t x;
    memset(&x, 0, sizeof(x));
    x.op = PIOUSB_OP_LOOPBACK;
    x.addr = 0x15; x.ep = 0xe;          /* the USB CRC whitepaper's token */
    int st = piousb_xfer(&x, 200000);
    cprintf("usbprobe loop: %s, %u bytes read back:", status_name((unsigned)st), (unsigned)x.rx_len);
    for (unsigned i = 0; i < x.rx_len; i++) cprintf(" %02x", x.data[i]);
    cprintf("  (RX flags 0x%02x; sent 80 2d 15 ef)\n", x.pid);

    memset(&x, 0, sizeof(x));
    x.op = PIOUSB_OP_LOOPBACK;
    x.len = 8;
    memcpy(x.data, get_dev8, 8);
    st = piousb_xfer(&x, 200000);
    cprintf("usbprobe loop: SETUP stage to address 0: %s, %u bytes on the wire:", status_name((unsigned)st),
            (unsigned)x.rx_len);
    for (unsigned i = 0; i < x.rx_len; i++) cprintf(" %02x", x.data[i]);
    cprintf("\nusbprobe loop: sent 80 2d 00 10 | 80 c3 80 06 00 01 00 00 08 00 eb 94 | then the device's handshake"
            " (prefill took %u bytes)\n", x.addr);

    memset(&x, 0, sizeof(x));
    x.op = PIOUSB_OP_LOOPBACK;
    x.addr = 0x15; x.ep = 0xe;
    x.low_speed = 1;
    st = piousb_xfer(&x, 200000);
    cprintf("usbprobe loop: low speed (PRE, then the token at 1.5 Mbit/s): %s, %u bytes:", status_name((unsigned)st),
            (unsigned)x.rx_len);
    for (unsigned i = 0; i < x.rx_len; i++) cprintf(" %02x", x.data[i]);
    cprintf("  (want 80 2d 15 ef)\n");

    memset(&x, 0, sizeof(x));
    x.op = PIOUSB_OP_LOOPBACK;
    x.len = 8;
    x.low_speed = 1;
    memcpy(x.data, get_dev8, 8);
    st = piousb_xfer(&x, 200000);
    cprintf("usbprobe loop: low-speed SETUP stage to address 0: %s, %u bytes:", status_name((unsigned)st),
            (unsigned)x.rx_len);
    for (unsigned i = 0; i < x.rx_len; i++) cprintf(" %02x", x.data[i]);
    cprintf("\nusbprobe loop: (our token, ee, then the device's reply -- 80 d2 is an ACK)\n");
}

int piousb_proc_render(char *buf, uint32_t cap) {
    piousb_status_t s;
    piousb_status(&s);
    return ksnprintf(buf, cap,
        "running=%u\nconnected=%u\nenabled=%u\nspeed=%s\nsof_count=%lu\nframe=%lu\n"
        "attaches=%lu\ndetaches=%lu\nresets=%lu\nxfers=%lu\nacks=%lu\nnaks=%lu\n"
        "stalls=%lu\ntimeouts=%lu\ncrc_errors=%lu\npid_errors=%lu\ntoggles=%lu\n"
        "turnaround_max_cycles=%lu\nlate_frames=%lu\ntx_stuck=%lu\nloops=%lu\ncore1_stack_used=%lu\n"
        "rx_eop_misses=%lu\nrx_eop_wait_max_cycles=%lu\ndbg_rx_flags=0x%02lx\ndbg_rx_bytes=%lu\ndbg_reset_line=%lu\n",
        s.running, s.connected, s.enabled, s.connected ? (s.full_speed ? "full" : "low") : "none",
        (unsigned long)s.sof_count, (unsigned long)s.frame,
        (unsigned long)s.attaches, (unsigned long)s.detaches, (unsigned long)s.resets,
        (unsigned long)s.xfers, (unsigned long)s.acks, (unsigned long)s.naks,
        (unsigned long)s.stalls, (unsigned long)s.timeouts, (unsigned long)s.crc_errors,
        (unsigned long)s.pid_errors, (unsigned long)s.toggles,
        (unsigned long)s.turnaround_max_cycles, (unsigned long)s.late_frames,
        (unsigned long)g_tx_stuck, (unsigned long)s.loops, (unsigned long)s.core1_stack_used,
        (unsigned long)s.rx_eop_misses, (unsigned long)s.rx_eop_wait_max_cycles,
        (unsigned long)s.dbg_rx_flags, (unsigned long)s.dbg_rx_bytes, (unsigned long)s.dbg_reset_line);
}

#else /* no PIO-USB port on this board */

int piousb_init(void) { return -1; }
int piousb_xfer(piousb_xfer_t *x, uint32_t timeout_us) { (void)timeout_us; return x->status = PIOUSB_ENGINE; }
void piousb_status(piousb_status_t *st) { memset(st, 0, sizeof(*st)); }
piousb_shared_t *piousb_shared(void) { return NULL; }
void piousb_shared_region(uintptr_t *base, uintptr_t *size) { *base = 0; *size = 0; }
void piousb_probe(void) { cprintf("usbprobe: this board has no PIO-USB host port\n"); }
void piousb_loopback(void) { piousb_probe(); }
int piousb_proc_render(char *buf, uint32_t cap) { return ksnprintf(buf, cap, "running=0\n"); }

#endif
