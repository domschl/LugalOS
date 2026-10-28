/* K210 system NOR flash — the GD25LQ on the Maix Bit's dedicated flash
 * pads, driven by the SPI3 SSIC (plan/phase65 §1.2, §1.5).
 *
 * Silicon facts this driver exists around, all measured on the board
 * (tools/spi_nor_probe_k210.c holds the same sequences as probes):
 *
 *   * The SSIC at 0x54000000 is Canaan's "SSI_Canaan" variant, not
 *     stock DW SSI: SR reads 6 at idle and BUSY never clears, so
 *     pacing must use TXFLR levels only; DFS reads 0 whatever is
 *     written — frames are fixed 8-bit; the FIFO is 32 deep by the
 *     vendor driver's own arithmetic but stays conservative here at 16.
 *   * The RX FIFO survives a transaction (nothing above a disable
 *     clears it), and a stale first beat shifts the whole answer
 *     window — each command therefore drains and re-enables.
 *   * In standard TX+RX mode the answer stream begins at beat n_tx:
 *     beats during the command phase are what MISO happened to hold.
 *     RDID answers C8 60 18 at beats 1..3 (measured).
 *   * The part needs no FPIOA step; its pads belong to this block.
 *     IO2/IO3 (WP#/HOLD#) idle correctly only under this controller —
 *     the reason the same pads are dead as GPIO and the reason BROM
 *     flash boot keeps working beside this driver.
 *   * Single-line standard mode only. QE stays 0: quad would need the
 *     QE bit and QPI framing the BROM may fight, for a bus that is
 *     already faster than the kernel's appetite. The QE bit is left
 *     exactly as found (measured 0).
 *
 * Kernel placement: this is the /flash0 backend (drivers/flashdisk.c
 * picks it via CONFIG_BOARD_K210) and the byte-mover under
 * tools/k210flash.py's SRAM writer blob. Unlike every flash here except
 * the P4's, there is no XIP: the kernel runs from SRAM while the bus
 * churns, so an erase cannot yank the instruction stream — no cache
 * suspension dance and no second-hart parking, which is what makes the
 * P4's erase path elaborate. The one rule that remains is that WIP
 * polling spins, so callers are tasks, not trap handlers. */

#include "drivers/spi_nor_k210.h"
#include "kernel/lock.h"
#include "kernel/printk.h"

#include <string.h>

#define REG(a)   (*(volatile uint32_t *)(uintptr_t)(a))

#define SYSCTL            0x50440000UL
#define CLK_EN_CENT       REG(SYSCTL + 0x28)
#define CLK_EN_PERI       REG(SYSCTL + 0x2c)
#define CENT_APB2         (1u << 5)
#define PERI_SPI3         (1u << 9)           /* clk_en_peri: rom0 dma1 ai2 dvp3 fft4 gpio5 spi0..spi3 = 6..9 -- the struct in k210_sysctl.h, not the sdk-legacy enum */

#define SPI3              0x54000000UL
#define SPI_CTRLR0        REG(SPI3 + 0x00)
#define SPI_CTRLR1        REG(SPI3 + 0x04)
#define SPI_SSIENR        REG(SPI3 + 0x08)
#define SPI_SER           REG(SPI3 + 0x10)
#define SPI_BAUDR         REG(SPI3 + 0x14)
#define SPI_TXFLR         REG(SPI3 + 0x20)
#define SPI_RXFLR         REG(SPI3 + 0x24)
#define SPI_IMR           REG(SPI3 + 0x2c)
#define SPI_DR            REG(SPI3 + 0x60)

/* NOR commands (the GD25LQ/W25Q common subset). */
#define CMD_RES           0x06      /* set write-enable latch           */
#define CMD_RDSR          0x05
#define CMD_READ          0x03
#define CMD_PP            0x02      /* page program, <= 256 bytes       */
#define CMD_SE            0x20      /* sector erase, 4 KB               */
#define CMD_BE            0xD8      /* block erase, 64 KB               */
#define CMD_RDID          0x9F
#define CMD_RSTEN         0x66      /* reset enable / reset: the part's */
#define CMD_RST           0x99      /* own recovery from any mode       */
#define CMD_WRSR          0x01
#define CMD_EXIT_4B       0xE9      /* leave 4-byte-address mode        */
#define SR_WIP            (1u << 0)
#define SR_WEL            (1u << 1)

/* The runtime write floor (cmake/flash_layout_k210.cmake): below this
 * lie the loader and the boot machinery, which a running system must
 * never program. The P4 driver draws the same line for the same
 * reason — one typo'd address should not be an unbootable board. The
 * boot-record bit clears the future on-device update needs are a
 * deliberately narrower door (a word AND, no erase), not a widening
 * of this one. */
#if !defined(LUGALOS_K210_FLASH_WRITABLE_FLOOR)
#define LUGALOS_K210_FLASH_WRITABLE_FLOOR 0u
#endif

#define NOR_PAGE_SIZE     256u
#define NOR_SECTOR_SIZE   0x1000u
#define NOR_BLOCK_SIZE    0x10000u

/* A beat is a byte on the wire. Worst-case WIP waits: 400 ms for a GD
 * sector erase at 390 MHz is over a hundred million loops; bound by
 * iterations that comfortably exceed the datasheet maxima. */
#define XFER_SPIN_MAX     20000000u

/* guarded by: g_nor_ylock (all public entry points; the SPI3 FIFOs and
 * the part's WEL latch are shared, and WREN must pair with its command). */
static ylock_t g_nor_ylock;
static bool     g_nor_present;
static uint32_t g_nor_capacity;
static const char *g_nor_part = "none";

static void spi_pads_clock(void) {
    CLK_EN_CENT |= CENT_APB2;
    CLK_EN_PERI |= PERI_SPI3;
}

/* One full-duplex transaction: n_tx command bytes then (n_tot - n_tx)
 * zero-fill bytes, collecting n_tot answer beats. Answer beats start at
 * index n_tx. CS is the SSIC's own; SER is raised for the frame. */
static uint32_t spi_xfer(const uint8_t *tx, uint32_t n_tx,
                         uint8_t *rx, uint32_t n_tot) {
    uint32_t rxgot = 0, sent = 0;
    /* Drain stale beats — but never re-disable the SSIC between
     * frames: the enable edge puts glitch clocks on the wire while
     * CS is idle, and the NOR latched them as garbage commands
     * (measured: stray BP bits and half-written pages). This loop
     * mirrors the proven writer blob (tools/k210_wblob.c). */
    while (SPI_RXFLR & 0x3Fu)
        (void)SPI_DR;
    SPI_SER = 1;
    uint32_t spin = 0;
    while (rxgot < n_tot && spin++ < XFER_SPIN_MAX) {
        while (SPI_RXFLR & 0x3Fu) {
            uint8_t b = (uint8_t)SPI_DR;
            if (rx && rxgot < n_tot) rx[rxgot++] = b;
            else rxgot++;
        }
        if (sent < n_tot && (SPI_TXFLR & 0x3Fu) < 16u) {
            SPI_DR = (sent < n_tx) ? tx[sent] : 0xFFu;
            sent++;
        }
    }
    /* CS must stay asserted until the shifter has drained. */
    while ((SPI_TXFLR & 0x3Fu) && spin-- < 2u * XFER_SPIN_MAX)
        ;
    for (volatile uint32_t d = 0; d < 200u; d++) ;
    SPI_SER = 0;
    return rxgot;
}

/* The part raises WIP some microseconds after the CS fall; an
 * instantly-following RDSR can still read idle and let the next
 * command collide with the running program/erase. Coarse settle
 * before the first poll (measured requirement). */
static void nor_settle(void) {
    for (volatile uint32_t d = 0; d < 100000u; d++) ;
}

static uint8_t nor_status(void) {
    const uint8_t cmd = CMD_RDSR;
    uint8_t ans[4] = {0};
    spi_xfer(&cmd, 1, ans, 2);
    return ans[1];
}

/* Poll until the part clears WIP. -1 on timeout. */
static int nor_wait_ready(void) {
    for (uint32_t spin = 0; spin < XFER_SPIN_MAX; spin++)
        if (!(nor_status() & SR_WIP))
            return 0;
    return -1;
}

static void nor_write_enable(void) {
    const uint8_t cmd = CMD_RES;
    spi_xfer(&cmd, 1, 0, 1);
}

int spi_nor_k210_init(void) {
    ylock_acquire(&g_nor_ylock);
    spi_pads_clock();
    /* BROM's XPI reads leave spi3_xip_en (SYSCTL+0x58 bit15) set, and
     * while it is set the XPI path owns the NOR pads — SSIC masters
     * nothing and every RX beat is stale FIFO content. Take the pads
     * back before touching the controller. */
    REG(SYSCTL + 0x58) &= ~((1u << 15) | (1u << 12));
    SPI_SSIENR = 0;
    SPI_CTRLR0 = 0;               /* mode 0, TMOD standard, 8-bit fixed */
    SPI_CTRLR1 = 0;
    SPI_BAUDR  = 64;              /* parent ~390 MHz -> SCLK ~6 MHz, the rate the writer blob was silicon-verified at; 4 (100 MHz) makes the part read 0xFF forever (measured) */
    SPI_IMR    = 0;               /* no PLIC traffic ever from this block */
    SPI_SSIENR = 1;

    /* Reset the part: the only recovery from whatever mode a previous
     * owner (BROM boot attempt, ISP stub) left it in. */
    const uint8_t rst_en = CMD_RSTEN, rst = CMD_RST, exit4b = CMD_EXIT_4B;
    spi_xfer(&rst_en, 1, 0, 1);
    spi_xfer(&rst, 1, 0, 1);
    nor_wait_ready();
    /* Force 3-byte address width. A latched 4-byte mode (BROM, the
     * ISP stub's DIO init, or a previous kernel) shifts every fixed
     * 3-byte frame by one byte: reads show a leading 0xFF and
     * programs land one byte early. */
    spi_xfer(&exit4b, 1, 0, 1);
    /* Clear any stale block protect / SRWD. Programs into protected
     * blocks complete WIP-clean while writing nothing — the worst
     * failure mode there is. */
    nor_write_enable();
    const uint8_t wrsr[2] = { CMD_WRSR, 0x00 };
    spi_xfer(wrsr, 2, 0, 2);
    nor_wait_ready();

    const uint8_t cmd = CMD_RDID;
    uint8_t ans[8] = {0};
    spi_xfer(&cmd, 1, ans, 4);
    uint32_t id = ((uint32_t)ans[1] << 16) | (ans[2] << 8) | ans[3];
    uint8_t mfr = (uint8_t)(id >> 16), cap = (uint8_t)id;
    g_nor_present  = (id != 0 && id != 0xFFFFFFu);
    /* Capacity from the third RDID byte: it is the log2 of the byte
     * count on both GigaDevice and Winbond families. */
    g_nor_capacity = g_nor_present ? (1u << cap) : 0;
    switch (id) {
    case 0xC86018u: case 0xC84018u: g_nor_part = "gd25lq128"; break;
    case 0xC86017u: case 0xC84017u: g_nor_part = "gd25lq64";  break;
    case 0xC86016u: case 0xC84016u: g_nor_part = "gd25lq32";  break;
    case 0xEF4018u: g_nor_part = "w25q128"; break;
    case 0xEF4017u: g_nor_part = "w25q64";  break;
    default: g_nor_part = g_nor_present ? "nor-generic" : "none"; break;
    }
    ylock_release(&g_nor_ylock);
    return g_nor_present ? 0 : -1;
}

bool spi_nor_k210_ready(void)       { return g_nor_present; }
uint32_t spi_nor_k210_capacity(void){ return g_nor_capacity; }
const char *spi_nor_k210_part(void) { return g_nor_part; }

int spi_nor_k210_read(uint32_t addr, void *buf, uint32_t len) {
    if (!g_nor_present || addr + len > g_nor_capacity)
        return -1;
    ylock_acquire(&g_nor_ylock);
    /* READ 0x03 answers continuously, but keep transactions at 256
     * bytes so one long read cannot starve the ylock holder's peers
     * for an uncomfortable time; the cost is negligible at this speed. */
    uint8_t *out = (uint8_t *)buf;
    int rc = 0;
    while (len) {
        uint32_t chunk = len > 256u ? 256u : len;
        uint8_t cmd[4] = { CMD_READ, (uint8_t)(addr >> 16),
                           (uint8_t)(addr >> 8), (uint8_t)addr };
        uint8_t ans[260];
        uint32_t got = spi_xfer(cmd, 4, ans, 4 + chunk);
        if (got < 4 + chunk) { rc = -1; break; }
        memcpy(out, ans + 4, chunk);
        out += chunk; addr += chunk; len -= chunk;
    }
    ylock_release(&g_nor_ylock);
    return rc;
}

int spi_nor_k210_write(uint32_t addr, const void *buf, uint32_t len) {
    if (!g_nor_present || addr + len > g_nor_capacity ||
        addr < (uint32_t)LUGALOS_K210_FLASH_WRITABLE_FLOOR)
        return -1;
    ylock_acquire(&g_nor_ylock);
    const uint8_t *in = (const uint8_t *)buf;
    int rc = 0;
    while (len) {
        /* Page program must not cross a 256-byte page. */
        uint32_t chunk = NOR_PAGE_SIZE - (addr % NOR_PAGE_SIZE);
        if (chunk > len) chunk = len;
        if (nor_wait_ready() < 0) { rc = -1; break; }
        nor_write_enable();
        uint8_t frame[4 + NOR_PAGE_SIZE];
        /* NOLINT: filled below, then transmitted in full */
        frame[0] = CMD_PP;
        frame[1] = (uint8_t)(addr >> 16);
        frame[2] = (uint8_t)(addr >> 8);
        frame[3] = (uint8_t)addr;
        memcpy(frame + 4, in, chunk);
        if (spi_xfer(frame, 4 + chunk, 0, 4 + chunk) != 4 + chunk) {
            rc = -1; break;
        }
        nor_settle();
        if (nor_wait_ready() < 0) { rc = -1; break; }
        in += chunk; addr += chunk; len -= chunk;
    }
    ylock_release(&g_nor_ylock);
    return rc;
}

static int nor_erase(uint32_t addr, uint8_t cmd) {
    if (!g_nor_present || addr >= g_nor_capacity ||
        addr < (uint32_t)LUGALOS_K210_FLASH_WRITABLE_FLOOR)
        return -1;
    ylock_acquire(&g_nor_ylock);
    int rc = 0;
    if (nor_wait_ready() < 0) rc = -1;
    if (rc == 0) {
        nor_write_enable();
        uint8_t frame[4] = { cmd, (uint8_t)(addr >> 16),
                             (uint8_t)(addr >> 8), (uint8_t)addr };
        spi_xfer(frame, 4, 0, 4);
        nor_settle();
        if (nor_wait_ready() < 0) rc = -1;
    }
    ylock_release(&g_nor_ylock);
    return rc;
}

int spi_nor_k210_erase_sector(uint32_t addr) {
    if (addr % NOR_SECTOR_SIZE) return -1;
    return nor_erase(addr, CMD_SE);
}

int spi_nor_k210_erase_block(uint32_t addr) {
    if (addr % NOR_BLOCK_SIZE) return -1;
    return nor_erase(addr, CMD_BE);
}
