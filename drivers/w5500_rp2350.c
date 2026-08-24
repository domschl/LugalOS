/*
 * WIZnet W5500 on SPI0 — N4, plan/phase18_networking_and_auth.md.
 * See drivers/include/drivers/w5500.h for what this is and why the phase has
 * no IP stack in it.
 *
 * Register access is the W5500's variable-length data mode: every transfer is
 * a three-byte header -- 16-bit address, then a control byte carrying the
 * block select, the read/write bit and the mode -- followed by as many data
 * bytes as CS stays low for. The block select is what makes the address space
 * navigable: one block for the common registers, and three per socket
 * (registers, TX buffer, RX buffer).
 */

#include "drivers/w5500.h"
#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/irq.h"
#include "kernel/sched.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "lugalos_config.h"

#include <string.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_W5500_SCK_GPIO)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RESETS_BASE             0x40020000UL
#define RESETS_RESET            (RESETS_BASE + 0x0)
#define RESETS_RESET_CLR        (RESETS_BASE + 0x3000 + 0x0)
#define RESETS_RESET_DONE       (RESETS_BASE + 0x8)
#define RESETS_SPI0_BIT         (1u << 18)

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)
#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_IN             (SIO_BASE + 0x004)
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)
#define SIO_GPIO_OE_CLR         (SIO_BASE + 0x040)

/* Phase 17b's lesson, applied before it can cost anything this time: RP2350
 * gates peripherals to Secure-privileged by default, upstream of PMP, and a
 * U-mode task cannot grant its way around it. SPI0 and the pins this driver
 * drives are opened here, from M-mode, at init -- so the U-mode conversion
 * this driver is headed for does not begin with a load access fault on its
 * first register read. */
#define ACCESSCTRL_BASE          0x40060000UL
#define ACCESSCTRL_GPIO_NSMASK0  (ACCESSCTRL_BASE + 0x0c)
#define ACCESSCTRL_SPI0          (ACCESSCTRL_BASE + 0x90)
#define ACCESSCTRL_NSP           (1u << 1)
#define ACCESSCTRL_NSU           (1u << 0)
#define ACCESSCTRL_WRITE_PASSWORD 0xacce0000UL

#define SPI0_BASE               ((uintptr_t)CONFIG_SPI0_BASE)
#define SSPCR0                  (SPI0_BASE + 0x00)
#define SSPCR1                  (SPI0_BASE + 0x04)
#define SSPDR                   (SPI0_BASE + 0x08)
#define SSPSR                   (SPI0_BASE + 0x0C)
#define SSPCPSR                 (SPI0_BASE + 0x10)

#define CS_PIN   CONFIG_W5500_CS_GPIO
#define RST_PIN  CONFIG_W5500_RST_GPIO
#define INT_PIN  CONFIG_W5500_INT_GPIO
#define CS_MASK  (1u << CS_PIN)
#define RST_MASK (1u << RST_PIN)

/* --- W5500 register map (datasheet §3) --------------------------------- */

/* Block select values, already shifted into the control byte's position. The
 * socket blocks are (n*4 + k) << 3 with k = 1 registers, 2 TX, 3 RX. */
#define BSB_COMMON        (0x00u << 3)
#define BSB_SOCK_REG(n)   ((uint8_t)(((n) * 4u + 1u) << 3))
#define BSB_SOCK_TX(n)    ((uint8_t)(((n) * 4u + 2u) << 3))
#define BSB_SOCK_RX(n)    ((uint8_t)(((n) * 4u + 3u) << 3))
#define CTRL_WRITE        0x04u   /* RWB */
#define CTRL_VDM          0x00u   /* OM = 00: length set by how long CS is low */

/* Common registers */
#define W5500_MR        0x0000  /* mode; bit 7 = software reset */
#define W5500_GAR       0x0001  /* gateway address, 4 */
#define W5500_SUBR      0x0005  /* subnet mask, 4 */
#define W5500_SHAR      0x0009  /* MAC, 6 */
#define W5500_SIPR      0x000F  /* our IP, 4 */
#define W5500_PHYCFGR   0x002E  /* bit 0 = link up */
#define W5500_VERSIONR  0x0039  /* always 0x04 on a W5500 */

/* Socket registers */
#define Sn_MR           0x0000
#define Sn_CR           0x0001
#define Sn_IR           0x0002
#define Sn_SR           0x0003
#define Sn_PORT         0x0004
#define Sn_RXBUF_SIZE   0x001E
#define Sn_TXBUF_SIZE   0x001F
#define Sn_TX_FSR       0x0020
#define Sn_TX_RD        0x0022
#define Sn_TX_WR        0x0024
#define Sn_RX_RSR       0x0026
#define Sn_RX_RD        0x0028

#define Sn_MR_TCP       0x01
#define Sn_CR_OPEN      0x01
#define Sn_CR_LISTEN    0x02
#define Sn_CR_DISCON    0x08
#define Sn_CR_CLOSE     0x10
#define Sn_CR_SEND      0x20
#define Sn_CR_RECV      0x40

#define Sn_SR_CLOSED      0x00
#define Sn_SR_INIT        0x13
#define Sn_SR_LISTEN      0x14
#define Sn_SR_ESTABLISHED 0x17
#define Sn_SR_CLOSE_WAIT  0x1C

/* 564 is the port IANA assigns to the Plan 9 file service. */
#define W5500_9P_PORT 564
#define SOCK_9P       0
/* Must match what w5500_socket_memory() allocates. */
#define W5500_SOCK_BUF_BYTES 8192u

/* --- serialising access ------------------------------------------------- */

/* One owner at a time, across a whole OPERATION rather than a single
 * transfer.
 *
 * This driver is reached from two tasks: `p9srv`, which polls the link
 * continuously, and the shell, whose `net`/`net-config` touch the same
 * registers. The kernel preempts at 100 Hz, so without this the two
 * interleave -- and not only mid-transfer, with CS dropped underneath a
 * three-byte header, but between the register reads and writes that make up
 * one socket operation. Read Sn_TX_WR, be preempted, have the other context
 * send, then write back a stale pointer, and the chip's own buffer accounting
 * is wrong from then on.
 *
 * The symptom is worth recording because it does not look like a race: the
 * board stops answering **ping** while `net` still reports link UP and the
 * socket in LISTEN. Nothing in the 9P layer is involved at all -- the chip's
 * networking block has simply been scribbled on (2026-08-24).
 *
 * A flag with a yield rather than a channel: the destination for this driver
 * is a U-mode task with an endpoint (the plan's N4), where serialisation is
 * what a channel already provides for free. This is the honest intermediate,
 * and it is marked as such.
 *
 * Every public entry point takes it; every internal helper assumes it is
 * already held, which is why they are separated below. */
static volatile bool g_busy;

static void w5500_lock(void) {
    for (;;) {
        uintptr_t f = irq_save();
        if (!g_busy) { g_busy = true; irq_restore(f); return; }
        irq_restore(f);
        sched_yield();   /* a no-op before sched_init(), which is when init runs */
    }
}

static void w5500_unlock(void) { g_busy = false; }

/* Internals that assume the lock is already held. */
static bool link_up_locked(void);
static int  w5500_send_locked(const uint8_t *buf, uint32_t len);
static bool cmd_wait(void);

/* --- SPI ---------------------------------------------------------------- */

static void cs_select(void)   { REG(SIO_GPIO_OUT_CLR) = CS_MASK; }
static void cs_deselect(void) { REG(SIO_GPIO_OUT_SET) = CS_MASK; }

static uint8_t spi_xfer(uint8_t tx) {
    int timeout = 100000;
    while (!(REG(SSPSR) & (1u << 1)) && --timeout > 0);   /* TNF: room to send */
    REG(SSPDR) = tx;
    timeout = 100000;
    while (!(REG(SSPSR) & (1u << 2)) && --timeout > 0);   /* RNE: a byte arrived */
    return (uint8_t)REG(SSPDR);
}

/* One VDM transfer: header, then `len` bytes in the chosen direction. CS
 * frames the whole thing, which in VDM is what says how long the data is. */
static void w5500_xfer(uint16_t addr, uint8_t bsb_ctrl, uint8_t *buf, uint32_t len, bool write) {
    cs_select();
    spi_xfer((uint8_t)(addr >> 8));
    spi_xfer((uint8_t)(addr & 0xFF));
    spi_xfer((uint8_t)(bsb_ctrl | (write ? CTRL_WRITE : 0u) | CTRL_VDM));
    for (uint32_t i = 0; i < len; i++) {
        uint8_t out = write ? buf[i] : 0xFF;
        uint8_t in = spi_xfer(out);
        if (!write) buf[i] = in;
    }
    cs_deselect();
}

static uint8_t rd8(uint16_t addr, uint8_t bsb) {
    uint8_t v = 0;
    w5500_xfer(addr, bsb, &v, 1, false);
    return v;
}
static void wr8(uint16_t addr, uint8_t bsb, uint8_t v) {
    w5500_xfer(addr, bsb, &v, 1, true);
}
static uint16_t rd16(uint16_t addr, uint8_t bsb) {
    uint8_t v[2] = { 0, 0 };
    w5500_xfer(addr, bsb, v, 2, false);
    return (uint16_t)((v[0] << 8) | v[1]);
}
static void wr16(uint16_t addr, uint8_t bsb, uint16_t val) {
    uint8_t v[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    w5500_xfer(addr, bsb, v, 2, true);
}

/* Sn_RX_RSR and Sn_TX_FSR are updated by hardware while being read, so a
 * single read can catch them mid-change. The datasheet's own remedy: read
 * until two consecutive reads agree. */
static bool rd16_stable(uint16_t addr, uint8_t bsb, uint16_t *out) {
    uint16_t a = rd16(addr, bsb);
    for (int i = 0; i < 8; i++) {
        uint16_t b = rd16(addr, bsb);
        if (a == b) { *out = a; return true; }
        a = b;
    }
    /* Eight reads and it never settled. The old version returned the last
     * value anyway, which is the one thing that must not happen: a torn
     * Sn_RX_RSR is not a small error, it is a number that then advances
     * Sn_RX_RD past the data and turns every subsequent read into stale
     * buffer. Saying "not now" costs one poll -- the next is microseconds
     * away -- and says nothing false. */
    *out = a;
    return false;
}

/* --- state -------------------------------------------------------------- */

static bool     g_present;
static bool     g_have_address;
static uint8_t  g_mac[6]  = { 0x02, 0x4C, 0x47, 0x00, 0x00, 0x01 };  /* 02:LG:... */
static uint8_t  g_ip[4], g_mask[4], g_gw[4];
static uint32_t g_rx_bytes, g_tx_bytes, g_accepts;
static bool     g_phy_forced_10bt;
static bool     g_reset_by_pin, g_reset_by_sw;
static bool     g_connected;
static uint32_t g_frames_in, g_frames_out, g_resyncs, g_cmd_timeouts, g_rx_overruns;
static uint32_t g_rsr_unstable;
static bool     g_debug;
static uint8_t  g_last_sr = 0xFF;

/* Defined with its callbacks further down; declared here because
 * w5500_set_address() registers it for background service and sits above. */
static p9_link_t g_w5500_link;

static void spi0_init(void) {
    REG(RESETS_RESET_CLR) = RESETS_SPI0_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_SPI0_BIT) && --timeout > 0);

    REG(IO_BANK0_CTRL(CONFIG_W5500_SCK_GPIO))  = 1;   /* F1 = SPI0 */
    REG(IO_BANK0_CTRL(CONFIG_W5500_MOSI_GPIO)) = 1;
    REG(IO_BANK0_CTRL(CONFIG_W5500_MISO_GPIO)) = 1;
    REG(PADS_BANK0_PAD(CONFIG_W5500_SCK_GPIO))  = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_W5500_MOSI_GPIO)) = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_W5500_MISO_GPIO)) = 0x5A;

    /* CS and RST as outputs, INT as an input (polled -- see the plan's N4). */
    REG(IO_BANK0_CTRL(CS_PIN)) = 5;                   /* F5 = SIO */
    REG(PADS_BANK0_PAD(CS_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = CS_MASK;
    cs_deselect();

    REG(IO_BANK0_CTRL(RST_PIN)) = 5;
    REG(PADS_BANK0_PAD(RST_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = RST_MASK;
    REG(SIO_GPIO_OUT_SET) = RST_MASK;                 /* released; active low */

    REG(IO_BANK0_CTRL(INT_PIN)) = 5;
    REG(PADS_BANK0_PAD(INT_PIN)) = 0x5A;              /* input, pull-up */
    REG(SIO_GPIO_OE_CLR) = (1u << INT_PIN);

    REG(ACCESSCTRL_GPIO_NSMASK0) |= (CS_MASK | RST_MASK | (1u << INT_PIN));
    REG(ACCESSCTRL_SPI0) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_SPI0)
                           | ACCESSCTRL_NSP | ACCESSCTRL_NSU;

    /* 150 MHz / (6 * (1+1)) = 12.5 MHz, the same figure the SD card uses on
     * this board.
     *
     * It was 1.25 MHz for a while, on the theory that jumper wires to a module
     * with no series termination were corrupting writes -- the chip worked and
     * then stopped answering ARP, and slowing the bus appeared to help. `net
     * bustest` settled it: 20000 reads and three widths of write, at four
     * clock rates up to 12.5 MHz, with **zero** errors in every column. The
     * bus is clean; the slower clock had only been changing the timing of a
     * logic bug. Restored, and the lesson recorded rather than the workaround
     * kept. */
    REG(SSPCR1) = 0;
    REG(SSPCPSR) = 6;
    REG(SSPCR0) = (1u << 8) | 0x7;                    /* SCR=1, 8-bit, mode 0 */
    REG(SSPCR1) = (1u << 1);                          /* SSE */
}

/* Did the chip actually reset?
 *
 * SIPR is 0.0.0.0 out of reset and this driver writes a non-zero address into
 * it as soon as it has one, so a stale address here means the reset did not
 * happen. At boot the answer is trivially yes -- SIPR is zero either way --
 * and the check is worth nothing; on a re-run of (net-config ...) against a
 * configured board it is worth everything, and that is precisely the path
 * where the chip has been seen to come back wrong. */
static bool w5500_reset_took(void) {
    uint8_t sip[4] = { 0, 0, 0, 0 };
    w5500_xfer(W5500_SIPR, BSB_COMMON, sip, 4, false);
    return (uint8_t)(sip[0] | sip[1] | sip[2] | sip[3]) == 0;
}

/* Reset the chip, and check that it reset.
 *
 * The RSTn pin was the only mechanism here, and a driver that asserts a reset
 * without ever confirming it is asserting a hope. That matters because the
 * chip has a bad state that *survives* this function: a board that reports
 * link UP and LISTEN and answers nothing keeps doing so across repeated
 * (net-config ...) calls, each of which believes it has just reset the part.
 * Either the reset is not reaching the chip -- one wire, one pad, one module's
 * reset circuit -- or it is reaching it and not clearing the state. Those need
 * telling apart, and until now nothing here could.
 *
 * So: pulse RSTn, verify, and if it did not take, use the software reset the
 * datasheet provides for exactly this (MR bit 7), which depends on no board
 * wiring at all. Whichever worked is reported by `net`. */
static void w5500_hw_reset(void) {
    /* The module manual says >=2 us low and 150 ms settle; the W5500
     * datasheet says >=500 us and ~1 ms for PLL lock. Take the generous
     * reading of both -- it costs a millisecond at boot, once. */
    REG(SIO_GPIO_OUT_CLR) = RST_MASK;
    time_delay_us(1000);
    REG(SIO_GPIO_OUT_SET) = RST_MASK;
    time_delay_us(150000);

    g_reset_by_pin = w5500_reset_took();
    g_reset_by_sw  = false;
    if (g_reset_by_pin) return;

    wr8(W5500_MR, BSB_COMMON, 0x80);
    time_delay_us(150000);
    g_reset_by_sw = w5500_reset_took();
    printk("[W5500] RSTn pulse did not reset the chip; software reset (MR bit 7) %s\n",
           g_reset_by_sw ? "did" : "did NOT either -- the part is not answering reset at all");
}

/* Configure the PHY for auto-negotiation, explicitly, rather than trusting
 * whatever the module's strap pins happen to say.
 *
 * PHYCFGR's reset value takes its operating mode from hardware pins (OPMD=0),
 * which on a third-party module is a fact about that module's layout and not
 * something this driver can check. Setting OPMD=1 with OPMDC=111 says
 * "all capable, auto-negotiation enabled" in the register, where it is
 * visible and the same on every board. The PHY is then reset (RST is active
 * LOW in this register, which is the opposite of the pin) so the new mode is
 * actually adopted.
 *
 * Auto-negotiation itself then takes a second or more -- which is why the
 * boot-time link report is not the last word, and why `net` exists. */
static void w5500_phy_set(uint8_t opmdc) {
    uint8_t cfg = (uint8_t)(0x40u | (uint8_t)(opmdc << 3));   /* OPMD=1, RST low */
    wr8(W5500_PHYCFGR, BSB_COMMON, cfg);
    time_delay_us(1000);
    wr8(W5500_PHYCFGR, BSB_COMMON, (uint8_t)(cfg | 0x80u));   /* release RST */
    time_delay_us(1000);
}

/* Auto-negotiation first, then 10BT half-duplex if it does not take.
 *
 * Measured on this hardware, 2026-08-24: with OPMDC=111 (all capable,
 * auto-negotiation) the link never comes up -- not in three seconds, not in
 * forty -- and the switch shows no light on that port at all. Forced to
 * OPMDC=000 (10BT half-duplex, no negotiation) it links in about 2.5 s, and
 * PHYCFGR then reads its speed bit back as 10 Mbps even when 100BT is asked
 * for, which says the 100BT path on this module is not usable rather than
 * that the request was ignored.
 *
 * `net watch` ruled out the alternative explanation before this was written:
 * 8 seconds with zero PHYCFGR changes and zero bad VERSIONR reads, so the
 * chip is not browning out under the PHY's current and reverting its own
 * configuration. It is negotiation, on this module, with this switch.
 *
 * 10 Mbit is not a constraint worth minding here. A 2 KB msize over SPI at
 * 12.5 MHz, through a kernel that copies every frame, is nowhere near 10
 * Mbit -- the wire stopped being the bottleneck long before this.
 *
 * Auto is still tried first, and for a reason: the next module, or the next
 * switch, may negotiate perfectly, and hard-coding around one board's
 * behaviour would hide that. What is reported is which mode actually took. */
static bool w5500_phy_bring_up(void) {
    w5500_phy_set(7);                                  /* all capable, auto-neg */
    for (int i = 0; i < 30 && !link_up_locked(); i++) time_delay_us(100000);
    if (link_up_locked()) { g_phy_forced_10bt = false; return true; }

    w5500_phy_set(0);                                  /* 10BT half, no auto-neg */
    for (int i = 0; i < 50 && !link_up_locked(); i++) time_delay_us(100000);
    g_phy_forced_10bt = link_up_locked();
    return g_phy_forced_10bt;
}

static void w5500_apply_identity(void) {
    w5500_xfer(W5500_SHAR, BSB_COMMON, g_mac, 6, true);
    if (g_have_address) {
        w5500_xfer(W5500_SIPR, BSB_COMMON, g_ip, 4, true);
        w5500_xfer(W5500_SUBR, BSB_COMMON, g_mask, 4, true);
        w5500_xfer(W5500_GAR,  BSB_COMMON, g_gw, 4, true);
    }
}

/* Socket 0 into TCP LISTEN on 564. Idempotent: called at init and again
 * whenever the peer goes away, which is the whole of this driver's
 * connection management. */
/* Socket memory, allocated ONCE after a hardware reset and never touched
 * again.
 *
 * Two things learned the hard way on 2026-08-24, each of which cost a working
 * board:
 *
 * 1. The other seven sockets must be surrendered BEFORE socket 0 is given its
 *    share. Each defaults to 2 KB, so the other order asks the chip for more
 *    than the 16 KB it has -- an allocation it cannot honour and does not
 *    report refusing.
 * 2. This must not run on every re-listen. It used to live in w5500_listen(),
 *    which the background poller reaches whenever the socket reads CLOSED --
 *    so a link with no peer reconfigured the chip's memory map continuously.
 *    The symptom was not a broken socket: the board stopped answering **ping**,
 *    because reallocating socket buffers underneath a running MAC takes the
 *    whole networking block down with it.
 *
 * 8 KB rather than the full 16: four times the msize is ample headroom for a
 * peer that pauses, and leaving half unallocated costs nothing on a chip whose
 * other sockets are unused. */
static void w5500_socket_memory(void) {
    for (unsigned n = 1; n < 8; n++) {
        wr8(Sn_RXBUF_SIZE, BSB_SOCK_REG(n), 0);
        wr8(Sn_TXBUF_SIZE, BSB_SOCK_REG(n), 0);
    }
    wr8(Sn_RXBUF_SIZE, BSB_SOCK_REG(SOCK_9P), 8);
    wr8(Sn_TXBUF_SIZE, BSB_SOCK_REG(SOCK_9P), 8);
}

/* Put socket 0 into TCP LISTEN on 564. Idempotent and cheap when it already
 * is, which matters because the background poller reaches this on every pass
 * while no peer is attached. */
static void w5500_listen(void) {
    if (rd8(Sn_SR, BSB_SOCK_REG(SOCK_9P)) == Sn_SR_LISTEN) return;

    wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_CLOSE);
    cmd_wait();

    wr8(Sn_MR, BSB_SOCK_REG(SOCK_9P), Sn_MR_TCP);
    wr16(Sn_PORT, BSB_SOCK_REG(SOCK_9P), W5500_9P_PORT);
    wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_OPEN);
    cmd_wait();

    if (rd8(Sn_SR, BSB_SOCK_REG(SOCK_9P)) != Sn_SR_INIT) return;

    wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_LISTEN);
    cmd_wait();
}

int w5500_init(void) {
    spi0_init();
    w5500_hw_reset();

    uint8_t ver = rd8(W5500_VERSIONR, BSB_COMMON);
    g_present = (ver == 0x04);
    if (!g_present) {
        /* The single most useful line this driver can print. 0x00 or 0xFF is
         * "the bus is not working" -- wiring, power, or an ACCESSCTRL window
         * that was never opened -- and anything else is a part that is not a
         * W5500. Either way the answer is not in software above this layer. */
        printk("[W5500] VERSIONR read 0x%02x, expected 0x04 -- SPI0 bus not talking to a W5500\n"
               "        (check MISO=GP%d MOSI=GP%d SCK=GP%d CS=GP%d, and 3V3 at >=200 mA)\n",
               ver, CONFIG_W5500_MISO_GPIO, CONFIG_W5500_MOSI_GPIO,
               CONFIG_W5500_SCK_GPIO, CONFIG_W5500_CS_GPIO);
        return -1;
    }

    /* Order matters: **the PHY comes up before the identity is written.**
     *
     * This is measured, not reasoned. Putting the PHY bring-up last -- so that
     * the MAC is fully configured before the link ever comes up, which reads
     * like the more careful order -- makes the board dead 8 times out of 8.
     * This order is the one that works.
     *
     * It is not yet reliable, and saying so is the point of this comment. The
     * open failure is a chip that reports link UP, socket LISTEN and every
     * pointer sane over a bus `net bustest` proves clean, and answers nothing
     * at all -- not ARP, not ping. Nothing looks wrong from the inside; the
     * board is simply invisible. It survives `net phy`, and only a power cycle
     * or a lucky re-run of (net-config ...) clears it. See the plan's N4 notes
     * for the measurements; what is known is that the trigger is somewhere in
     * this bring-up and not in the SPI bus, the socket buffers, or the 9P
     * layer above, all of which have been eliminated.
     *
     * Bounded: a gateway with no cable must still finish booting. Worst case
     * is 3 s of auto-negotiation plus 5 s of forced 10BT, and `net` is the
     * live answer afterwards either way. */
    bool up = w5500_phy_bring_up();

    w5500_socket_memory();
    w5500_apply_identity();
    if (g_have_address) w5500_listen();

    printk("[W5500] Ethernet controller present (VERSIONR 0x04), PHYCFGR 0x%02x, link %s%s\n",
           rd8(W5500_PHYCFGR, BSB_COMMON), up ? "UP" : "down",
           (up && g_phy_forced_10bt) ? " (forced 10BT half -- auto-negotiation did not link)" : "");
    if (!g_have_address) {
        /* Not an error, and not a default address either: see the plan's §3.
         * A board that picks an address it was not given causes a conflict on
         * a network it knows nothing about. */
        printk("[W5500] No address configured -- staying off the network. Set one with\n"
               "        (net-config \"ip\" \"mask\" \"gateway\") in /sd0/system/etc/usr_init.lisp\n");
    }
    return 0;
}

bool w5500_present(void) { return g_present; }

/* Unlocked: for callers that already hold the lock. */
static bool link_up_locked(void) {
    if (!g_present) return false;
    return (rd8(W5500_PHYCFGR, BSB_COMMON) & 0x01u) != 0;
}

bool w5500_link_up(void) {
    if (!g_present) return false;
    w5500_lock();
    bool up = link_up_locked();
    w5500_unlock();
    return up;
}

bool w5500_have_address(void) { return g_have_address; }

void w5500_set_mac(const uint8_t mac[6]) {
    for (unsigned i = 0; i < 6; i++) g_mac[i] = mac[i];
    if (g_present) w5500_apply_identity();
}

int w5500_set_address(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]) {
    w5500_lock();
    for (unsigned i = 0; i < 4; i++) { g_ip[i] = ip[i]; g_mask[i] = mask[i]; g_gw[i] = gw[i]; }
    g_have_address = true;
    if (!g_present) { w5500_unlock(); return -1; }

    /* A full hardware reset, not just a rewrite of SIPR/SHAR.
     *
     * Writing the source address registers while a socket is open leaves the
     * chip in a state where it reports everything correctly -- link UP, socket
     * LISTEN, sane pointers -- and answers nothing at all, not even ARP. The
     * datasheet's own order is configure-then-open, and this is what that
     * costs when it is not followed: a board that looks healthy from the
     * inside and is invisible from the outside (2026-08-24, several times
     * before the pattern was recognised).
     *
     * Reset, re-configure, re-open. It costs about a second and it is the
     * only way this command can be safe to run twice, which -- since it comes
     * from a config file that a user will edit and re-run -- it must be. */
    /* Same order as init, for the same reason: reset, configure the MAC while
     * the link is still down, and bring the PHY up last. See w5500_init(). */
    w5500_hw_reset();
    (void)w5500_phy_bring_up();
    w5500_socket_memory();
    w5500_apply_identity();
    w5500_listen();

    /* Register for background service HERE, not at boot.
     *
     * kernel_main() walks the device registry once, early, and registers every
     * DEV_F_BACKGROUND_9P link it finds -- but this link only exists once the
     * board has an address, and the address arrives later, from
     * /sd0/system/etc/usr_init.lisp. So at boot w5500_get_link() answers NULL,
     * the walk skips it, and nothing ever polls the socket: TCP connects
     * (the chip accepts in hardware) and then every request goes unanswered,
     * which is precisely how this presented (2026-08-24).
     *
     * Re-registering is a no-op, so calling net-config twice is harmless. */
    w5500_unlock();
    p9_link_register_background(&g_w5500_link);
    return 0;
}

static const char *sock_state_name(uint8_t sr) {
    switch (sr) {
    case Sn_SR_CLOSED:      return "CLOSED";
    case Sn_SR_INIT:        return "INIT";
    case Sn_SR_LISTEN:      return "LISTEN (waiting for a client)";
    case Sn_SR_ESTABLISHED: return "ESTABLISHED (peer connected)";
    case Sn_SR_CLOSE_WAIT:  return "CLOSE_WAIT";
    default:                return "other";
    }
}

/* Force a PHY mode, or watch the link.
 *
 * Bring-up tools, and the reason they are in the driver rather than in a
 * scratch script: when a link will not come up, the useful experiments are
 * "does it link if auto-negotiation is taken out of the equation" and "is the
 * chip holding the mode I set or resetting under me", and both want to be one
 * command rather than one reflash.
 *
 * OPMDC (PHYCFGR[5:3]) per the W5500 datasheet:
 *   000 10BT half, no auto-neg      100 100BT half, auto-neg
 *   001 10BT full, no auto-neg      110 power down
 *   010 100BT half, no auto-neg     111 all capable, auto-neg  <- the default here
 *   011 100BT full, no auto-neg
 *
 * 10BT half-duplex without auto-negotiation is the mode that asks least of
 * the cable, the magjack and the link partner; if nothing links there,
 * the problem is not negotiation. */
int w5500_phy_mode(const char *mode) {
    if (!g_present) { cprintf("net: no W5500 present\n"); return -1; }
    w5500_lock();

    if (mode && strcmp(mode, "retry") == 0) {
        bool up = w5500_phy_bring_up();
        w5500_unlock();
        cprintf("net: %s%s\n", up ? "link UP" : "no link in either mode",
                (up && g_phy_forced_10bt) ? " (forced 10BT half)" : "");
        return up ? 0 : -1;
    }

    uint8_t opmdc;
    if      (!mode || strcmp(mode, "auto") == 0) opmdc = 7;
    else if (strcmp(mode, "100f") == 0)          opmdc = 3;
    else if (strcmp(mode, "100h") == 0)          opmdc = 2;
    else if (strcmp(mode, "10f") == 0)           opmdc = 1;
    else if (strcmp(mode, "10h") == 0)           opmdc = 0;
    else if (strcmp(mode, "down") == 0)          opmdc = 6;
    else {
        cprintf("usage: net phy auto|100f|100h|10f|10h|down|retry\n");
        w5500_unlock();
        return -1;
    }

    w5500_phy_set(opmdc);

    cprintf("net: PHY set to %s, watching for 6 s...\n", mode ? mode : "auto");
    for (int i = 0; i < 12; i++) {
        time_delay_us(500000);
        uint8_t phy = rd8(W5500_PHYCFGR, BSB_COMMON);
        cprintf("  t=%d.%ds  PHYCFGR 0x%02x  link %s  %s  %s\n",
                (i + 1) / 2, ((i + 1) % 2) * 5, phy,
                (phy & 0x01u) ? "UP" : "down",
                (phy & 0x02u) ? "100Mbps" : "10Mbps",
                (phy & 0x04u) ? "full" : "half");
        if (phy & 0x01u) { cprintf("net: link came up\n"); w5500_unlock(); return 0; }
    }
    cprintf("net: no link in that mode\n");
    w5500_unlock();
    return -1;
}

/* Is the chip holding what it was told? A W5500 that browns out under the
 * PHY's current draw resets, and a reset reverts PHYCFGR to whatever the
 * module's strap pins say -- so a value that changes on its own is a power
 * problem wearing a network problem's clothes. VERSIONR is read alongside it
 * because a chip mid-reset does not answer 0x04 either. */
void w5500_debug(bool on) { g_debug = on; g_last_sr = 0xFF; }

/* --- bus integrity ------------------------------------------------------
 *
 * Is the SPI bus actually delivering what it is told to?
 *
 * This exists because "the chip works at 1.25 MHz and wedges at 12.5" is a
 * suggestive observation and not evidence. A slower clock changes the timing
 * of a logic bug just as surely as it changes the electrical margin, so
 * before anyone reaches for a soldering iron, the bus itself should be asked
 * directly -- with no sockets, no pointers and no networking involved.
 *
 * Two measurements, because they fail differently:
 *
 *   reads   VERSIONR is a constant 0x04. Anything else came from the wire,
 *           not the chip.
 *   writes  A pattern into an unused socket's DHAR and straight back out.
 *           This is the one that matters here: a corrupted *write* would
 *           scribble on the chip's state while every read still looked
 *           perfect, which is exactly the symptom under investigation
 *           (link UP, LISTEN, sane pointers, answers nothing).
 *
 * Socket 7 is the scratch: it has no buffer allocated (w5500_socket_memory()
 * gave it none), is never opened, and its DHAR means nothing while closed.
 * Restored to its reset value afterwards anyway.
 *
 * A multi-byte transfer on purpose. A single byte exercises three header
 * bytes and one payload byte; six payload bytes hold CS low longer and are
 * where marginal timing shows up first. */
#define BUSTEST_SCRATCH_SOCK 7
#define Sn_DHAR              0x0006

static void spi_set_clock(uint32_t cpsdvsr, uint32_t scr) {
    REG(SSPCR1) = 0;                                   /* SSE off to reprogram */
    REG(SSPCPSR) = cpsdvsr;
    REG(SSPCR0) = (scr << 8) | 0x7;                    /* 8-bit, mode 0 */
    REG(SSPCR1) = (1u << 1);
}

void w5500_bustest(unsigned iterations) {
    if (!g_present) { cprintf("net: no W5500 present\n"); return; }
    if (iterations == 0 || iterations > 200000u) iterations = 20000u;

    static const struct { uint32_t cps, scr; const char *label; } SPEEDS[] = {
        {  6, 1, "12.50 MHz" },
        { 12, 1, " 6.25 MHz" },
        { 30, 1, " 2.50 MHz" },
        { 60, 1, " 1.25 MHz" },
    };

    w5500_lock();
    cprintf("SPI bus integrity, %u iterations per speed.\n", iterations);
    cprintf("VERSIONR is a constant 0x04; the write test round-trips 6 bytes\n"
            "through an unused socket's DHAR. Any non-zero column is the wire.\n\n");
    cprintf("  speed       read err   GAR(4,common)   PORT(2,sock)   SHAR(6,common)\n");
    cprintf("  ----------  --------   -------------   ------------   ------------\n");

    uint32_t seed = 0x12345678u;
    for (unsigned sp = 0; sp < sizeof(SPEEDS) / sizeof(SPEEDS[0]); sp++) {
        spi_set_clock(SPEEDS[sp].cps, SPEEDS[sp].scr);

        uint32_t read_err = 0, write_err = 0, err_common = 0, err_sockreg = 0;
        for (unsigned i = 0; i < iterations; i++) {
            if (rd8(W5500_VERSIONR, BSB_COMMON) != 0x04) read_err++;

            uint8_t pat[6], back[6];
            for (unsigned k = 0; k < 6; k++) {
                seed = seed * 1103515245u + 12345u;    /* not random, just varied */
                pat[k] = (uint8_t)(seed >> 16);
            }

            /* Three targets, because the first version of this test failed
             * 100% at every speed -- which is not what a marginal wire looks
             * like -- and the useful question became *which* writes fail:
             * a common register, a socket register, or a multi-byte one. */
            w5500_xfer(W5500_GAR, BSB_COMMON, pat, 4, true);
            w5500_xfer(W5500_GAR, BSB_COMMON, back, 4, false);
            for (unsigned k = 0; k < 4; k++) if (back[k] != pat[k]) { err_common++; break; }

            w5500_xfer(Sn_PORT, BSB_SOCK_REG(BUSTEST_SCRATCH_SOCK), pat, 2, true);
            w5500_xfer(Sn_PORT, BSB_SOCK_REG(BUSTEST_SCRATCH_SOCK), back, 2, false);
            for (unsigned k = 0; k < 2; k++) if (back[k] != pat[k]) { err_sockreg++; break; }

            /* Six bytes through SHAR rather than a closed socket's DHAR.
             * DHAR failed 100% at every speed in the first version of this
             * test, which is not a wire and is not a length: it is a register
             * that a closed socket does not keep. SHAR is the MAC -- six
             * bytes, common block, and demonstrably writable, since the board
             * answers ARP with what is written there. Restored below. */
            w5500_xfer(W5500_SHAR, BSB_COMMON, pat, 6, true);
            w5500_xfer(W5500_SHAR, BSB_COMMON, back, 6, false);
            for (unsigned k = 0; k < 6; k++) if (back[k] != pat[k]) { write_err++; break; }
        }
        cprintf("  %s   %8u   %13u   %12u   %12u\n", SPEEDS[sp].label,
                (unsigned)read_err, (unsigned)err_common,
                (unsigned)err_sockreg, (unsigned)write_err);
    }

    /* DHAR back to its reset value, and the bus back to the driver's own
     * speed -- whatever this file currently sets in spi0_init(). */
    {
        uint8_t zero4[4] = { 0, 0, 0, 0 };
        w5500_xfer(Sn_PORT, BSB_SOCK_REG(BUSTEST_SCRATCH_SOCK), zero4, 2, true);
        w5500_xfer(W5500_GAR, BSB_COMMON, g_gw, 4, true);   /* the real gateway back */
        w5500_xfer(W5500_SHAR, BSB_COMMON, g_mac, 6, true); /* and the real MAC */
        (void)zero4;
    }
    spi_set_clock(6, 1);

    cprintf("\nAll zeros: the bus is clean and a wedged chip is a logic bug.\n"
            "Errors that grow with speed: signal integrity -- shorter leads,\n"
            "series resistors on SCK/MOSI, decoupling at the module.\n");
    w5500_unlock();
}

void w5500_watch(unsigned secs) {
    if (!g_present) { cprintf("net: no W5500 present\n"); return; }
    w5500_lock();
    if (secs == 0 || secs > 60) secs = 10;
    uint8_t first = rd8(W5500_PHYCFGR, BSB_COMMON);
    cprintf("net: watching PHYCFGR/VERSIONR for %u s (started at 0x%02x)\n", secs, first);
    unsigned changes = 0, bad_version = 0;
    for (unsigned i = 0; i < secs * 5u; i++) {
        time_delay_us(200000);
        uint8_t phy = rd8(W5500_PHYCFGR, BSB_COMMON);
        uint8_t ver = rd8(W5500_VERSIONR, BSB_COMMON);
        if (ver != 0x04) bad_version++;
        if (phy != first) {
            changes++;
            cprintf("  t=%u.%us  PHYCFGR 0x%02x -> 0x%02x  VERSIONR 0x%02x\n",
                    i / 5u, (i % 5u) * 2u, first, phy, ver);
            first = phy;
        }
    }
    w5500_unlock();
    cprintf("net: %u change(s), %u bad VERSIONR read(s) in %u s -- %s\n",
            changes, bad_version, secs,
            (changes == 0 && bad_version == 0)
                ? "the chip is stable, so a dead link is cable, magjack or switch"
                : "the chip is NOT stable: suspect supply current or a loose 3V3/GND");
}

/* --- the 9P link (p9_link_t over socket 0) ------------------------------ */

/* One frame in flight, assembled here as bytes arrive.
 *
 * 9P frames are length-prefixed -- size[4] little-endian, counting itself --
 * so this needs no framing of its own, which is the whole reason a TCP link
 * and the USB-CDC link can share the server unchanged. What it does need is
 * somewhere to accumulate a frame that arrives in several TCP segments, which
 * over a 10 Mbit link and a 2 KB msize is most of them. */
static uint8_t  g_rx_frame[P9_MAX_MSIZE];
static uint32_t g_rx_have;

static uint32_t frame_len(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Moves whatever the chip has into g_rx_frame. The W5500 keeps the socket's
 * received bytes in its own 16 KB buffer and hands them over by address, so
 * "receive" here is: ask how much is there, read that many bytes from the
 * read pointer, advance the pointer, and tell the chip with a RECV command
 * that the space is free again. */
static void w5500_drain_rx(void) {
    if (g_rx_have >= sizeof(g_rx_frame)) return;

    uint16_t avail;
    if (!rd16_stable(Sn_RX_RSR, BSB_SOCK_REG(SOCK_9P), &avail)) {
        g_rsr_unstable++;
        return;   /* try again on the next poll rather than trust a torn read */
    }
    if (avail == 0) return;

    /* RSR can never exceed the socket's own buffer. When it does, the chip's
     * read pointer and its write pointer have lost each other -- and the
     * consequence of believing it is worse than the corruption that caused
     * it: reading `avail` bytes from a pointer that is already past the data
     * hands the frame parser several kilobytes of stale buffer, every byte of
     * which it discards one at a time while the real request sits behind it.
     * That is what 13229 resync discards for 61 frames looked like
     * (2026-08-24).
     *
     * Dropping the connection is the honest response: the peer will
     * reconnect, and a session that resumes from a known-good LISTEN is worth
     * more than one that limps. */
    if (avail > W5500_SOCK_BUF_BYTES) {
        g_rx_overruns++;
        g_rx_have = 0;
        wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_CLOSE);
        cmd_wait();
        g_connected = false;
        w5500_listen();
        return;
    }

    uint32_t room = (uint32_t)sizeof(g_rx_frame) - g_rx_have;
    uint32_t take = (avail < room) ? avail : room;

    uint16_t rd = rd16(Sn_RX_RD, BSB_SOCK_REG(SOCK_9P));
    w5500_xfer(rd, BSB_SOCK_RX(SOCK_9P), &g_rx_frame[g_rx_have], take, false);
    wr16(Sn_RX_RD, BSB_SOCK_REG(SOCK_9P), (uint16_t)(rd + take));
    wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_RECV);
    cmd_wait();

    g_rx_have += take;
    g_rx_bytes += take;
}

/* Connection management, such as it is: accept by noticing, and re-listen
 * when the peer goes away. A closed connection also drops any half-assembled
 * frame -- the next client's first bytes must not be read as the tail of a
 * previous one's request. */
/* Every command wait in this driver was `while (rd8(Sn_CR) != 0) {}`.
 * Unbounded, and this driver holds a lock across it: a chip that stops
 * clearing its command register would take the 9P server and the shell's
 * `net` down with it, and the board would look hung rather than confused.
 * Bounded now, because a driver that gives up is debuggable and one that
 * spins is not. */
static bool cmd_wait(void) {
    for (int i = 0; i < 100000; i++) {
        if (rd8(Sn_CR, BSB_SOCK_REG(SOCK_9P)) == 0) return true;
    }
    g_cmd_timeouts++;
    return false;
}

static void w5500_service_socket(void) {
    uint8_t sr = rd8(Sn_SR, BSB_SOCK_REG(SOCK_9P));

    /* The socket's state machine, as it happens. Off by default and worth its
     * weight during bring-up: a connection that misbehaves shows up here as a
     * state sequence, which is the difference between "the chip is confused"
     * and "the code is". */
    if (g_debug && sr != g_last_sr) {
        uint16_t rsr = 0;
        (void)rd16_stable(Sn_RX_RSR, BSB_SOCK_REG(SOCK_9P), &rsr);
        printk("[W5500] Sn_SR 0x%02x -> 0x%02x (RSR %u, have %u)\n",
               g_last_sr, sr, (unsigned)rsr, (unsigned)g_rx_have);
        g_last_sr = sr;
    } else if (sr != g_last_sr) {
        g_last_sr = sr;
    }

    if (sr == Sn_SR_ESTABLISHED) {
        if (!g_connected) { g_connected = true; g_accepts++; }
        w5500_drain_rx();
        return;
    }

    if (g_connected) {
        g_connected = false;
        g_rx_have = 0;
    }

    if (sr == Sn_SR_CLOSE_WAIT) {
        wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_DISCON);
        cmd_wait();
        w5500_listen();
    } else if (sr == Sn_SR_CLOSED) {
        w5500_listen();
    }
}

static int w5500_link_poll(p9_link_t *link) {
    (void)link;
    if (!g_present || !g_have_address) return 0;
    w5500_lock();
    w5500_service_socket();
    w5500_unlock();

    if (g_rx_have < 4) return 0;
    uint32_t declared = frame_len(g_rx_frame);
    /* Same resync-on-garbage rule as the USB-CDC link: a length that cannot
     * be a 9P frame means the stream is misaligned, and discarding one byte
     * lets the next poll re-examine the window shifted by one rather than
     * wedging on it forever. Over TCP this should never happen -- the stream
     * is ordered and complete -- but "should never" is exactly the assumption
     * that wedged that link once already. */
    if (declared < 7 || declared > P9_MAX_MSIZE) {
        for (uint32_t i = 1; i < g_rx_have; i++) g_rx_frame[i - 1] = g_rx_frame[i];
        g_rx_have--;
        g_resyncs++;
        return 0;
    }
    return (g_rx_have >= declared) ? 1 : 0;
}

static int w5500_link_recv(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    if (g_rx_have < 4) return -1;
    uint32_t declared = frame_len(g_rx_frame);
    if (declared < 7 || declared > max_len || g_rx_have < declared) return -1;

    for (uint32_t i = 0; i < declared; i++) buf[i] = g_rx_frame[i];
    /* Whatever followed this frame in the same TCP segment stays for the next
     * poll -- a pipelining client is entirely legal even if ours is not. */
    for (uint32_t i = declared; i < g_rx_have; i++) g_rx_frame[i - declared] = g_rx_frame[i];
    g_rx_have -= declared;
    g_frames_in++;
    return (int)declared;
}

static int w5500_link_send(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    if (!g_present || !g_connected) return -1;

    /* Held for the whole frame, not per transfer: the pointer arithmetic
     * around Sn_TX_WR is only correct if nothing else moves it in between. */
    w5500_lock();
    int r = w5500_send_locked(buf, len);
    w5500_unlock();
    if (r > 0) g_frames_out++;
    return r;
}

static int w5500_send_locked(const uint8_t *buf, uint32_t len) {
    uint32_t sent = 0;
    while (sent < len) {
        /* The chip's TX buffer is 16 KB and a frame is at most 2 KB, so this
         * loop is a formality on an idle link -- and is not one on a link
         * whose peer has stopped reading, which is precisely when a driver
         * that assumed "it always fits" would corrupt the stream. */
        uint16_t free_space;
        if (!rd16_stable(Sn_TX_FSR, BSB_SOCK_REG(SOCK_9P), &free_space)) continue;
        if (free_space == 0) {
            if (rd8(Sn_SR, BSB_SOCK_REG(SOCK_9P)) != Sn_SR_ESTABLISHED) return -1;
            continue;
        }
        uint32_t chunk = len - sent;
        if (chunk > free_space) chunk = free_space;

        uint16_t wr = rd16(Sn_TX_WR, BSB_SOCK_REG(SOCK_9P));
        w5500_xfer(wr, BSB_SOCK_TX(SOCK_9P), (uint8_t *)(uintptr_t)&buf[sent], chunk, true);
        wr16(Sn_TX_WR, BSB_SOCK_REG(SOCK_9P), (uint16_t)(wr + chunk));

        wr8(Sn_IR, BSB_SOCK_REG(SOCK_9P), 0x10);          /* clear SEND_OK first */
        wr8(Sn_CR, BSB_SOCK_REG(SOCK_9P), Sn_CR_SEND);
        if (!cmd_wait()) return -1;

        /* Wait for the chip to say it went out. Bounded, because a peer that
         * vanished mid-frame must not hang the server: the socket leaving
         * ESTABLISHED is the escape. */
        for (;;) {
            uint8_t ir = rd8(Sn_IR, BSB_SOCK_REG(SOCK_9P));
            if (ir & 0x10) { wr8(Sn_IR, BSB_SOCK_REG(SOCK_9P), 0x10); break; }
            if (ir & 0x08) { wr8(Sn_IR, BSB_SOCK_REG(SOCK_9P), 0x08); return -1; }  /* TIMEOUT */
            if (rd8(Sn_SR, BSB_SOCK_REG(SOCK_9P)) != Sn_SR_ESTABLISHED) return -1;
        }

        sent += chunk;
        g_tx_bytes += chunk;
    }
    return (int)sent;
}

static p9_link_t g_w5500_link = {
    .name = "w5500net",
    .poll = w5500_link_poll,
    .send_frame = w5500_link_send,
    .recv_frame = w5500_link_recv,
    .ctx = NULL,
    /* The one link in this tree that demands a key. Everything else here is
     * a channel inside one address space or a cable on a desk; this one is
     * an Ethernet jack, and phase 18 §1 says what that changes. */
    .auth_required = true,
};

p9_link_t *w5500_get_link(void) {
    return (g_present && g_have_address) ? &g_w5500_link : NULL;
}

void w5500_report(void) {
    w5500_lock();
    cprintf("W5500 Ethernet (SPI0: SCK=GP%d MOSI=GP%d MISO=GP%d CS=GP%d RST=GP%d INT=GP%d)\n",
            CONFIG_W5500_SCK_GPIO, CONFIG_W5500_MOSI_GPIO, CONFIG_W5500_MISO_GPIO,
            CONFIG_W5500_CS_GPIO, CONFIG_W5500_RST_GPIO, CONFIG_W5500_INT_GPIO);
    if (!g_present) {
        cprintf("  chip      : NOT PRESENT (VERSIONR did not read 0x04)\n");
        w5500_unlock();
        return;
    }
    cprintf("  chip      : W5500, VERSIONR 0x%02x\n", rd8(W5500_VERSIONR, BSB_COMMON));
    {
        uint8_t phy = rd8(W5500_PHYCFGR, BSB_COMMON);
        /* The raw register alongside the verdict: bit 0 is link, bit 1 is
         * 100 vs 10, bit 2 is full vs half duplex. When the verdict is "down"
         * the raw value is what distinguishes a PHY that is configured and
         * seeing nothing from one that never came out of reset. */
        cprintf("  link      : %s  (PHYCFGR 0x%02x: %s, %s)\n",
                (phy & 0x01u) ? "UP" : "down (no cable, or the switch port is down)",
                phy, (phy & 0x02u) ? "100Mbps" : "10Mbps",
                (phy & 0x04u) ? "full duplex" : "half duplex");
    }
    cprintf("  last reset: %s\n",
            g_reset_by_pin ? "RSTn pin"
                           : (g_reset_by_sw ? "SOFTWARE (MR bit 7) -- the RSTn pin did nothing"
                                            : "NEITHER pin nor software reset took effect"));
    cprintf("  mac       : %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    if (!g_have_address) {
        cprintf("  address   : UNCONFIGURED -- this board is not on the network\n");
        w5500_unlock();
        return;
    }
    cprintf("  address   : %u.%u.%u.%u  mask %u.%u.%u.%u  gw %u.%u.%u.%u\n",
            g_ip[0], g_ip[1], g_ip[2], g_ip[3],
            g_mask[0], g_mask[1], g_mask[2], g_mask[3],
            g_gw[0], g_gw[1], g_gw[2], g_gw[3]);
    if (g_phy_forced_10bt) {
        /* Say what this costs, because it is what misled N4's bring-up for a
         * whole session. With auto-negotiation disabled the PHY reports link
         * from received energy alone, so PHYCFGR's link bit above says "UP"
         * whether or not the far end ever agreed a mode. A switch port that is
         * still auto-negotiating may parallel-detect its way to 10BASE-T and
         * carry traffic -- or may not, and the register reads identically
         * either way. Every "link UP, LISTEN, answers nothing" board in this
         * phase was in exactly this state, on a switch showing no light. */
        cprintf("  phy mode  : forced 10BT half-duplex (auto-negotiation did not link here)\n");
        cprintf("              NOTE: in forced mode the link bit above is not\n"
                "              evidence the far end agreed. Check the switch's own\n"
                "              port LED; if it is dark, nothing is passing.\n");
    }
    /* Read back from the chip, not from this driver's own variables.
     *
     * Every "reports healthy and answers nothing" session in N4's bring-up was
     * diagnosed against numbers that came from C constants: the port from
     * W5500_9P_PORT, the address from g_ip. Those say what the driver *meant*
     * to configure, and a register that silently did not take is precisely the
     * failure they cannot show. So each of these is what the W5500 itself
     * holds right now; where it disagrees with the intent, the line says so. */
    {
        uint8_t  sip[4] = { 0, 0, 0, 0 };
        uint8_t  smr    = rd8(Sn_MR, BSB_SOCK_REG(SOCK_9P));
        uint16_t sport  = rd16(Sn_PORT, BSB_SOCK_REG(SOCK_9P));
        uint8_t  rxsz   = rd8(Sn_RXBUF_SIZE, BSB_SOCK_REG(SOCK_9P));
        uint8_t  txsz   = rd8(Sn_TXBUF_SIZE, BSB_SOCK_REG(SOCK_9P));
        w5500_xfer(W5500_SIPR, BSB_COMMON, sip, 4, false);

        cprintf("  9P socket : port %u, %s\n", (unsigned)sport,
                sock_state_name(rd8(Sn_SR, BSB_SOCK_REG(SOCK_9P))));
        cprintf("  chip regs : SIPR %u.%u.%u.%u, Sn_MR 0x%02x, bufs %u/%u KB rx/tx%s\n",
                sip[0], sip[1], sip[2], sip[3], smr, rxsz, txsz,
                (sip[0] == g_ip[0] && sip[1] == g_ip[1] && sip[2] == g_ip[2] &&
                 sip[3] == g_ip[3] && sport == W5500_9P_PORT &&
                 (smr & 0x0Fu) == Sn_MR_TCP && rxsz == 8 && txsz == 8)
                    ? "" : "  <-- DISAGREES WITH WHAT THIS DRIVER SET");
    }
    cprintf("  traffic   : %u accepted, %u bytes in, %u bytes out\n",
            (unsigned)g_accepts, (unsigned)g_rx_bytes, (unsigned)g_tx_bytes);
    cprintf("  frames    : %u in, %u out, %u resync discards, %u command timeouts\n",
            (unsigned)g_frames_in, (unsigned)g_frames_out, (unsigned)g_resyncs,
            (unsigned)g_cmd_timeouts);
    cprintf("  rx overruns: %u, unstable RSR reads deferred: %u\n",
            (unsigned)g_rx_overruns, (unsigned)g_rsr_unstable);
    cprintf("  socket now: RSR %u, RD 0x%04x, TX_FSR %u, assembling %u byte(s)\n",
            (unsigned)({ uint16_t v = 0; (void)rd16_stable(Sn_RX_RSR, BSB_SOCK_REG(SOCK_9P), &v); v; }),
            (unsigned)rd16(Sn_RX_RD, BSB_SOCK_REG(SOCK_9P)),
            (unsigned)({ uint16_t v = 0; (void)rd16_stable(Sn_TX_FSR, BSB_SOCK_REG(SOCK_9P), &v); v; }),
            (unsigned)g_rx_have);
    w5500_unlock();
}

#else  /* no W5500 on this board */

int  w5500_init(void)        { return -1; }
bool w5500_present(void)     { return false; }
bool w5500_link_up(void)     { return false; }
bool w5500_have_address(void){ return false; }
void w5500_set_mac(const uint8_t mac[6]) { (void)mac; }
int  w5500_set_address(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]) {
    (void)ip; (void)mask; (void)gw; return -1;
}
void w5500_report(void) { cprintf("No W5500 on this board.\n"); }

#endif
