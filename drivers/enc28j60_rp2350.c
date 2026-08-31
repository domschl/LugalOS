/*
 * Microchip ENC28J60 on SPI0 -- R4, plan/phase19_ip_stack_and_ethernet.md.
 * See drivers/include/drivers/enc28j60.h for what this is and where it sits.
 *
 * Register access is the chip's simple SPI command set (datasheet §4): a
 * one-byte opcode carrying a 5-bit register address in its low bits, then
 * one or more data bytes while CS stays low. Four opcodes matter here: RCR
 * (read control register), WCR (write), BFS/BFC (set/clear individual bits
 * -- ETH registers only, never MAC/MII), plus RBM/WBM (read/write the
 * chip's own 8 KB packet buffer, sequentially, auto-incrementing).
 *
 * Registers live in one of four banks selected by ECON1[1:0]; the five
 * registers at 0x1B-0x1F (EIE, EIR, ESTAT, ECON2, ECON1) are the same in
 * every bank and never need a switch. Reading a MAC or MII register (as
 * opposed to an ETH one) returns a throwaway byte before the real one --
 * the chip's own pipeline latency, not an error -- and BFS/BFC do not work
 * on them at all; a read-modify-write via RCR/WCR is the only way to touch
 * one bit of a MAC register.
 */

#include "drivers/enc28j60.h"
#include "kernel/irq.h"
#include "kernel/sched.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "kernel/identity.h"
#include "lugalos_config.h"

#include <string.h>

#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_ETH_CS_GPIO)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define RESETS_BASE             0x40020000UL
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

/* Same reasoning as drivers/spisd_rp2350.c and the former w5500_rp2350.c:
 * RP2350 gates peripherals to Secure-privileged by default, upstream of
 * PMP, so the pins and SPI0 itself must be opened from here (still M-mode
 * at init) or a later U-mode conversion starts with a load access fault. */
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

#define CS_PIN   CONFIG_ETH_CS_GPIO
#define RST_PIN  CONFIG_ETH_RST_GPIO
#define INT_PIN  CONFIG_ETH_INT_GPIO
#define CS_MASK  (1u << CS_PIN)
#define RST_MASK (1u << RST_PIN)

/* --- ENC28J60 register map (datasheet §3, register summary tables) ----- */

/* Every register reference below packs bank (bits 5-6), address (bits 0-4)
 * and "is this a MAC/MII register" (bit 8) into one uint16_t, so the SPI
 * layer can look at one number and know how to reach it -- no separate bank
 * table to keep in step with a register list by hand. */
#define R(bank, addr)     ((uint16_t)(((bank) << 5) | (addr)))
#define RM(bank, addr)    ((uint16_t)(0x100u | ((bank) << 5) | (addr)))
#define R_BANK(r)         ((uint8_t)(((r) >> 5) & 0x03u))
#define R_ADDR(r)         ((uint8_t)((r) & 0x1Fu))
#define R_IS_MAC(r)       (((r) & 0x100u) != 0)

/* Common to every bank -- no bank switch ever needed for these. */
#define EIE      R(0, 0x1B)
#define EIR      R(0, 0x1C)
#define ESTAT    R(0, 0x1D)
#define ECON2    R(0, 0x1E)
#define ECON1    R(0, 0x1F)

/* Bank 0: buffer pointers. */
#define ERDPTL   R(0, 0x00)
#define ERDPTH   R(0, 0x01)
#define EWRPTL   R(0, 0x02)
#define EWRPTH   R(0, 0x03)
#define ETXSTL   R(0, 0x04)
#define ETXSTH   R(0, 0x05)
#define ETXNDL   R(0, 0x06)
#define ETXNDH   R(0, 0x07)
#define ERXSTL   R(0, 0x08)
#define ERXSTH   R(0, 0x09)
#define ERXNDL   R(0, 0x0A)
#define ERXNDH   R(0, 0x0B)
#define ERXRDPTL R(0, 0x0C)
#define ERXRDPTH R(0, 0x0D)
#define ERXWRPTL R(0, 0x0E)
#define ERXWRPTH R(0, 0x0F)

/* Bank 1: receive filters and the pending-packet count. */
#define ERXFCON  R(1, 0x18)
#define EPKTCNT  R(1, 0x19)

/* Bank 2: MAC control and the MII management interface. MACON2 (0x01) is
 * reserved on the shipping datasheet and normally never touched -- kept
 * here only for the MARST bring-up experiment below, which writes it once. */
#define MACON2   RM(2, 0x01)
#define MACON1   RM(2, 0x00)
#define MACON3   RM(2, 0x02)
#define MACON4   RM(2, 0x03)
#define MABBIPG  RM(2, 0x04)
#define MAIPGL   RM(2, 0x06)
#define MAIPGH   RM(2, 0x07)
#define MAMXFLL  RM(2, 0x0A)
#define MAMXFLH  RM(2, 0x0B)
#define MICMD    RM(2, 0x12)
#define MIREGADR RM(2, 0x14)
#define MIWRL    RM(2, 0x16)
#define MIWRH    RM(2, 0x17)
#define MIRDL    RM(2, 0x18)
#define MIRDH    RM(2, 0x19)

/* Bank 3: MAC address and chip identity. The physical offsets are NOT in
 * octet order -- this is the chip's own layout (datasheet Table 3-3), the
 * single most commonly mis-copied fact about this part. MAADR1 (offset
 * 0x04) holds the MAC's first transmitted octet, MAADR6 (offset 0x01) its
 * last; do not "tidy" this into sequential offsets. */
#define MAADR5   RM(3, 0x00)
#define MAADR6   RM(3, 0x01)
#define MAADR3   RM(3, 0x02)
#define MAADR4   RM(3, 0x03)
#define MAADR1   RM(3, 0x04)
#define MAADR2   RM(3, 0x05)
#define MISTAT   RM(3, 0x0A)
#define EREVID   R(3, 0x12)

/* ECON1 bits */
#define ECON1_BSEL_MASK 0x03u
#define ECON1_RXEN      0x04u
#define ECON1_TXRTS     0x08u
#define ECON1_RXRST     0x40u
#define ECON1_TXRST     0x80u

/* EIR bits */
#define EIR_TXERIF   0x02u
#define EIR_TXIF     0x08u

/* ESTAT bits */
#define ESTAT_TXABRT  0x02u

/* ECON2 bits */
#define ECON2_AUTOINC 0x80u
#define ECON2_PKTDEC  0x40u

/* ERXFCON bits: unicast-to-our-MAC or broadcast, both requiring a passing
 * CRC. CRCEN is unconditionally ANDed with whatever else is enabled --
 * datasheet §8.1 -- rather than joining the OR group ANDOR selects among. */
#define ERXFCON_BCEN  0x01u
#define ERXFCON_CRCEN 0x20u
#define ERXFCON_UCEN  0x80u

/* MACON1 bits */
#define MACON1_MARXEN 0x01u

/* MACON3 bits. PADCFG=0b001 (pad to 60 bytes, append CRC if missing),
 * TXCRCEN, FRMLNEN (type/length field checked) -- 0x32 total, the same
 * value effectively every open ENC28J60 driver settles on for a reason: the
 * datasheet's own worked example uses it. */
#define MACON3_FRMLNEN 0x02u
#define MACON3_TXCRCEN 0x10u
#define MACON3_PADCFG0 0x20u

/* MACON4: DEFER, required for correct half-duplex operation per the
 * datasheet (§6.5) -- without it the MAC does not defer transmission
 * behind a busy medium the way half duplex requires. */
#define MACON4_DEFER 0x40u

/* MICMD bits */
#define MICMD_MIIRD 0x01u

/* MISTAT bits */
#define MISTAT_BUSY 0x01u

/* PHY registers (16-bit, reached only through MIREGADR/MICMD/MIRD/MIWR). */
#define PHCON1  0x00
#define PHCON2  0x10
#define PHSTAT2 0x11

#define PHCON2_HDLDIS 0x0100u   /* disable loopback -- required for half duplex, §6.5 */
#define PHSTAT2_LSTAT 0x0400u   /* non-latching link status */

/* --- packet buffer layout, 8 KB total (0x0000-0x1FFF) ------------------
 *
 * One RX ring and one TX slot, the split every ENC28J60 driver in the wild
 * converges on: 6.5 KB circular RX buffer at the low end, 1.5 KB TX buffer
 * at the top. RXSTOP is deliberately an odd address (0x19FF): the errata
 * workaround below needs ERXRDPT to always hold an odd value, and this is
 * its starting one. */
#define RXSTART 0x0000u
#define RXSTOP  0x19FFu
#define TXSTART 0x1A00u
#define TXSTOP  0x1FFFu

/* --- serialising access -------------------------------------------------
 *
 * Same shape and the same reason as the former w5500_rp2350.c's g_busy: this
 * driver is reached from `netsrv` (net/stack.c's net_poll(), continuously)
 * and from the shell (`net txtest`/`net rxtest` call netif_send()/poll the
 * unclaimed latch directly). Without serialising a whole register sequence
 * -- not just one SPI transfer -- the two interleave mid-operation: one
 * context sets EWRPT for a write, the scheduler preempts, the other reads a
 * pointer register expecting its own prior write to still be in effect. */
static volatile bool g_busy;

static void enc_lock(void) {
    for (;;) {
        uintptr_t f = irq_save();
        if (!g_busy) { g_busy = true; irq_restore(f); return; }
        irq_restore(f);
        sched_yield();   /* a no-op before sched_init(), which is when init runs */
    }
}
static void enc_unlock(void) { g_busy = false; }

/* --- SPI ------------------------------------------------------------------ */

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

/* --- register-level opcodes (datasheet §4) ------------------------------- */

#define OP_RCR 0x00u   /* 000aaaaa */
#define OP_WCR 0x40u   /* 010aaaaa */
#define OP_BFS 0x80u   /* 100aaaaa */
#define OP_BFC 0xA0u   /* 101aaaaa */
#define OP_RBM 0x3Au   /* fixed opcode byte */
#define OP_WBM 0x7Au   /* fixed opcode byte */
#define OP_SRC 0xFFu   /* fixed opcode byte, no address, no data */

static uint8_t g_bank = 0xFF;   /* invalid, so the first select always writes */

static void select_bank(uint16_t r) {
    if (R_ADDR(r) >= 0x1B) return;              /* common register, any bank */
    uint8_t bank = R_BANK(r);
    if (bank == g_bank) return;
    cs_select();
    spi_xfer(OP_BFC | R_ADDR(ECON1));
    spi_xfer(ECON1_BSEL_MASK);
    cs_deselect();
    if (bank) {
        cs_select();
        spi_xfer(OP_BFS | R_ADDR(ECON1));
        spi_xfer(bank);
        cs_deselect();
    }
    g_bank = bank;
    /* Diagnostic, not a documented requirement, and not yet a confirmed
     * explanation: some registers on this chip read or write wrong (MACON1,
     * the odd-offset MAADRs) in a pattern that isn't fully accounted for by
     * "first access after a bank switch" -- MAADR1, also first-touched in
     * its bank, came back clean. Worth ruling in or out anyway, because it
     * is the one variable a 4x SPI clock change (which did not move the
     * corruption at all) could not have tested: the gap between the
     * bank-select transaction finishing and the next one starting, as
     * opposed to timing within a transaction. */
    time_delay_us(20);
}

static uint8_t rcr(uint16_t r) {
    select_bank(r);
    cs_select();
    spi_xfer(OP_RCR | R_ADDR(r));
    if (R_IS_MAC(r)) spi_xfer(0x00);   /* MAC/MII reads return a dummy byte first --
                                        * confirmed load-bearing: disabling it as an
                                        * experiment produced byte-identical output,
                                        * meaning bank 2's problem is a write that
                                        * never lands, not a read-path bug. */
    uint8_t v = spi_xfer(0x00);
    /* CS hold time: the datasheet requires CS to stay asserted for >=210 ns
     * after the last clock edge before release for a MAC/MII register
     * access to latch correctly -- documented explicitly in
     * espressif/esp-eth-drivers' enc28j60 README ("CS Hold Time needs to
     * be configured to be at least 210 ns to properly read MAC and MII
     * registers"), and distinct from every settling delay tried so far
     * (all of which were gaps *after* cs_deselect(), never before it).
     * 1 us is comfortable margin over 210 ns. */
    if (R_IS_MAC(r)) time_delay_us(1);
    cs_deselect();
    return v;
}

static void wcr(uint16_t r, uint8_t v) {
    select_bank(r);
    cs_select();
    spi_xfer(OP_WCR | R_ADDR(r));
    spi_xfer(v);
    /* Same CS hold-time requirement as rcr()'s -- see that function's
     * comment. Applies to writes too, on the theory that the datasheet's
     * >=210 ns figure is about the internal MAC-domain latch generally,
     * not reads specifically. */
    if (R_IS_MAC(r)) time_delay_us(1);
    cs_deselect();
    time_delay_us(20);
}

/* ETH registers only -- never valid on a MAC/MII one (datasheet §4.2.1). */
static void bfs(uint16_t r, uint8_t bits) {
    select_bank(r);
    cs_select();
    spi_xfer(OP_BFS | R_ADDR(r));
    spi_xfer(bits);
    cs_deselect();
}
static void bfc(uint16_t r, uint8_t bits) {
    select_bank(r);
    cs_select();
    spi_xfer(OP_BFC | R_ADDR(r));
    spi_xfer(bits);
    cs_deselect();
}

static uint16_t rcr16(uint16_t lo, uint16_t hi) {
    return (uint16_t)rcr(lo) | ((uint16_t)rcr(hi) << 8);
}
static void wcr16(uint16_t lo, uint16_t hi, uint16_t v) {
    wcr(lo, (uint8_t)(v & 0xFF));
    wcr(hi, (uint8_t)(v >> 8));
}

static void rbm(uint8_t *buf, uint32_t len) {
    cs_select();
    spi_xfer(OP_RBM);
    for (uint32_t i = 0; i < len; i++) buf[i] = spi_xfer(0xFF);
    cs_deselect();
}
static void wbm(const uint8_t *buf, uint32_t len) {
    cs_select();
    spi_xfer(OP_WBM);
    for (uint32_t i = 0; i < len; i++) spi_xfer(buf[i]);
    cs_deselect();
}

/* --- MII (PHY register) access, datasheet §3.3 ---------------------------
 *
 * Indirect: park the PHY register address in MIREGADR, then either write
 * MIWRL/H (write starts the moment the high byte lands) or set MICMD.MIIRD
 * and wait (read). Either way, MISTAT.BUSY must clear before the result --
 * or the next MII operation -- is trustworthy. */
static void mii_wait_idle(void) {
    int timeout = 10000;
    while ((rcr(MISTAT) & MISTAT_BUSY) && --timeout > 0);
}

static void phy_write(uint8_t addr, uint16_t val) {
    wcr(MIREGADR, addr);
    wcr(MIWRL, (uint8_t)(val & 0xFF));
    wcr(MIWRH, (uint8_t)(val >> 8));
    mii_wait_idle();
}

static uint16_t phy_read(uint8_t addr) {
    wcr(MIREGADR, addr);
    bfs(MICMD, MICMD_MIIRD);
    time_delay_us(11);          /* datasheet: >=10.24 us before BUSY is valid */
    mii_wait_idle();
    bfc(MICMD, MICMD_MIIRD);
    return rcr16(MIRDL, MIRDH);
}

/* --- state ---------------------------------------------------------------- */

static bool     g_present;
static uint32_t g_rx_frames, g_tx_frames, g_rx_dropped, g_tx_aborts;
static netif_t  g_netif;

/* netif_t callbacks, defined further down; declared here so
 * enc28j60_init() can wire them into g_netif without reordering the file
 * into "callbacks first, init second". */
static int  enc_poll(netif_t *nif);
static int  enc_send_frame(netif_t *nif, const uint8_t *buf, uint32_t len);
static int  enc_recv_frame(netif_t *nif, uint8_t *buf, uint32_t max_len);
static bool enc_link_up(netif_t *nif);

static bool link_up_locked(void) {
    if (!g_present) return false;
    /* Same quirk as MACON1 (enc_poll_locked()'s comment): a register read
     * on this clone occasionally comes back as the reset/cleared value
     * when the true state is set. Link state is a physical condition that
     * does not flap on the timescale of a few SPI transactions, so a
     * three-try retry treats one UP reading as authoritative rather than
     * requiring every try to agree -- a false negative from this quirk is
     * far more likely than a link that is genuinely bouncing within
     * microseconds. */
    for (int i = 0; i < 3; i++) {
        if (phy_read(PHSTAT2) & PHSTAT2_LSTAT) return true;
    }
    return false;
}

/* --- init ------------------------------------------------------------------ */

static void spi0_init(void) {
    REG(RESETS_RESET_CLR) = RESETS_SPI0_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESETS_SPI0_BIT) && --timeout > 0);

    REG(IO_BANK0_CTRL(CONFIG_ETH_SCK_GPIO))  = 1;   /* F1 = SPI0 */
    REG(IO_BANK0_CTRL(CONFIG_ETH_MOSI_GPIO)) = 1;
    REG(IO_BANK0_CTRL(CONFIG_ETH_MISO_GPIO)) = 1;
    REG(PADS_BANK0_PAD(CONFIG_ETH_SCK_GPIO))  = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_ETH_MOSI_GPIO)) = 0x5A;
    REG(PADS_BANK0_PAD(CONFIG_ETH_MISO_GPIO)) = 0x5A;

    REG(IO_BANK0_CTRL(CS_PIN)) = 5;                 /* F5 = SIO */
    REG(PADS_BANK0_PAD(CS_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = CS_MASK;
    cs_deselect();

    REG(IO_BANK0_CTRL(RST_PIN)) = 5;
    REG(PADS_BANK0_PAD(RST_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_SET) = RST_MASK;
    REG(SIO_GPIO_OUT_SET) = RST_MASK;               /* released; active low */

    /* INT: input only, with the internal pull-up 0x5A already carries --
     * the module's own INT pin is open-drain and fits no pull-up of its
     * own (tests/hw/README.md's wiring table). Not read yet: this driver
     * polls EPKTCNT rather than servicing an interrupt (see enc_poll()). */
    REG(IO_BANK0_CTRL(INT_PIN)) = 5;
    REG(PADS_BANK0_PAD(INT_PIN)) = 0x5A;
    REG(SIO_GPIO_OE_CLR) = (1u << INT_PIN);

    REG(ACCESSCTRL_GPIO_NSMASK0) |= (CS_MASK | RST_MASK | (1u << INT_PIN));
    REG(ACCESSCTRL_SPI0) = ACCESSCTRL_WRITE_PASSWORD | REG(ACCESSCTRL_SPI0)
                           | ACCESSCTRL_NSP | ACCESSCTRL_NSU;

    /* 150 MHz / 16 = 9.375 MHz.
     *
     * Backwards from the usual bring-up instinct, and the reason is
     * specific to cloned ENC28J60 silicon: many of these clones need a
     * SPI clock of at least ~8 MHz to keep their internal buffer-pointer
     * and MAC-register state from corrupting -- too slow is the failure
     * mode, not too fast. This driver spent a long detour at 2.5 MHz and
     * then 600 kHz chasing exactly that corruption (MACON1/MACON3/MABBIPG
     * and the odd-offset MAADR bytes all failing to hold a written value,
     * reproducibly, across two different physical chips) before this was
     * identified as the actual cause rather than a driver bug. 16/SCR=0
     * clears the ~8 MHz floor with margin while staying under the
     * datasheet's 20 MHz ceiling. */
    REG(SSPCR1) = 0;
    REG(SSPCPSR) = 16;
    REG(SSPCR0) = 0x7;                              /* SCR=0, 8-bit, mode 0 */
    REG(SSPCR1) = (1u << 1);                        /* SSE */
}

static void enc_hw_reset(void) {
    REG(SIO_GPIO_OUT_CLR) = RST_MASK;
    time_delay_us(1000);
    REG(SIO_GPIO_OUT_SET) = RST_MASK;
    time_delay_us(1000);

    /* Software reset too, even right after a hardware one: SRC is what the
     * datasheet's own init sequence uses, and it is cheap insurance if the
     * RST line's timing was ever marginal. Errata: ESTAT.CLKRDY does not
     * reliably reflect the post-reset state -- the documented workaround is
     * an unconditional delay rather than polling it, so that is what this
     * does (>=1 ms required; 2 ms taken). */
    cs_select();
    spi_xfer(OP_SRC);
    cs_deselect();
    time_delay_us(2000);
    g_bank = 0xFF;
}

static void enc_apply_mac(const uint8_t mac[6]) {
    wcr(MAADR1, mac[0]);
    wcr(MAADR2, mac[1]);
    wcr(MAADR3, mac[2]);
    wcr(MAADR4, mac[3]);
    wcr(MAADR5, mac[4]);
    wcr(MAADR6, mac[5]);
}

/* Reads MAADR1-6 straight back, in the same non-sequential order they were
 * written in. Bank 3, MAC-class registers -- a different bank and a
 * different register class than the SRAM loopback exercises, so this is
 * independent evidence that select_bank()'s bank-3 path and the RCR
 * dummy-byte handling for MAC/MII registers are both correct, rather than
 * assuming a passing loopback (bank 0, plain buffer opcodes) generalises. */
static void enc_read_mac(uint8_t out[6]) {
    out[0] = rcr(MAADR1);
    out[1] = rcr(MAADR2);
    out[2] = rcr(MAADR3);
    out[3] = rcr(MAADR4);
    out[4] = rcr(MAADR5);
    out[5] = rcr(MAADR6);
}

static void enc_mac_phy_init(void) {
    /* Receive buffer ring. ERXRDPT gets RXSTOP -- odd, per the errata below
     * -- rather than RXSTART; there is nothing received yet for it to
     * protect, so any legal starting value works and the odd one avoids
     * relying on a first update to fix an even one up. */
    wcr16(ERXSTL, ERXSTH, RXSTART);
    wcr16(ERXNDL, ERXNDH, RXSTOP);
    wcr16(ERXRDPTL, ERXRDPTH, RXSTOP);
    wcr16(ERDPTL, ERDPTH, RXSTART);

    /* Errata: EIR.PKTIF does not reliably track pending packets. EPKTCNT is
     * the documented workaround and this driver's poll() below uses it,
     * never PKTIF. */
    wcr(ERXFCON, ERXFCON_UCEN | ERXFCON_CRCEN | ERXFCON_BCEN);

    wcr(MACON1, MACON1_MARXEN);
    wcr(MACON3, MACON3_FRMLNEN | MACON3_TXCRCEN | MACON3_PADCFG0);
    wcr(MACON4, MACON4_DEFER);
    wcr16(MAMXFLL, MAMXFLH, NETIF_FRAME_MAX + 4u);  /* +4: hardware counts the CRC we never see */

    /* Half duplex back-to-back/non-back-to-back inter-packet gaps -- the
     * datasheet's own recommended constants (§6.5), not derived. */
    wcr(MABBIPG, 0x12);
    wcr(MAIPGL, 0x12);
    wcr(MAIPGH, 0x0C);

    /* PHY: half duplex (PDPXMD left clear -- PHCON1's reset default), and
     * HDLDIS set. Without HDLDIS the PHY loops transmitted frames back to
     * its own receiver in half duplex, which the datasheet documents as
     * required (§6.5) and which reads back as "every frame we send arrives
     * at our own receiver a moment later" if skipped. */
    phy_write(PHCON1, 0x0000);
    phy_write(PHCON2, PHCON2_HDLDIS);

    bfs(ECON1, ECON1_RXEN);
}

/* Bring-up diagnostic: a write-then-read-back of the chip's own SRAM via
 * RBM/WBM (0x3A/0x7A, no address byte -- they move bytes at the current
 * EWRPT/ERDPT). EWRPT/ERDPT are bank 0 registers, but bank 0 is also the
 * documented post-reset default, so this exercises the *simplest* path
 * through select_bank() (clearing BSEL to an already-clear value) rather
 * than the bank-3 switch EREVID needs -- so a loopback pass with a bad
 * EREVID isolates the fault to that switch (or to EREVID itself, on a clone
 * that does not implement it) rather than to the bus. Kept as a permanent
 * boot-time check rather than a one-off: it costs eight bytes over SPI and
 * turns "EREVID read something odd" into two different, separately
 * actionable problems instead of one ambiguous one. */
static bool enc_sram_loopback(void) {
    static const uint8_t pattern[4] = { 0xA5, 0x3C, 0x81, 0x5A };
    uint8_t back[4] = { 0, 0, 0, 0 };
    wcr16(EWRPTL, EWRPTH, 0x0000);
    wbm(pattern, sizeof(pattern));
    wcr16(ERDPTL, ERDPTH, 0x0000);
    rbm(back, sizeof(back));
    return memcmp(pattern, back, sizeof(pattern)) == 0;
}

int enc28j60_init(void) {
    spi0_init();
    enc_hw_reset();

    bool sram_ok = enc_sram_loopback();

    /* Bring-up experiments, condensed onto few printk lines because
     * /proc/kmsg's ring buffer has repeatedly evicted earlier entries once
     * this driver logs much more than a handful -- verified live over the
     * console instead when that happened.
     *
     * (1) MARST: an early datasheet revision documented MACON2 (bank 2,
     * 0x01) with a MAC-reset bit that must be cleared before the MAC block
     * responds to anything -- dropped from the shipping datasheet, so
     * normally untouched. If this clone's silicon retains it and powers up
     * asserted, it would hold the whole MAC block in reset and explain
     * every MAC/MII register failing at once while everything outside the
     * MAC block (ECON1, ERXFCON, EREVID, buffer pointers) keeps working.
     * (2) Three MABBIPG write/read round trips: plain, a 1 ms settle
     * (fifty times the 20 us already used elsewhere), and written twice.
     * (3) ECON1 self-test: BSEL values 0-3 written via WCR (bypassing
     * BFS/select_bank entirely) and read straight back, to separate "is
     * bank selection itself the problem" from everything above. */
    wcr(MACON2, 0x00);
    uint8_t macon2_back = rcr(MACON2);

    wcr(MABBIPG, 0x99);
    uint8_t isolated = rcr(MABBIPG);
    wcr(MABBIPG, 0x88);
    time_delay_us(1000);
    uint8_t isolated3 = rcr(MABBIPG);
    wcr(MABBIPG, 0x66);
    time_delay_us(200);
    wcr(MABBIPG, 0x66);
    time_delay_us(200);
    uint8_t isolated4 = rcr(MABBIPG);

    uint8_t econ1_back[4];
    {
        static const uint8_t testvals[] = { 0x00, 0x01, 0x02, 0x03 };
        for (unsigned i = 0; i < sizeof(testvals); i++) {
            cs_select(); spi_xfer(OP_WCR | R_ADDR(ECON1)); spi_xfer(testvals[i]); cs_deselect();
            time_delay_us(20);
            cs_select(); spi_xfer(OP_RCR | R_ADDR(ECON1));
            econ1_back[i] = spi_xfer(0x00);
            cs_deselect();
        }
        g_bank = testvals[sizeof(testvals) - 1];   /* matches the last write above */
    }

    printk("[ENC28J60] bringup: MACON2<-0x00 -> 0x%02x; MABBIPG<-0x99 -> 0x%02x; "
           "<-0x88+1ms -> 0x%02x; <-0x66x2 -> 0x%02x\n",
           macon2_back, isolated, isolated3, isolated4);
    printk("[ENC28J60] bringup: ECON1 self-test 0x00/01/02/03 -> 0x%02x/0x%02x/0x%02x/0x%02x\n",
           econ1_back[0], econ1_back[1], econ1_back[2], econ1_back[3]);

    /* EREVID is a silicon revision number, not a fixed constant -- known
     * values are 0x02 and 0x06 across the B5/B7 revisions this part has
     * shipped as. 0x00 or 0xFF is "the bus is not talking to anything";
     * anything else is accepted, on the theory that a wrong-but-plausible
     * revision is still evidence of a real chip and this driver has never
     * been run against a second one to know its exact set.
     *
     * Presence is decided from the SRAM loopback, not EREVID: RBM/WBM need
     * no bank select, so they are strictly more trustworthy evidence that
     * this is a live, responding chip than one register read is. A handful
     * of very cheap clone boards are known to answer EREVID with 0x00 while
     * otherwise working -- treating that as fatal would refuse hardware
     * that runs fine, on a register whose only job is a version number
     * nothing downstream of this function reads again. */
    uint8_t rev = rcr(EREVID);
    g_present = sram_ok;
    if (!g_present) {
        printk("[ENC28J60] SRAM loopback failed (wrote a5:3c:81:5a, EREVID read 0x%02x) --\n"
               "           SPI0 bus not talking to a chip (check SI=GP%d SO=GP%d SCK=GP%d\n"
               "           CS=GP%d RESET=GP%d, GND common with the RP2350, and 3V3 -- not 5V)\n",
               rev, CONFIG_ETH_MOSI_GPIO, CONFIG_ETH_MISO_GPIO,
               CONFIG_ETH_SCK_GPIO, CS_PIN, RST_PIN);
        return -1;
    }
    if (rev == 0x00 || rev == 0xFF) {
        printk("[ENC28J60] present (SRAM loopback OK) but EREVID read 0x%02x, not a documented\n"
               "           revision -- likely a clone; proceeding anyway\n", rev);
    }

    enc_mac_phy_init();

    /* Same reasoning as the MAADR readback: prove the writes landed rather
     * than assume it, on a chip that has already shown one register class
     * (RCR at odd bank-3 offsets) reading back wrong. PHCON1/PHCON2 go
     * through the MII path (MIREGADR/MIWR), not RCR/WCR at all, so this is
     * independent evidence either way. */
    printk("[ENC28J60] readback: ECON1 0x%02x, MACON1 0x%02x, MACON3 0x%02x, PHCON1 0x%04x, PHCON2 0x%04x\n",
           rcr(ECON1), rcr(MACON1), rcr(MACON3), phy_read(PHCON1), phy_read(PHCON2));
    printk("[ENC28J60] more detail: MACON4 0x%02x (want 0x40), MABBIPG 0x%02x (want 0x12), "
           "MAMXFLL 0x%02x MAMXFLH 0x%02x (want 0xee 0x05) [bank2]; ERXFCON 0x%02x (want 0xa1) [bank1]\n",
           rcr(MACON4), rcr(MABBIPG), rcr(MAMXFLL), rcr(MAMXFLH), rcr(ERXFCON));

    memset(&g_netif, 0, sizeof(g_netif));
    g_netif.name = "enc0";
    g_netif.poll = enc_poll;
    g_netif.send_frame = enc_send_frame;
    g_netif.recv_frame = enc_recv_frame;
    g_netif.link_up = enc_link_up;
    if (netif_register(&g_netif) != 0) {
        printk("[ENC28J60] netif_register() failed -- no free interface slot\n");
        g_present = false;
        return -1;
    }
    /* netif_register() has now filled g_netif.mac with the node's identity
     * (we left it zeroed); the chip needs it too, for its own unicast
     * filter -- see enc_apply_mac(). */
    enc_apply_mac(g_netif.mac);

    uint8_t mac_back[6];
    enc_read_mac(mac_back);
    bool mac_ok = memcmp(g_netif.mac, mac_back, 6) == 0;
    if (!mac_ok) {
        printk("[ENC28J60] MAADR readback mismatch -- wrote %02x:%02x:%02x:%02x:%02x:%02x,\n"
               "           read back %02x:%02x:%02x:%02x:%02x:%02x (bank 3 write/read suspect)\n",
               g_netif.mac[0], g_netif.mac[1], g_netif.mac[2], g_netif.mac[3], g_netif.mac[4], g_netif.mac[5],
               mac_back[0], mac_back[1], mac_back[2], mac_back[3], mac_back[4], mac_back[5]);
    }

    uint16_t phstat2 = phy_read(PHSTAT2);
    char macstr[18];
    netif_mac_str(g_netif.mac, macstr);
    printk("[ENC28J60] present (EREVID 0x%02x, MAADR readback %s), mac %s, link %s (PHSTAT2 0x%04x)\n",
           rev, mac_ok ? "OK" : "MISMATCH", macstr,
           (phstat2 & PHSTAT2_LSTAT) ? "UP" : "down (check cable/switch)", phstat2);
    return 0;
}

netif_t *enc28j60_get_netif(void) { return g_present ? &g_netif : NULL; }

/* --- transmit --------------------------------------------------------------
 *
 * Errata (Transmit Logic): after certain abort conditions the transmit
 * state machine can wedge. The documented workaround is unconditional --
 * reset it (ECON1.TXRST) immediately before every transmission, not only
 * after a failure is observed, since the failure that matters is the one
 * this avoids rather than the one it would let through and then explain. */
static int enc_send_locked(const uint8_t *buf, uint32_t len) {
    if (len > NETIF_FRAME_MAX) return -1;

    bfs(ECON1, ECON1_TXRST);
    bfc(ECON1, ECON1_TXRST);
    bfc(EIR, EIR_TXERIF | EIR_TXIF);

    wcr16(EWRPTL, EWRPTH, TXSTART);
    uint8_t ctrl = 0x00;   /* use MACON3's PADCFG/CRC defaults for this frame */
    wbm(&ctrl, 1);
    wbm(buf, len);

    wcr16(ETXSTL, ETXSTH, TXSTART);
    wcr16(ETXNDL, ETXNDH, (uint16_t)(TXSTART + len));   /* last byte written */

    bfs(ECON1, ECON1_TXRTS);

    /* Bounded: a switch port that never accepts the frame must not hang the
     * caller. ~2 ms at this SPI rate is generous for a 10 Mbit link sending
     * at most 1514 bytes. */
    int timeout = 20000;
    uint8_t eir;
    do {
        eir = rcr(EIR);
    } while (!(eir & (EIR_TXIF | EIR_TXERIF)) && --timeout > 0);

    bfc(ECON1, ECON1_TXRTS);

    if (!(eir & EIR_TXIF) || (eir & EIR_TXERIF) || (rcr(ESTAT) & ESTAT_TXABRT)) {
        g_tx_aborts++;
        return -1;
    }
    return (int)len;
}

static int enc_send_frame(netif_t *nif, const uint8_t *buf, uint32_t len) {
    (void)nif;
    if (!g_present) return -1;
    enc_lock();
    int r = enc_send_locked(buf, len);
    enc_unlock();
    if (r > 0) g_tx_frames++;
    return r;
}

/* --- receive ----------------------------------------------------------------
 *
 * One frame latched here per poll(), the same shape net/stack.c's own
 * single-reused-buffer design already expects (net_poll() calls poll() then
 * immediately recv_frame() on the same call stack -- never a queue of more
 * than one). */
static uint8_t  g_rx_buf[NETIF_FRAME_MAX];
static uint32_t g_rx_len;

/* Errata (Receive Buffer): ERXRDPT must always be written with an ODD
 * value. The chip's own "next packet pointer" is not guaranteed odd, so the
 * update is next-1, wrapping to RXSTOP at the one point that subtraction
 * would leave the ring (next == RXSTART). Getting this wrong does not fail
 * loudly -- it corrupts the ring's free-space accounting and packets after
 * the first bad one arrive truncated or not at all. */
static void advance_rdpt(uint16_t next_pkt) {
    uint16_t rdpt = (next_pkt == RXSTART) ? RXSTOP : (uint16_t)(next_pkt - 1);
    wcr16(ERXRDPTL, ERXRDPTH, rdpt);
}

static uint32_t g_mac_reinits;

static int enc_poll_locked(void) {
    if (g_rx_len > 0) return 1;              /* already have one waiting */

    /* Workaround, not a root-cause fix: ECON1.RXEN spontaneously clearing
     * is a documented ENC28J60 failure mode in the wild, independent of
     * this specific clone -- see ntruchsess/arduino_uip#167, which reports
     * it on genuine hardware on noisy production networks over hours to
     * weeks, RXEN cleared with the activity LED still blinking, and
     * recommends exactly this: notice ECON1.RXEN==0 and reinitialise
     * rather than patch the one bit. This driver hit the same signature
     * within seconds rather than weeks, plus MACON1.MARXEN clearing
     * alongside it and (once) a stalled transmit -- broad enough that a
     * single-bit patch stopped being enough once it started showing up in
     * more than one register. A full reinit is cheap here (a few hundred
     * bytes over SPI, no scheduler wait) and catches whatever else might
     * have drifted, not just the two registers observed so far. */
    /* Checked independently, not just ECON1.RXEN: observed a run where
     * ECON1.RXEN read correctly set while MACON1 itself had reverted --
     * a combination the arduino_uip issue's single symptom doesn't cover,
     * and checking only ECON1 silently missed it (0 reinits logged while
     * MACON1 sat wrong). Either one wrong triggers the same full recovery. */
    if ((rcr(ECON1) & ECON1_RXEN) == 0 || rcr(MACON1) != MACON1_MARXEN) {
        enc_mac_phy_init();
        enc_apply_mac(g_netif.mac);
        g_mac_reinits++;
    }

    if (rcr(EPKTCNT) == 0) return 0;

    uint8_t hdr[6];
    rbm(hdr, sizeof(hdr));
    uint16_t next_pkt = (uint16_t)hdr[0] | ((uint16_t)hdr[1] << 8);
    uint16_t rsv_len  = (uint16_t)hdr[2] | ((uint16_t)hdr[3] << 8);
    bool     ok        = (hdr[4] & 0x80) != 0;   /* RSV bit 23: received OK */

    if (!ok || rsv_len == 0 || rsv_len > sizeof(g_rx_buf)) {
        /* Drain and discard: the pointer must still advance past this
         * packet, or the ring never frees the space it occupied. */
        if (rsv_len && rsv_len <= sizeof(g_rx_buf)) {
            uint8_t scratch[64];
            uint32_t left = rsv_len;
            while (left) {
                uint32_t chunk = left < sizeof(scratch) ? left : sizeof(scratch);
                rbm(scratch, chunk);
                left -= chunk;
            }
        }
        wcr16(ERDPTL, ERDPTH, next_pkt);
        advance_rdpt(next_pkt);
        bfs(ECON2, ECON2_PKTDEC);
        g_rx_dropped++;
        return 0;
    }

    rbm(g_rx_buf, rsv_len);
    wcr16(ERDPTL, ERDPTH, next_pkt);
    advance_rdpt(next_pkt);
    bfs(ECON2, ECON2_PKTDEC);

    g_rx_len = rsv_len;
    return 1;
}

static int enc_poll(netif_t *nif) {
    (void)nif;
    if (!g_present) return -1;
    enc_lock();
    int r = enc_poll_locked();
    enc_unlock();
    return r;
}

static int enc_recv_frame(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    (void)nif;
    if (g_rx_len == 0 || g_rx_len > max_len) return -1;
    enc_lock();
    memcpy(buf, g_rx_buf, g_rx_len);
    uint32_t n = g_rx_len;
    g_rx_len = 0;
    enc_unlock();
    g_rx_frames++;
    return (int)n;
}

static bool enc_link_up(netif_t *nif) {
    (void)nif;
    if (!g_present) return false;
    enc_lock();
    bool up = link_up_locked();
    enc_unlock();
    return up;
}

void enc28j60_dump_regs(void) {
    if (!g_present) { printk("[ENC28J60] not present\n"); return; }
    enc_lock();
    uint8_t eie = rcr(EIE), eir = rcr(EIR), estat = rcr(ESTAT);
    uint8_t econ1 = rcr(ECON1), econ2 = rcr(ECON2);
    uint8_t pktcnt = rcr(EPKTCNT), erxfcon = rcr(ERXFCON), macon1 = rcr(MACON1);
    uint16_t erxrdpt = rcr16(ERXRDPTL, ERXRDPTH);
    uint16_t erxwrpt = rcr16(ERXWRPTL, ERXWRPTH);
    uint16_t phstat2 = phy_read(PHSTAT2);
    enc_unlock();
    printk("[ENC28J60] EIE 0x%02x EIR 0x%02x ESTAT 0x%02x ECON1 0x%02x ECON2 0x%02x\n",
           eie, eir, estat, econ1, econ2);
    printk("[ENC28J60] EPKTCNT %u ERXFCON 0x%02x MACON1 0x%02x ERXRDPT 0x%04x ERXWRPT 0x%04x PHSTAT2 0x%04x\n",
           pktcnt, erxfcon, macon1, erxrdpt, erxwrpt, phstat2);
    printk("[ENC28J60] full MAC/PHY reinit triggered %lu time(s) since boot\n",
           (unsigned long)g_mac_reinits);
}

#else  /* no ENC28J60 pin map on this board */

int enc28j60_init(void) { return -1; }
netif_t *enc28j60_get_netif(void) { return NULL; }
void enc28j60_dump_regs(void) { printk("[ENC28J60] not built for this board\n"); }

#endif
