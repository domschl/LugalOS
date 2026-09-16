/* The ESP32-P4's Ethernet MAC: clocks, pads, PHY reset and MDIO.
 * Z1, plan/phase28_esp32p4_ethernet.md.
 *
 * Provenance, because on this chip it is the whole ballgame (the rule is
 * plan/phase27_esp32p4_bringup.md §3.2, and it has already caught two
 * clock-gate bits that plausible reasoning got wrong): every register offset
 * and bit position below was read off the ESP32-P4 TRM's own bit diagrams and
 * cross-checked against ESP-IDF's generated headers under
 * ~/gith/esp/esp-idf/components/soc/esp32p4/register/hw_ver1/soc/. Where a
 * field mattered enough to be worth a second pair of eyes, the TRM register
 * number is named in the comment.
 *
 * What this file is NOT, yet: a netif. The descriptor rings and their cache
 * maintenance are Z2, auto-negotiation is Z3, frames are Z4. Z1 exists to
 * make exactly one claim testable -- that the PHY is there and answers.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "net/netif.h"
#include "drivers/emac_esp32p4.h"
#include "arch/esp32p4_intr.h"
#include "lugalos_config.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* --- Peripheral bases (TRM Table 9.3-2, IDF soc/reg_base.h) -------------- */

#define HPPERIPH1_BASE      0x500C0000UL
#define GPIO_BASE           (HPPERIPH1_BASE + 0x20000UL)   /* 0x500E0000 */
#define IOMUX_BASE          (HPPERIPH1_BASE + 0x21000UL)   /* 0x500E1000 */
#define HP_SYS_BASE         (HPPERIPH1_BASE + 0x25000UL)   /* 0x500E5000 */
#define HP_SYS_CLKRST_BASE  (HPPERIPH1_BASE + 0x26000UL)   /* 0x500E6000 */
#define LP_CLKRST_BASE      0x50111000UL                   /* LPAON + 0x1000 */

/* --- Clock and reset -----------------------------------------------------
 *
 * TRM §55.5.1 gives this as an ordered procedure, and two of its properties
 * are the reason this function is shaped the way it is.
 *
 * First, the gates are spread across THREE controllers, not one. The bus
 * clock is in HP_SYS_CLKRST_SOC_CLK_CTRL1; the RMII/RX/TX gates and their
 * source-selects are in HP_SYS_CLKRST_PERI_CLK_CTRL00/01; the pad's own
 * always-on gate and the EMAC's peripheral reset are in LP_CLKRST, a
 * different peripheral in a different power domain. There is no position to
 * infer here and no pattern to follow -- this is the same trap phase 27
 * recorded for the UART, whose two gates are SOC_CLK_CTRL1 bit 18 and
 * SOC_CLK_CTRL2 bit 7.
 *
 * Second, the RX/TX divisors depend on the negotiated line speed, which is
 * not known until auto-negotiation finishes. So this function deliberately
 * does NOT claim to finish the clock setup: it establishes everything that is
 * speed-independent and leaves the divisors at their reset value of 1, which
 * is correct for 100 Mbit/s. Z3 revisits them when the link resolves, and
 * that is a sequencing fact rather than an omission.
 */

#define SOC_CLK_CTRL1       (HP_SYS_CLKRST_BASE + 0x18)
#define  EMAC_SYS_CLK_EN        (1u << 13)   /* TRM Reg 11.7 */

#define PERI_CLK_CTRL00     (HP_SYS_CLKRST_BASE + 0x30)
#define  PAD_EMAC_REF_CLK_EN    (1u << 24)   /* TRM Reg 11.13, confirmed */
#define  EMAC_RMII_CLK_SRC_SEL_S 25          /* [26:25] */
#define  EMAC_RMII_CLK_SRC_SEL_M (3u << EMAC_RMII_CLK_SRC_SEL_S)
#define  EMAC_RMII_CLK_EN       (1u << 27)
#define  EMAC_RX_CLK_SRC_SEL    (1u << 28)
#define  EMAC_RX_CLK_EN         (1u << 29)

#define PERI_CLK_CTRL01     (HP_SYS_CLKRST_BASE + 0x34)
#define  EMAC_RX_CLK_DIV_NUM_M  0xffu        /* [7:0] */
#define  EMAC_TX_CLK_SRC_SEL    (1u << 8)
#define  EMAC_TX_CLK_EN         (1u << 9)
#define  EMAC_TX_CLK_DIV_NUM_S  10           /* [17:10] */
#define  EMAC_TX_CLK_DIV_NUM_M  (0xffu << EMAC_TX_CLK_DIV_NUM_S)

#define REF_CLK_CTRL1       (HP_SYS_CLKRST_BASE + 0x28)
#define  REF_50M_CLK_EN         (1u << 27)

/* LP_CLKRST_HP_CLK_CTRL_REG (0x40): the pad's always-on gate. Its reset value
 * is 1, so this is normally already on -- which is precisely why it is worth
 * writing explicitly rather than relying on it. A gate that is enabled by
 * default is a gate nobody notices until something else clears it. */
#define LP_HP_CLK_CTRL      (LP_CLKRST_BASE + 0x40)
#define  HP_PAD_EMAC_TXRX_CLK_EN (1u << 15)

/* LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG (0x4C), TRM Register 11.70, read off
 * the rendered bit diagram: bit 31 FORCE_NORST_EMAC, bit 30 RST_EN_EMAC,
 * bit 29 FORCE_NORST_SDMMC, bit 28 RST_EN_SDMMC, 27:0 reserved, all reset 0.
 * RST_EN is 1 = hold in reset, 0 = release. */
#define LP_SDMMC_EMAC_RST   (LP_CLKRST_BASE + 0x4c)
#define  RST_EN_EMAC            (1u << 30)
#define  FORCE_NORST_EMAC       (1u << 31)

/* HP_SYSTEM_GMAC_CTRL0_REG (+0x14C): PHY_INTF_SEL is bits [4:2], and 4 selects
 * RMII (TRM §55.5.1). */
#define HP_SYS_GMAC_CTRL0   (HP_SYS_BASE + 0x14c)
#define  PHY_INTF_SEL_S         2
#define  PHY_INTF_SEL_M         (7u << PHY_INTF_SEL_S)
#define  PHY_INTF_RMII          (4u << PHY_INTF_SEL_S)

/* --- The MAC's own registers (TRM chapter 55) --------------------------- */

#define EMAC_BASE           ((uintptr_t)CONFIG_EMAC_BASE)
#define EMAC_GMIIADDR       (EMAC_BASE + 0x10)
#define  GMII_GB                (1u << 0)    /* busy; set to start, self-clears */
#define  GMII_GW                (1u << 1)    /* 1 = write, 0 = read */
#define  GMII_CR_S              2            /* [5:2] CSR clock range */
#define  GMII_GR_S              6            /* [10:6] PHY register */
#define  GMII_PA_S              11           /* [15:11] PHY address */
#define EMAC_GMIIDATA       (EMAC_BASE + 0x14)
#define EMAC_BUSMODE        (EMAC_BASE + 0x1000)
#define  BUSMODE_SWR            (1u << 0)

/* The MDC divider.
 *
 * EMAC_CR selects MDC as a division of the CSR clock, which on this chip is
 * the system clock (IDF emac_ll_get_csr_clk_src(): SOC_MOD_CLK_SYS). This
 * kernel has never configured the P4's clock tree -- phase 27 deliberately
 * drove the console from the crystal so that it would not have to -- so the
 * system clock is whatever the boot ROM left, and this file does not know it.
 *
 * Rather than guess, take the largest divider the field offers: 0b0101 is
 * CSR/124. The asymmetry is what makes this safe rather than lazy. IEEE 802.3
 * caps MDC at 25 MHz, so a divider that is too small breaks the bus, while
 * one that is too large only makes each transaction slower -- at the P4's
 * maximum 400 MHz that is 3.2 MHz, and at the crystal's 40 MHz it is 323 kHz.
 * A scan of 32 addresses costs microseconds either way.
 *
 * Z3 may revisit this once something in this tree knows the system clock.
 * Until then the conservative choice is the honest one. */
#define GMII_CR_CSR_DIV_124     (5u << GMII_CR_S)

/* --- IO_MUX and the GPIO matrix (TRM chapter 10) ------------------------
 *
 * The per-pad IO_MUX register is IOMUX_BASE + 0x4 + 4*n, confirmed on this
 * silicon by E1's GPIO20 toggle before it was ever used here. MCU_SEL picks
 * which peripheral owns the pad; every EMAC RMII pad is function 3, and
 * function 1 is plain GPIO / matrix (IDF's PIN_FUNC_GPIO). */
#define IOMUX_PAD(n)        (IOMUX_BASE + 0x4 + 4u * (uint32_t)(n))
#define  IOMUX_MCU_SEL_S        12
#define  IOMUX_MCU_SEL_M        (7u << IOMUX_MCU_SEL_S)
#define  IOMUX_FUN_IE           (1u << 9)
#define  IOMUX_FUN_PU           (1u << 8)
#define  IOMUX_FUN_PD           (1u << 7)
/* "Pulse width shorter than 2 clock cycles will be filtered out" (IO_MUX pad
 * register, bit 15). Harmless at MDIO speeds and emphatically not something
 * to leave to chance on a 50 MHz RMII input. */
#define  IOMUX_FILTER_EN        (1u << 15)
#define  IOMUX_FUN_DRV_S        10           /* [11:10] */
#define  IOMUX_FUN_DRV_M        (3u << IOMUX_FUN_DRV_S)
#define  IOMUX_FUNC_GPIO        1u
#define  IOMUX_FUNC_EMAC        3u

/* Per-pad output routing: which signal drives this pad. 256 means "plain
 * GPIO", which is the reset value. */
#define GPIO_OUT_SEL(n)     (GPIO_BASE + 0x558 + 4u * (uint32_t)(n))
#define  GPIO_OUT_SEL_GPIO      256u
/* Per-SIGNAL input routing: which pad feeds this peripheral input. Bit 7
 * ("do not bypass GPIO") must be set for the matrix to be used at all --
 * clearing it bypasses the matrix, which is the default and is not what a
 * matrix-routed signal wants. */
#define GPIO_IN_SEL(sig)    (GPIO_BASE + 0x158 + 4u * (uint32_t)(sig))
#define  GPIO_IN_SEL_ROUTE      (1u << 7)

/* The P4 has 57 GPIOs, so every "one bit per pin" register is TWO registers:
 * a low word for pins 0..31 and a high word for 32..56. This board puts four
 * EMAC signals above the split (TX_EN 49, RMII_CLK 50, PHY reset 51, MDIO 52)
 * and MDC at 31, right on the boundary -- so a helper that forgets the high
 * bank is wrong for most of this driver and correct for exactly the pin a
 * casual test would try first.
 *
 * The compiler caught one instance of this (a constant `1u << 51` overflowed
 * and warned); it could not catch the ones behind a runtime pin number, which
 * would have shifted by 52 and quietly written bit 20 of the low word. Hence
 * accessors rather than macros, used everywhere. */
#define GPIO_OUT_W1TS_LO    (GPIO_BASE + 0x8)
#define GPIO_OUT_W1TC_LO    (GPIO_BASE + 0xc)
#define GPIO_OUT_W1TS_HI    (GPIO_BASE + 0x14)
#define GPIO_OUT_W1TC_HI    (GPIO_BASE + 0x18)
#define GPIO_ENABLE_W1TS_LO (GPIO_BASE + 0x24)
#define GPIO_ENABLE_W1TS_HI (GPIO_BASE + 0x30)

/* Sets the pin's bit in whichever half of a paired register it belongs to.
 * `lo`/`hi` are the two register addresses; the caller names the pair. */
static void gpio_bit_set(uintptr_t lo, uintptr_t hi, uint32_t gpio) {
    if (gpio < 32u) REG(lo) = (1u << gpio);
    else            REG(hi) = (1u << (gpio - 32u));
}

/* GPIO-matrix signal indices, IDF soc/gpio_sig_map.h. MDIO is bidirectional,
 * so its pad carries two of them: MDO out and MDI in. */
#define SIG_MII_MDI_IN      107u
#define SIG_MII_MDC_OUT     108u
#define SIG_MII_MDO_OUT     109u

/* --- small helpers ------------------------------------------------------ */

static void reg_modify(uintptr_t addr, uint32_t clear, uint32_t set) {
    uint32_t v = REG(addr);
    v &= ~clear;
    v |= set;
    REG(addr) = v;
}

/* Puts a pad on an IO_MUX function directly (no matrix). `ie` requests the
 * input buffer, which every RMII receive signal and the RMII clock need. */
/* Configures one pad: function select, input buffer, floating, no pin filter.
 *
 * **Drive strength is deliberately left alone**, which is a correction rather
 * than an omission. This used to force FUN_DRV to 3 -- the maximum -- with
 * the reasoning "RMII runs at 50 MHz", which sounds right and is an assumption
 * nobody measured. ESP-IDF configures these same pads
 * (emac_esp_iomux_init(), esp_eth_mac_esp_gpio.c) by setting the IOMUX
 * function and the pull mode and **never touching drive strength at all**,
 * leaving the pad default. On short unterminated board traces the maximum
 * setting is if anything the wrong choice: it buys edge rate at the cost of
 * overshoot and ringing, which is how a 50 MHz signal arrives corrupted.
 *
 * Pulls are cleared explicitly because IDF does the same
 * (gpio_set_pull_mode(GPIO_FLOATING)): an RMII line is driven at both ends
 * and a stray pull-up fights the driver. MDIO is the exception and sets its
 * own pull-up afterwards, because it genuinely idles undriven. */
static void pad_iomux(uint32_t gpio, uint32_t func, bool ie) {
    uint32_t v = REG(IOMUX_PAD(gpio));
    v &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_IE | IOMUX_FUN_PU | IOMUX_FUN_PD |
           IOMUX_FILTER_EN);
    v |= (func << IOMUX_MCU_SEL_S);
    if (ie) v |= IOMUX_FUN_IE;
    REG(IOMUX_PAD(gpio)) = v;
}

/* Routes a peripheral output signal onto a pad through the matrix. */
static void matrix_out(uint32_t gpio, uint32_t sig) {
    pad_iomux(gpio, IOMUX_FUNC_GPIO, false);
    REG(GPIO_OUT_SEL(gpio)) = sig;
    gpio_bit_set(GPIO_ENABLE_W1TS_LO, GPIO_ENABLE_W1TS_HI, gpio);
}

/* Routes a pad into a peripheral input signal through the matrix. */
static void matrix_in(uint32_t gpio, uint32_t sig) {
    reg_modify(IOMUX_PAD(gpio), 0, IOMUX_FUN_IE);
    REG(GPIO_IN_SEL(sig)) = GPIO_IN_SEL_ROUTE | (gpio & 0x3fu);
}

/* A plain push-pull GPIO output, for the PHY's reset line. */
static void gpio_output(uint32_t gpio, bool level) {
    pad_iomux(gpio, IOMUX_FUNC_GPIO, false);
    REG(GPIO_OUT_SEL(gpio)) = GPIO_OUT_SEL_GPIO;
    if (level) gpio_bit_set(GPIO_OUT_W1TS_LO, GPIO_OUT_W1TS_HI, gpio);
    else       gpio_bit_set(GPIO_OUT_W1TC_LO, GPIO_OUT_W1TC_HI, gpio);
    gpio_bit_set(GPIO_ENABLE_W1TS_LO, GPIO_ENABLE_W1TS_HI, gpio);
}

/* --- bring-up ----------------------------------------------------------- */

static void emac_clocks_on(void) {
    /* 1. The bus clock. Without it the EMAC's registers do not even decode. */
    reg_modify(SOC_CLK_CTRL1, 0, EMAC_SYS_CLK_EN);

    /* 2. Release the peripheral from reset. RST_EN is 1 = hold, 0 = release;
     *    both it and FORCE_NORST reset to 0, so this is normally a no-op, and
     *    is written anyway so that a second emac_probe() after a failed one
     *    starts from a known state. */
    reg_modify(LP_SDMMC_EMAC_RST, RST_EN_EMAC | FORCE_NORST_EMAC, 0);

    /* 3. RMII, not MII. */
    reg_modify(HP_SYS_GMAC_CTRL0, PHY_INTF_SEL_M, PHY_INTF_RMII);

    /* 4. The reference clock comes FROM the PHY on this board (its own 25 MHz
     *    crystal, 50 MHz out of M_CLKO), so the P4 must not drive a clock onto
     *    that pad. TRM §55.5.1's "reference clock sourced from the external
     *    crystal" branch: disable PAD_EMAC_REF_CLK and PLL_F50M. Driving both
     *    ends of a clock net is not a subtle failure, but it is an avoidable
     *    one. */
    reg_modify(PERI_CLK_CTRL00, PAD_EMAC_REF_CLK_EN, 0);
    reg_modify(REF_CLK_CTRL1, REF_50M_CLK_EN, 0);

    /* 5. The pad's always-on gate, in the LP domain. Reset value is 1. */
    reg_modify(LP_HP_CLK_CTRL, 0, HP_PAD_EMAC_TXRX_CLK_EN);

    /* 6. Source RMII/RX/TX from the pad, and enable the three gates. Source
     *    select 0 is PAD_EMAC_TXRX_CLK for all three (TRM §55.5.1's RMII
     *    branch names PAD_EMAC_TXRX_CLK as the source in each case), which is
     *    also their reset value -- written explicitly for the same reason as
     *    step 2.
     *
     *    The divisors are left at their reset value of 1: correct for
     *    100 Mbit/s, and revisited by Z3 when auto-negotiation says what the
     *    line speed actually is. */
    reg_modify(PERI_CLK_CTRL00,
               EMAC_RMII_CLK_SRC_SEL_M | EMAC_RX_CLK_SRC_SEL,
               EMAC_RMII_CLK_EN | EMAC_RX_CLK_EN);
    reg_modify(PERI_CLK_CTRL01, EMAC_TX_CLK_SRC_SEL, EMAC_TX_CLK_EN);
}

static void emac_pads(void) {
    /* RMII data and clock: IO_MUX function 3, direct, no matrix. Inputs need
     * the input buffer; outputs do not. The board file explains at length why
     * these particular numbers are not interchangeable. */
    pad_iomux(CONFIG_EMAC_RMII_CLK_GPIO, IOMUX_FUNC_EMAC, true);
    pad_iomux(CONFIG_EMAC_CRS_DV_GPIO,   IOMUX_FUNC_EMAC, true);
    pad_iomux(CONFIG_EMAC_RXD0_GPIO,     IOMUX_FUNC_EMAC, true);
    pad_iomux(CONFIG_EMAC_RXD1_GPIO,     IOMUX_FUNC_EMAC, true);
    pad_iomux(CONFIG_EMAC_TX_EN_GPIO,    IOMUX_FUNC_EMAC, false);
    pad_iomux(CONFIG_EMAC_TXD0_GPIO,     IOMUX_FUNC_EMAC, false);
    pad_iomux(CONFIG_EMAC_TXD1_GPIO,     IOMUX_FUNC_EMAC, false);

    /* Station management goes through the GPIO matrix, because MDC and MDIO
     * have no IO_MUX pad of their own on this chip. MDIO is bidirectional and
     * therefore carries both directions on one pad. */
    matrix_out(CONFIG_EMAC_MDC_GPIO,  SIG_MII_MDC_OUT);
    matrix_out(CONFIG_EMAC_MDIO_GPIO, SIG_MII_MDO_OUT);
    matrix_in(CONFIG_EMAC_MDIO_GPIO,  SIG_MII_MDI_IN);
    /* MDIO idles high and is driven by both ends; the pull-up matters when
     * neither is driving. */
    reg_modify(IOMUX_PAD(CONFIG_EMAC_MDIO_GPIO), 0, IOMUX_FUN_PU);
}

/* The IP101GRI's reset is active low. Held low, then released, then given
 * time before MDIO is attempted -- the part needs the reference clock stable
 * and its own internal reset to finish before it will answer. The figures
 * here are generous rather than calibrated: this runs once at bring-up, and
 * the cost of being slow is nothing while the cost of being early is a scan
 * that finds nothing and sends the reader looking for a wiring fault. */
static void phy_reset(void) {
    gpio_output(CONFIG_EMAC_PHY_RST_GPIO, false);
    time_delay_us(20000);
    gpio_bit_set(GPIO_OUT_W1TS_LO, GPIO_OUT_W1TS_HI, CONFIG_EMAC_PHY_RST_GPIO);
    time_delay_us(100000);
}

int emac_probe(void) {
    emac_clocks_on();
    emac_pads();
    phy_reset();

    /* The MAC's software reset.
     *
     * This is the step that will fail if anything earlier is wrong, and it
     * fails in a specific and useful way. The DWC_EMAC's own register
     * description says it outright: "The reset operation is completed only
     * when all resets in all active clock domains are deasserted Therefore,
     * it is essential that all PHY inputs clocks (applicable for the selected
     * PHY interface) are present for the software reset completion."
     *
     * In other words SWR does not clear until the PHY is supplying the 50 MHz
     * RMII reference. That is why phy_reset() runs BEFORE this and not after,
     * and it makes this bit an excellent single test of the whole clock and
     * pad configuration: if it clears, the reference clock is arriving.
     *
     * Bounded, and the bound is the diagnosis. */
    REG(EMAC_BUSMODE) |= BUSMODE_SWR;
    for (unsigned i = 0; i < 100000u; i++) {
        if ((REG(EMAC_BUSMODE) & BUSMODE_SWR) == 0) {
            return 0;
        }
    }
    return -1;
}

/* --- station management ------------------------------------------------- */

static int mdio_wait_idle(void) {
    for (unsigned i = 0; i < 100000u; i++) {
        if ((REG(EMAC_GMIIADDR) & GMII_GB) == 0) return 0;
    }
    return -1;
}

int emac_mdio_read(uint8_t phy_addr, uint8_t reg) {
    if (mdio_wait_idle() != 0) return -1;
    REG(EMAC_GMIIADDR) =
        ((uint32_t)(phy_addr & 0x1fu) << GMII_PA_S) |
        ((uint32_t)(reg & 0x1fu) << GMII_GR_S) |
        GMII_CR_CSR_DIV_124 | GMII_GB;
    if (mdio_wait_idle() != 0) return -1;
    return (int)(REG(EMAC_GMIIDATA) & 0xffffu);
}

int emac_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val) {
    if (mdio_wait_idle() != 0) return -1;
    REG(EMAC_GMIIDATA) = val;
    REG(EMAC_GMIIADDR) =
        ((uint32_t)(phy_addr & 0x1fu) << GMII_PA_S) |
        ((uint32_t)(reg & 0x1fu) << GMII_GR_S) |
        GMII_CR_CSR_DIV_124 | GMII_GW | GMII_GB;
    return mdio_wait_idle();
}

/* --- the scan ------------------------------------------------------------
 *
 * Clause 22 register 2 is PHYIDR1 and register 3 is PHYIDR2; together they
 * carry the OUI and the vendor's model and revision. An address with nothing
 * on it reads back 0xFFFF (MDIO idles high and nobody drove it low) or
 * 0x0000; anything else is a part.
 *
 * This is a table a human asked for, so it goes to the console with cprintf()
 * rather than into the kernel log -- the same call i2c_scan_bus() makes, for
 * the same reason. */
void emac_phy_scan(void) {
    int rc = emac_probe();
    if (rc != 0) {
        /* The diagnosis, kept here rather than printed: SWR cannot clear
         * until the PHY supplies the 50 MHz RMII reference, so this means the
         * reference clock (CONFIG_EMAC_RMII_CLK_GPIO), the PHY's reset line
         * (CONFIG_EMAC_PHY_RST_GPIO), or one of the clock gates -- and not
         * MDIO, which has not been attempted yet. Every runtime string on
         * this board is RAM-resident (linker/esp32p4.ld), so prose lives in
         * the source and the message carries the facts. */
        cprintf("[EMAC] probe failed (%d): MAC reset never completed; "
                "no RMII reference clock?\n", rc);
        return;
    }

    cprintf("EMAC at 0x%lx, MDIO on MDC=GPIO%d MDIO=GPIO%d. Scanning 0..31:\n",
            (unsigned long)EMAC_BASE, CONFIG_EMAC_MDC_GPIO,
            CONFIG_EMAC_MDIO_GPIO);

    unsigned found = 0;
    bool seen_expected = false;
    for (uint8_t a = 0; a < 32; a++) {
        int id1 = emac_mdio_read(a, 2);
        int id2 = emac_mdio_read(a, 3);
        if (id1 < 0 || id2 < 0) {
            cprintf("  %2u: MDIO stayed busy\n", (unsigned)a);
            continue;
        }
        if ((id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0)) continue;
        /* Clause 22: PHYIDR1 carries OUI bits 3..18, PHYIDR2 carries OUI
         * bits 19..24 in [15:10], the vendor's model in [9:4] and the
         * revision in [3:0]. Reassembling the OUI is worth the three lines --
         * it turns "some part answered" into a manufacturer a reader can look
         * up. Printed raw as well as decoded, because the raw pair is what
         * compares against a datasheet. */
        uint32_t oui = ((uint32_t)id1 << 6) | (((uint32_t)id2 >> 10) & 0x3fu);
        cprintf("  %2u: ID 0x%04x/0x%04x OUI %02x-%02x-%02x model %u rev %u\n",
                (unsigned)a, (unsigned)id1, (unsigned)id2,
                (unsigned)((oui >> 16) & 0xffu), (unsigned)((oui >> 8) & 0xffu),
                (unsigned)(oui & 0xffu),
                (unsigned)((id2 >> 4) & 0x3f), (unsigned)(id2 & 0xf));
        found++;
        if (a == (uint8_t)CONFIG_EMAC_PHY_ADDR) {
            seen_expected = true;
            if (id1 != CONFIG_EMAC_PHY_ID1 || id2 != CONFIG_EMAC_PHY_ID2) {
                cprintf("      ^ WRONG PART: board file says 0x%04x/0x%04x\n",
                        (unsigned)CONFIG_EMAC_PHY_ID1,
                        (unsigned)CONFIG_EMAC_PHY_ID2);
            }
        }
    }

    /* The board file's CONFIG_EMAC_PHY_ADDR was measured here in Z1. Saying
     * so on every run is what keeps it a claim instead of a comment: a
     * swapped board, a re-strapped PHY or a dead part all show up as a
     * disagreement rather than as a mysterious failure three milestones
     * later. */
    if (found == 0) {
        /* The MAC reset completed, so the reference clock is arriving; that
         * leaves MDC/MDIO routing or the PHY's address straps. */
        cprintf("  nothing answered (clock is fine; suspect MDC/MDIO or straps)\n");
    } else if (!seen_expected) {
        cprintf("  %u PHY%s responded, but NOT at %d where the board file "
                "says it should be.\n",
                found, found == 1 ? "" : "s", (int)CONFIG_EMAC_PHY_ADDR);
    } else if (found == 1) {
        cprintf("  1 PHY at %d, as the board file says. OK.\n",
                (int)CONFIG_EMAC_PHY_ADDR);
    } else {
        cprintf("  %u PHYs incl. the expected one at %d (more than one is "
                "unexpected here)\n", found, (int)CONFIG_EMAC_PHY_ADDR);
    }
}

/* ======================================================================
 * Z2 -- descriptor rings, cache maintenance, and MAC internal loopback.
 * ====================================================================== */

/* --- The descriptors ----------------------------------------------------
 *
 * The DWC_EMAC's "enhanced" descriptor is 32 bytes: two status/control words,
 * a buffer pointer, a second pointer, and four words of timestamp and
 * reserved space. That is the layout the hardware reads.
 *
 * They are declared as 64 bytes here anyway, and the padding is load-bearing.
 * The CPU and the DMA both write these words -- the CPU sets up a descriptor
 * and hands it over, the DMA writes status back -- so every descriptor needs
 * cache maintenance, and an invalidate discards a whole 64-byte line. If two
 * 32-byte descriptors shared a line, invalidating one to read its status
 * would throw away the neighbour's setup. One descriptor per line makes that
 * impossible rather than merely unlikely. (ESP-IDF pads to 64 on this chip
 * for the same reason, commented only as "due to cache arrangement".)
 *
 * Because the descriptors are 64 bytes and the DMA expects 32, the ring is
 * built in CHAINED mode rather than ring mode: each descriptor's second
 * pointer holds the address of its successor and the last points back at the
 * first, so the DMA never computes a stride and the padding costs nothing but
 * memory. The alternative -- ring mode with DESC_SKIP_LEN set to 8 words --
 * would work too, and is one more number to get wrong.
 */

typedef struct {
    volatile uint32_t des0;     /* status + control; bit 31 is OWN */
    volatile uint32_t des1;     /* buffer sizes and flags */
    volatile uint32_t buf;      /* buffer 1 address */
    volatile uint32_t next;     /* chained mode: address of the next descriptor */
    volatile uint32_t ext;      /* RDES4 extended status; unused in TX */
    volatile uint32_t rsvd;
    volatile uint32_t ts_lo;    /* IEEE 1588 timestamp -- phase 29, not read here */
    volatile uint32_t ts_hi;
    uint8_t pad[ESP32P4_L1_CACHE_LINE - 32];
} emac_desc_t;

_Static_assert(sizeof(emac_desc_t) == ESP32P4_L1_CACHE_LINE,
               "a descriptor must occupy exactly one cache line");

/* TDES0 */
#define TDES0_OWN           (1u << 31)
#define TDES0_IC            (1u << 30)  /* interrupt on completion */
#define TDES0_LS            (1u << 29)  /* last segment */
#define TDES0_FS            (1u << 28)  /* first segment */
#define TDES0_TCH           (1u << 20)  /* second address chained */
#define TDES0_ES            (1u << 15)  /* error summary */
/* What ES summarises on the transmit side. UF is the one to suspect when
 * frames go missing without the driver noticing: the MAC starts putting a
 * frame on the wire once TTC bytes are buffered, so a DMA that falls behind
 * aborts mid-frame and the far end sees nothing but a runt. */
#define TDES0_UF            (1u << 1)   /* underflow: FIFO ran dry mid-frame */
#define TDES0_ED            (1u << 2)   /* excessive deferral */
#define TDES0_EC            (1u << 8)   /* excessive collisions */
#define TDES0_LC            (1u << 9)   /* late collision */
#define TDES0_NC            (1u << 10)  /* no carrier */
#define TDES0_LOC           (1u << 11)  /* loss of carrier */
#define TDES0_IHE           (1u << 16)  /* IP header error */
/* TDES1 */
#define TDES1_TBS1_M        0x1fffu     /* buffer 1 size, [12:0] */

/* RDES0 */
#define RDES0_OWN           (1u << 31)
#define RDES0_ES            (1u << 15)  /* error summary */
#define RDES0_FL_S          16          /* frame length, [29:16] */
#define RDES0_FL_M          0x3fffu
#define RDES0_LS            (1u << 8)   /* last descriptor */
#define RDES0_FS            (1u << 9)   /* first descriptor */
/* The error bits ES summarises, named so a rejected frame can say *why*
 * rather than just that it failed. */
#define RDES0_CE            (1u << 1)   /* CRC error */
#define RDES0_DBE           (1u << 2)   /* dribble bit error */
#define RDES0_RE            (1u << 3)   /* receive error (RMII RX_ER) */
#define RDES0_RWT           (1u << 4)   /* receive watchdog timeout */
#define RDES0_OE            (1u << 11)  /* overflow: the RX FIFO overran */
#define RDES0_LE            (1u << 12)  /* length error */
#define RDES0_SAF           (1u << 13)  /* source address filter fail */
#define RDES0_DE            (1u << 14)  /* descriptor error: ran out mid-frame */
/* RDES1 */
#define RDES1_RCH           (1u << 14)  /* second address chained */
#define RDES1_RBS1_M        0x1fffu

/* --- Ring storage -------------------------------------------------------
 *
 * Static .bss, like every other allocation on this path -- net/netif.h's
 * contract is explicit that a driver owns its storage and that there is no
 * allocation here.
 *
 * Sizes, and what they cost. On this board .bss and the heap come out of the
 * same 384 KB (linker/esp32p4.ld), so every byte here is a byte palloc does
 * not get -- the same trade the RP2350 board file describes:
 *
 *     4 RX descriptors + 2 TX descriptors   6 * 64  =    384 B
 *     4 RX buffers + 2 TX buffers           6 * 1536 = 9216 B
 *                                                     ------
 *                                                      9600 B  (9.4 KB)
 *
 * Four RX and two TX is small on purpose: Z2 proves the mechanism, and a
 * deeper ring proves nothing more while costing the heap. Z4 revisits the
 * depth against real traffic rather than against a guess.
 *
 * 1536 is 24 whole cache lines and comfortably over NETIF_FRAME_MAX (1514).
 * Being a multiple of the line size is the point: it means a buffer never
 * shares a cache line with the next buffer, so invalidating one after the DMA
 * has written it cannot discard part of another.
 */
#define EMAC_RX_DESC_COUNT  4u
#define EMAC_TX_DESC_COUNT  2u
#define EMAC_BUF_SIZE       1536u

_Static_assert(EMAC_BUF_SIZE % ESP32P4_L1_CACHE_LINE == 0,
               "a DMA buffer must be a whole number of cache lines, or "
               "invalidating one will discard part of its neighbour");

#define CACHE_ALIGNED __attribute__((aligned(ESP32P4_L1_CACHE_LINE)))

static CACHE_ALIGNED emac_desc_t g_rx_desc[EMAC_RX_DESC_COUNT];
static CACHE_ALIGNED emac_desc_t g_tx_desc[EMAC_TX_DESC_COUNT];
static CACHE_ALIGNED uint8_t     g_rx_buf[EMAC_RX_DESC_COUNT][EMAC_BUF_SIZE];
static CACHE_ALIGNED uint8_t     g_tx_buf[EMAC_TX_DESC_COUNT][EMAC_BUF_SIZE];

static uint32_t g_rx_next;   /* the descriptor we will look at next */
static uint32_t g_tx_next;

/* --- DMA and MAC registers beyond the ones Z1 needed ------------------- */

#define EMAC_MACCONFIG      (EMAC_BASE + 0x0000)
#define  MACCFG_RE              (1u << 2)
#define  MACCFG_TE              (1u << 3)
#define  MACCFG_ACS             (1u << 7)   /* strip pad/CRC on receive */
#define  MACCFG_DM              (1u << 11)  /* full duplex */
#define  MACCFG_LM              (1u << 12)  /* MAC internal loopback */
#define  MACCFG_FES             (1u << 14)  /* 1 = 100 Mbit/s */
#define  MACCFG_PS              (1u << 15)  /* port select: MII/RMII 10-100 */
#define  MACCFG_JD              (1u << 22)  /* jabber disable */
#define  MACCFG_WD              (1u << 23)  /* watchdog disable */
#define EMAC_MACFRAMEFILTER (EMAC_BASE + 0x0004)
#define  FILTER_PR              (1u << 0)   /* promiscuous */
#define  FILTER_PM              (1u << 4)   /* pass all multicast */
#define  FILTER_DBF             (1u << 5)   /* 1 = BLOCK broadcast; note the sense */
#define  FILTER_RA              (1u << 31)  /* receive all, filter nothing */

/* The station address the receive filter matches against (Z4). Offsets and
 * byte order from IDF's emac_ll_set_addr() and the generated
 * soc/emac_reg.h, not from inference -- getting the halves backwards would
 * produce a filter that silently matches nothing.
 *
 * LOW holds bytes 0..3 little-endian; HIGH's low 16 bits hold bytes 4 and 5.
 * HIGH bit 31 (AE, "address enable") is documented read-only and always 1 on
 * this core, so address 0 cannot be disabled and nothing needs to set it. */
#define EMAC_MACADDR0HIGH   (EMAC_BASE + 0x0040)
#define EMAC_MACADDR0LOW    (EMAC_BASE + 0x0044)

/* Note the order: RX base comes BEFORE TX base. Swapping them is an easy
 * mistake with a confusing symptom (the DMA walks the wrong list and nothing
 * is ever owned), so they are named rather than computed. */
#define EMAC_DMA_TXPOLL     (EMAC_BASE + 0x1004)
#define EMAC_DMA_RXPOLL     (EMAC_BASE + 0x1008)
#define EMAC_DMA_RXBASE     (EMAC_BASE + 0x100c)
#define EMAC_DMA_TXBASE     (EMAC_BASE + 0x1010)
#define EMAC_DMA_STATUS     (EMAC_BASE + 0x1014)
/* Frames the controller itself threw away: bits 15:0 count "no descriptor was
 * free", bit 16 is that counter's overflow, bits 27:17 count frames lost to
 * the receive FIFO overflowing. The distinction matters when frames go
 * missing -- a DA-filter reject is invisible here, so a zero in this register
 * alongside frames that never arrived means they were filtered out, not
 * dropped for want of a buffer. */
#define EMAC_DMA_MISSEDFRAME (EMAC_BASE + 0x1020)
#define EMAC_DMA_OPMODE     (EMAC_BASE + 0x1018)
#define  OPMODE_SR              (1u << 1)   /* start receive */
#define  OPMODE_ST              (1u << 13)  /* start transmit */
#define  OPMODE_FTF             (1u << 20)  /* flush TX FIFO (self-clearing) */
#define  OPMODE_TSF             (1u << 21)  /* transmit store-and-forward */
#define  OPMODE_RSF             (1u << 25)  /* receive store-and-forward */
/* Threshold fields, used because store-and-forward is not available here:
 * RTC is [4:3] and TTC is [16:14], and zero in both means a 64-byte
 * threshold, which is what this driver wants. Named so the zeroes below are
 * visibly deliberate rather than merely absent. */
#define  OPMODE_RTC_64          (0u << 3)
#define  OPMODE_TTC_64          (0u << 14)

/* BUSMODE bits beyond SWR */
#define  BUSMODE_ATDS           (1u << 7)   /* 32-byte (enhanced) descriptors */
#define  BUSMODE_PBL_S          8
#define  BUSMODE_FB             (1u << 16)  /* fixed burst */
#define  BUSMODE_AAL            (1u << 25)  /* address-aligned beats */
#define  BUSMODE_MB             (1u << 26)  /* mixed burst */

/* --- cache helpers, named for what they mean ---------------------------- */

/* These are proven load-bearing, not defensive.
 *
 * Z2 measured it: with both primitives stubbed out to no-ops and nothing else
 * changed, the loopback test does not merely lose a frame occasionally, it
 * fails completely and immediately --
 *
 *     60 B: nothing came back (0), DMA status 0x00680084
 *     64 B: nothing came back (0), DMA status 0x00680084
 *     65 B: send refused (-2)      <- and every size after it
 *
 * `-2` is emac_tx() finding OWN still set. The DMA had cleared it in memory;
 * the CPU was reading a stale cache line. That is the whole hazard in one
 * observation, and it is why this driver cannot be written the way the
 * ENC28J60 and CYW43 drivers are.
 *
 * The experiment is two lines if it ever needs repeating: #define both of
 * these away at the top of this file and run `emac loopback`. */
static void desc_to_dma(emac_desc_t *d) {
    esp32p4_dcache_writeback((uintptr_t)d, sizeof(*d));
}
static void desc_from_dma(emac_desc_t *d) {
    esp32p4_dcache_invalidate((uintptr_t)d, sizeof(*d));
}

/* --- ring construction --------------------------------------------------- */

static void emac_rings_init(void) {
    for (uint32_t i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        emac_desc_t *d = &g_rx_desc[i];
        d->des1 = RDES1_RCH | (EMAC_BUF_SIZE & RDES1_RBS1_M);
        d->buf  = (uint32_t)(uintptr_t)g_rx_buf[i];
        d->next = (uint32_t)(uintptr_t)&g_rx_desc[(i + 1u) % EMAC_RX_DESC_COUNT];
        d->ext = d->rsvd = d->ts_lo = d->ts_hi = 0;
        /* The buffer is about to belong to the DMA, and the CPU may have
         * dirty lines over it from a previous frame. Invalidate rather than
         * write back: whatever is there is stale by definition, and writing
         * it back would be a pointless bus transaction that could also race
         * the DMA's own write. */
        esp32p4_dcache_invalidate((uintptr_t)g_rx_buf[i], EMAC_BUF_SIZE);
        /* OWN last, then push the whole descriptor out. The order is the
         * point: the DMA must never see OWN set over a half-built
         * descriptor. */
        d->des0 = RDES0_OWN;
        desc_to_dma(d);
    }
    for (uint32_t i = 0; i < EMAC_TX_DESC_COUNT; i++) {
        emac_desc_t *d = &g_tx_desc[i];
        d->des0 = TDES0_TCH;            /* chained; not owned by the DMA yet */
        d->des1 = 0;
        d->buf  = (uint32_t)(uintptr_t)g_tx_buf[i];
        d->next = (uint32_t)(uintptr_t)&g_tx_desc[(i + 1u) % EMAC_TX_DESC_COUNT];
        d->ext = d->rsvd = d->ts_lo = d->ts_hi = 0;
        desc_to_dma(d);
    }
    g_rx_next = 0;
    g_tx_next = 0;

    REG(EMAC_DMA_RXBASE) = (uint32_t)(uintptr_t)g_rx_desc;
    REG(EMAC_DMA_TXBASE) = (uint32_t)(uintptr_t)g_tx_desc;
}

/* Defined with the netif, further down: drops any half-received frame and
 * held by the netif, whose descriptor the DMA has just taken back. */
static void emac_netif_restarted(void);

/* Set while a loopback diagnostic owns the descriptor rings.
 *
 * `emac loopback` and `emac loopback stress` drive the rings directly, and
 * since Z4 registered eth0 they have been sharing them with a live netif:
 * net/stack.c's `netsrv` task polls emac_netif_poll() forever, and every
 * frame it takes is a frame the diagnostic sent and will never see come back.
 *
 * Measured rather than supposed -- during one 700-frame stress run the
 * interface's own rx counter climbed by 65, and two identical runs scored
 * 450/700 and 674/700, which is the signature of a race and not of hardware.
 * The Z2 test survived this undetected because seven frames finish inside
 * about one scheduler slice; a long run does not.
 *
 * The netif yields rather than locks: poll() simply reports "nothing" while
 * this is set, which is always a legal answer for a non-blocking poll. */
static volatile bool g_diag_owns_rings;

/* Brings the DMA and MAC up. `loopback` selects the MAC's internal loopback,
 * which is what lets Z2 prove the rings with no PHY and no cable in the
 * picture -- one candidate cause per failure, which is phase 27 §0's rule. */
static void emac_start(bool loopback) {
    /* Enhanced (32-byte) descriptors, mixed and fixed bursts, aligned beats,
     * burst length 32. */
    reg_modify(EMAC_BUSMODE, (0x3fu << BUSMODE_PBL_S),
               BUSMODE_ATDS | BUSMODE_FB | BUSMODE_AAL | BUSMODE_MB |
               (32u << BUSMODE_PBL_S));

    emac_rings_init();

    /* Threshold mode, NOT store-and-forward, and this is not a preference.
     *
     * Z2's first run failed every frame of 512 bytes and up with the DMA
     * status reporting receive overflow (bit 4), because store-and-forward
     * holds a whole frame in the MTL FIFO before releasing it and **this
     * chip's receive FIFO is 256 bytes**. A 512-byte frame cannot fit, so it
     * overflows and is dropped. ESP-IDF disables RSF for every target except
     * the original ESP32, with the one-line reason "Rx FIFO is only 256B".
     *
     * In threshold mode the DMA starts moving bytes out once 64 are buffered,
     * so frame size stops being bounded by the FIFO. "One descriptor, one
     * frame" still holds -- that is a property of the buffer being large
     * enough, not of store-and-forward.
     *
     * Flush the TX FIFO first; the bit self-clears. */
    REG(EMAC_DMA_OPMODE) = OPMODE_FTF;
    for (unsigned i = 0; i < 10000u && (REG(EMAC_DMA_OPMODE) & OPMODE_FTF); i++) {
        /* spin */
    }
    REG(EMAC_DMA_OPMODE) = OPMODE_RTC_64 | OPMODE_TTC_64;

    /* 100 Mbit/s full duplex, MII/RMII port, jabber and watchdog off (they
     * police frame sizes this driver deliberately walks up to).
     *
     * ACS -- automatic pad/CRC stripping -- is deliberately NOT set, and the
     * reason is worth stating because setting it looks like exactly what
     * net/netif.h wants ("no FCS ... both check and strip it"). The DWC_EMAC
     * strips only when the length/type field is **less than 1536**, i.e. only
     * for 802.3 length-framed packets. Every Ethernet II frame -- which is all
     * of them here, IPv4 is type 0x0800 = 2048 -- is above that, so ACS would
     * do nothing for real traffic while appearing to solve the problem.
     *
     * So the FCS is stripped in software instead, unconditionally, in
     * emac_rx(). Z2's first run showed this as every frame coming back
     * exactly four bytes long. */
    uint32_t cfg = MACCFG_PS | MACCFG_FES | MACCFG_DM |
                   MACCFG_JD | MACCFG_WD;
    if (loopback) cfg |= MACCFG_LM;
    REG(EMAC_MACCONFIG) = cfg;

    /* In loopback, filter nothing: the destination address is whatever the Z2
     * test wrote, and refusing frames there would mean debugging the filter
     * instead of the rings.
     *
     * On the wire (Z4), a filter register of **zero** is exactly what this
     * driver wants, which is worth spelling out because "0" reads like "not
     * configured yet":
     *
     *   PR  = 0  not promiscuous
     *   HUC = 0  unicast destinations are matched *perfectly* against
     *            Address0 -- the address emac_set_mac() programmed -- rather
     *            than through the hash table
     *   PM  = 0  and HMC = 0, so multicast is dropped
     *   DBF = 0  broadcast passes. The sense is inverted: the bit *disables*
     *            broadcast, so leaving it clear is what lets ARP work
     *   PCF = 00 MAC control frames (pause) are handled by the MAC and never
     *            reach us, which is what we want with flow control off
     *
     * That is precisely phase 28 Z4's requirement -- our unicast plus
     * broadcast -- with the hardware doing the comparison instead of a
     * memcmp() on every arriving frame. */
    REG(EMAC_MACFRAMEFILTER) = loopback ? FILTER_RA : 0u;

    REG(EMAC_MACCONFIG) = cfg | MACCFG_TE | MACCFG_RE;
    REG(EMAC_DMA_OPMODE) |= OPMODE_ST | OPMODE_SR;

    /* The rings were just reinitialised, so any descriptor the netif was
     * holding is now owned by the DMA again -- forget it rather than hand its
     * buffer up later. */
    emac_netif_restarted();
}

/* --- one frame out ------------------------------------------------------ */

/* Returns the buffer the next transmit will send from, or NULL if the DMA
 * still owns that descriptor. Fill it, then call emac_tx_submit().
 *
 * Split out from emac_tx() so a caller that generates its payload (the
 * loopback selftest) can write straight into the DMA buffer instead of
 * building it somewhere else and having it copied. That removes 3 KB of .bss
 * -- which on this board is 3 KB the heap gets to keep -- and it makes the
 * selftest stronger, because a test with no second copy of the data cannot
 * accidentally compare a buffer against itself. */
/* Transmit failures the MAC reported after the fact. A frame that fails on
 * the wire is not refused by emac_tx_submit() -- that only hands the
 * descriptor over -- so without looking at the status the DMA wrote back, a
 * lost frame is invisible to everything above. */
static uint32_t g_tx_hw_errors;
static uint32_t g_tx_err_last_des0;
static uint32_t g_tx_err_seen_bits;

static uint8_t *emac_tx_buffer(void) {
    emac_desc_t *d = &g_tx_desc[g_tx_next];
    desc_from_dma(d);
    if (d->des0 & TDES0_OWN) return NULL;

    /* The DMA has finished with this descriptor and written its status back.
     * Read it before the buffer is refilled -- this is the only moment the
     * outcome of the *previous* transmission through this slot is still
     * legible. */
    if (d->des0 & TDES0_ES) {
        g_tx_err_last_des0 = d->des0;
        g_tx_err_seen_bits |= d->des0;
        g_tx_hw_errors++;
    }
    return g_tx_buf[g_tx_next];
}

static int emac_tx_submit(uint32_t len) {
    if (len == 0 || len > EMAC_BUF_SIZE) return -1;
    emac_desc_t *d = &g_tx_desc[g_tx_next];

    /* The CPU just wrote the buffer; push it to memory before the DMA reads
     * it. This is the half that is easy to forget and whose absence looks
     * like corrupted frames rather than like a cache bug. */
    esp32p4_dcache_writeback((uintptr_t)g_tx_buf[g_tx_next], len);

    d->des1 = len & TDES1_TBS1_M;
    d->des0 = TDES0_TCH | TDES0_FS | TDES0_LS | TDES0_IC | TDES0_OWN;
    desc_to_dma(d);

    g_tx_next = (g_tx_next + 1u) % EMAC_TX_DESC_COUNT;

    /* Tell the DMA to look again; it may have suspended on an unowned
     * descriptor. Any value will do -- the register is a doorbell. */
    REG(EMAC_DMA_TXPOLL) = 1;
    return 0;
}


/* --- one frame in ------------------------------------------------------- */

/* Returns a pointer to the buffer holding the next received frame and its
 * length in *len, or NULL if none is ready or it errored. The caller must
 * call emac_rx_release() when done with the bytes; until then the descriptor
 * is not handed back to the DMA.
 *
 * The zero-copy half of the same split as emac_tx_buffer(): the selftest
 * verifies straight out of the DMA buffer. */
static const uint8_t *emac_rx_peek(uint32_t *len_out) {
    emac_desc_t *d = &g_rx_desc[g_rx_next];

    desc_from_dma(d);
    if (d->des0 & RDES0_OWN) return NULL;  /* nothing yet */

    uint32_t st = d->des0;
    uint32_t len = (st >> RDES0_FL_S) & RDES0_FL_M;

    /* RDES0's frame length counts the 4-byte FCS, because ACS is off and
     * would not have stripped it for an Ethernet II frame anyway (see
     * emac_start()). net/netif.h's contract is that what goes up carries no
     * FCS, so it comes off here -- unconditionally, since the MAC has already
     * checked it and a frame whose CRC failed never gets this far without
     * RDES0_ES set. */
    if (len >= 4u) len -= 4u;
    else st |= RDES0_ES;                   /* impossibly short; treat as error */

    if ((st & RDES0_ES) || !(st & RDES0_FS) || !(st & RDES0_LS)) {
        return NULL;                       /* errored or split across buffers */
    }

    /* The DMA wrote this buffer behind the cache's back, so drop any cached
     * copy before reading it. Without this the CPU can read a line that
     * predates the frame -- which, because the previous frame's bytes are
     * often still there, looks like the *last* frame arriving again rather
     * than like nothing arriving. */
    esp32p4_dcache_invalidate((uintptr_t)g_rx_buf[g_rx_next], EMAC_BUF_SIZE);
    *len_out = len;
    return g_rx_buf[g_rx_next];
}

/* Hands the current receive descriptor back to the DMA. Safe to call when
 * emac_rx_peek() returned NULL for an errored frame; that is how the ring
 * recovers. */
static void emac_rx_release(void) {
    emac_desc_t *d = &g_rx_desc[g_rx_next];
    esp32p4_dcache_invalidate((uintptr_t)g_rx_buf[g_rx_next], EMAC_BUF_SIZE);
    d->des0 = RDES0_OWN;
    desc_to_dma(d);
    g_rx_next = (g_rx_next + 1u) % EMAC_RX_DESC_COUNT;
    REG(EMAC_DMA_RXPOLL) = 1;
}


/* --- the Z2 selftest ----------------------------------------------------
 *
 * Sends frames through the MAC's internal loopback and checks that what comes
 * back is byte-identical. No PHY, no cable, no link -- which is the whole
 * point. A failure here has one candidate cause (the rings, the cache
 * discipline, or the buffer ownership protocol), where the same failure
 * attempted on a live wire would have four.
 *
 * The sizes are chosen to hit the edges the descriptor fields actually have:
 *
 *   60   the smallest legal Ethernet frame without FCS. Below 60 the MAC pads,
 *        which would make "what came back equals what went in" false for a
 *        reason that is not a bug.
 *   64   one whole cache line, so the buffer's tail is exactly a line boundary.
 *   65   one byte past a line, the case where a partial final line has to be
 *        maintained correctly.
 *   1514 NETIF_FRAME_MAX -- the largest frame this stack will ever hand down.
 */
static const uint16_t g_loopback_sizes[] = { 60, 64, 65, 128, 512, 1500, 1514 };

/* The expected byte at `off` in a frame of `len` bytes. A pure function, so
 * the test needs no copy of what it sent: the first six bytes are a
 * destination MAC, the next six a source MAC, then an EtherType, then a
 * pattern that depends on the length as well as the offset. */
static uint8_t loopback_byte(uint32_t off, uint32_t len) {
    static const uint8_t head[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    if (off < 6u)  return head[off];
    if (off == 12u) return 0x08;
    if (off == 13u) return 0x00;
    return (uint8_t)(off + len);
}

/* `emac loopback stress N`: the same send-and-verify as the Z2 test, N times
 * over, reporting only the totals.
 *
 * Written for a specific question that the seven-frame test cannot answer.
 * Z5 found that roughly 1-2% of transmitted frames never reach the peer, with
 * the MAC reporting no error and the peer's NIC reporting no error -- and the
 * two candidate explanations (the DMA/descriptor path inside the chip, or the
 * RMII/PHY path out of it) are indistinguishable from the wire. Internal
 * loopback exercises the first and not the second: MAC-internal loopback
 * never reaches the PHY. A loss rate here means the bug is ours; a clean run
 * of thousands means it is downstream of the MAC. */
/* The send-and-verify loop, shared by every loopback the driver offers.
 *
 * Factored out deliberately rather than copied: four separate rewrites of
 * this loop each shipped a different defect, and a second copy for PHY
 * loopback would have been a fifth chance to get it wrong. Whatever loops the
 * frames back, the verification is identical. */
static void loopback_run(uint32_t rounds, uint32_t *p_sent, uint32_t *p_ok,
                         uint32_t *p_corrupt, uint32_t *p_refused) {

    /* Verification is by content, not by position, and that distinction is
     * the whole reason this test works where two earlier versions did not.
     *
     * loopback_byte() is a pure function of offset *and length*, so a frame
     * can be checked against its own received length without knowing which
     * send it belongs to. Both earlier attempts instead assumed strict
     * one-in-one-out -- send a frame, expect that frame -- and a single late
     * or missing frame put the stream permanently one behind, after which
     * every subsequent frame was compared against the previous one's
     * expectation. They reported "1439 lost, 1376 wrong length" out of 3500,
     * which was entirely the test eating itself and said nothing whatever
     * about the hardware.
     *
     * Draining after every send also keeps at most one or two frames
     * outstanding, so the four-deep receive ring cannot overflow and turn a
     * test artifact into a "loss". */
    if (rounds == 0) rounds = 100;
    uint32_t sent = 0, ok = 0, corrupt = 0, refused = 0;

    for (uint32_t r = 0; r < rounds; r++) {
        for (unsigned si = 0; si < sizeof(g_loopback_sizes) / sizeof(g_loopback_sizes[0]); si++) {
            uint32_t len = g_loopback_sizes[si];

            uint8_t *tx = NULL;
            for (unsigned i = 0; i < 1000u && !tx; i++) {
                tx = emac_tx_buffer();
                if (!tx) time_delay_us(10);
            }
            if (!tx) { refused++; continue; }
            for (uint32_t i = 0; i < len; i++) tx[i] = loopback_byte(i, len);
            if (emac_tx_submit(len) != 0) { refused++; continue; }
            sent++;

            /* Drain the ring *empty*, not one frame per send. Waiting only
             * on the first attempt and then taking whatever else is already
             * there is what keeps a late frame from being left behind to be
             * miscounted later: an earlier version drained exactly one and
             * reported 750 of 3500 "unaccounted", which was its own pacing
             * and not the hardware. */
            for (unsigned guard = 0; guard < EMAC_RX_DESC_COUNT + 1u; guard++) {
                bool owned = false;
                unsigned patience = (guard == 0) ? 2000u : 1u;
                for (unsigned i = 0; i < patience && !owned; i++) {
                    desc_from_dma(&g_rx_desc[g_rx_next]);
                    if (!(g_rx_desc[g_rx_next].des0 & RDES0_OWN)) { owned = true; break; }
                    time_delay_us(1);
                }
                if (!owned) break;              /* ring is empty */

                uint32_t got = 0;
                const uint8_t *rx = emac_rx_peek(&got);
                if (!rx) { corrupt++; emac_rx_release(); continue; }

                uint32_t bad = 0;
                for (uint32_t i = 0; i < got; i++) {
                    if (rx[i] != loopback_byte(i, got)) { bad++; break; }
                }
                emac_rx_release();
                if (bad) corrupt++; else ok++;
            }
        }
    }

    *p_sent = sent; *p_ok = ok; *p_corrupt = corrupt; *p_refused = refused;
}

static void loopback_report(const char *what, uint32_t sent, uint32_t ok,
                            uint32_t corrupt, uint32_t refused) {
    cprintf("EMAC %s loopback: %lu sent, %lu verified, %lu corrupt, "
            "%lu refused, %lu unaccounted\n", what,
            (unsigned long)sent, (unsigned long)ok, (unsigned long)corrupt,
            (unsigned long)refused, (unsigned long)(sent - ok - corrupt));
}

void emac_loopback_stress(uint32_t rounds) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    g_diag_owns_rings = true;
    emac_start(true);

    uint32_t sent, ok, corrupt, refused;
    loopback_run(rounds, &sent, &ok, &corrupt, &refused);

    reg_modify(EMAC_MACCONFIG, MACCFG_LM, 0);
    g_diag_owns_rings = false;
    loopback_report("MAC-internal", sent, ok, corrupt, refused);
}

void emac_loopback_test(void) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    g_diag_owns_rings = true;
    emac_start(true);

    cprintf("EMAC loopback: %u RX + %u TX descriptors of %u B, buffers %u B\n",
            (unsigned)EMAC_RX_DESC_COUNT, (unsigned)EMAC_TX_DESC_COUNT,
            (unsigned)sizeof(emac_desc_t), (unsigned)EMAC_BUF_SIZE);
    cprintf("  rings cost %u B of .bss; L1 line %u B (TRM 9.3.3.2 p909)\n",
            (unsigned)(sizeof(g_rx_desc) + sizeof(g_tx_desc) +
                       sizeof(g_rx_buf) + sizeof(g_tx_buf)),
            (unsigned)ESP32P4_L1_CACHE_LINE);

    unsigned passed = 0, failed = 0;

    for (unsigned s = 0; s < sizeof(g_loopback_sizes) / sizeof(g_loopback_sizes[0]); s++) {
        uint32_t len = g_loopback_sizes[s];

        /* Written straight into the DMA's transmit buffer, and checked
         * straight out of its receive buffer -- there is no second copy of
         * this frame anywhere, so the comparison cannot be satisfied by
         * comparing a buffer with itself. The expected bytes come from
         * loopback_byte(), a pure function of offset and length: a frame that
         * came back from the *previous* iteration (the classic stale-cache
         * symptom) has the wrong length baked into every byte and fails. */
        uint8_t *tx = emac_tx_buffer();
        if (!tx) {
            cprintf("  %4u B: no free transmit descriptor\n", (unsigned)len);
            failed++;
            continue;
        }
        for (uint32_t i = 0; i < len; i++) tx[i] = loopback_byte(i, len);

        int rc = emac_tx_submit(len);
        if (rc != 0) {
            cprintf("  %4u B: send refused (%d)\n", (unsigned)len, rc);
            failed++;
            continue;
        }

        /* Bounded wait. Loopback is a few microseconds; anything that has not
         * arrived in a millisecond is not going to. */
        const uint8_t *rx = NULL;
        uint32_t got = 0;
        bool owned = false;
        for (unsigned i = 0; i < 1000u && !owned; i++) {
            desc_from_dma(&g_rx_desc[g_rx_next]);
            if (!(g_rx_desc[g_rx_next].des0 & RDES0_OWN)) {
                owned = true;
                rx = emac_rx_peek(&got);
                break;
            }
            time_delay_us(1);
        }

        /* Only release what we own -- see emac_loopback_stress(). Releasing
         * after a timeout steps the ring past a descriptor the DMA still
         * holds, which desynchronises every frame after it. */
        if (!owned) {
            cprintf("  %4u B: nothing came back, DMA status 0x%08lx\n",
                    (unsigned)len, (unsigned long)REG(EMAC_DMA_STATUS));
            failed++;
            continue;
        }
        if (!rx) {
            cprintf("  %4u B: the frame came back with an error\n", (unsigned)len);
            emac_rx_release();
            failed++;
            continue;
        }
        if (got != len) {
            cprintf("  %4u B: came back %u B\n", (unsigned)len, (unsigned)got);
            emac_rx_release();
            failed++;
            continue;
        }
        uint32_t bad = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (rx[i] != loopback_byte(i, len)) bad++;
        }
        emac_rx_release();
        if (bad) {
            cprintf("  %4u B: %u byte%s differ\n", (unsigned)len,
                    (unsigned)bad, bad == 1 ? "" : "s");
            failed++;
            continue;
        }
        cprintf("  %4u B: ok\n", (unsigned)len);
        passed++;
    }

    /* Leave loopback off. A MAC left looped back would make Z3's link state
     * and Z4's first frame lie in a way that is very hard to see. */
    reg_modify(EMAC_MACCONFIG, MACCFG_LM, 0);
    g_diag_owns_rings = false;

    cprintf("EMAC loopback: %u passed, %u failed\n", passed, failed);
}

/* ======================================================================
 * Z3 -- auto-negotiation and link state.
 * ====================================================================== */

/* Clause-22 registers. All standard, all in IEEE 802.3 clause 22.2.4 -- the
 * reason this driver needs no IP101-specific code at all, and why ESP-IDF
 * v6.1 no longer ships an esp_eth_phy_ip101.c. */
#define PHY_BMCR            0u
#define  BMCR_RESET             (1u << 15)
#define  BMCR_LOOPBACK          (1u << 14)
#define  BMCR_SPEED_100         (1u << 13)
#define  BMCR_AN_ENABLE         (1u << 12)
#define  BMCR_POWERDOWN         (1u << 11)
#define  BMCR_ISOLATE           (1u << 10)
#define  BMCR_AN_RESTART        (1u << 9)
#define  BMCR_FULL_DUPLEX       (1u << 8)
#define PHY_BMSR            1u
#define  BMSR_AN_COMPLETE       (1u << 5)
#define  BMSR_REMOTE_FAULT      (1u << 4)
#define  BMSR_AN_ABILITY        (1u << 3)
#define  BMSR_LINK_UP           (1u << 2)
#define PHY_ANAR            4u
#define PHY_ANLPAR          5u
#define  AN_100BASE_TX_FD       (1u << 8)
#define  AN_100BASE_TX_HD       (1u << 7)
#define  AN_10BASE_T_FD         (1u << 6)
#define  AN_10BASE_T_HD         (1u << 5)
#define  AN_SELECTOR_802_3      0x0001u

/* The RMII reference is always 50 MHz, and the MAC's RX/TX clock domain has
 * to be 25 MHz at 100 Mbit/s and 2.5 MHz at 10 Mbit/s. The field holds
 * (divider - 1), so:
 *
 *     100 Mbit/s   50/25  = 2   ->  DIV_NUM 1
 *      10 Mbit/s   50/2.5 = 20  ->  DIV_NUM 19
 *
 * DIV_NUM's reset value is 1, which is why Z1 could leave the divisors alone
 * and still have a working 100 Mbit/s link -- and why doing so was luck
 * dressed as a decision until this function existed. A 10 Mbit/s link with
 * the 100 Mbit/s divisor does not fail cleanly; it corrupts. */
#define EMAC_CLK_DIV_100M   1u
#define EMAC_CLK_DIV_10M    19u

static emac_link_t g_link;          /* last resolved state */
static uint64_t    g_link_checked_ms;
static bool        g_link_valid;

/* Applies a resolved speed and duplex to the MAC and to the RMII clock
 * divisors. Both halves matter and they live in different peripherals: the
 * MAC's own idea of the line rate is MACCONFIG.FES, and the clock that
 * actually shifts the bits is HP_SYS_CLKRST's. Setting one without the other
 * is the failure mode this function exists to make impossible. */
/* The station address the receive filter matches against.
 *
 * Kept in a static as well as written to the register because it has to be
 * written **again every time the link comes up** -- see the note in
 * emac_apply_link(). */
static uint8_t g_station_mac[NETIF_MAC_LEN];
static bool    g_station_mac_valid;

static void emac_set_mac(const uint8_t mac[NETIF_MAC_LEN]) {
    if (mac != g_station_mac) {
        memcpy(g_station_mac, mac, NETIF_MAC_LEN);
        g_station_mac_valid = true;
    }

    /* Drop the receiver across the write, and this is not caution -- it is
     * measured. Writing Address0 with RE set corrupts whatever frame is in
     * flight: it comes out of the ring with RDES0_ES and is thrown away.
     * Rewriting the address every 200 ms (which an earlier version of this
     * driver did) cost 9% of a 10-per-second ping, with the driver's own
     * error counter matching the lost packets exactly -- 17 rejected frames
     * for 17 missing replies.
     *
     * Bringing RE down for the two stores is also what makes the write
     * actually reach the filter, which is why one write is now enough where
     * the earlier "retry forever" version was reaching for a bigger hammer to
     * solve the same problem. */
    uint32_t cfg = REG(EMAC_MACCONFIG);
    bool rx_live = (cfg & MACCFG_RE) != 0;
    if (rx_live) REG(EMAC_MACCONFIG) = cfg & ~MACCFG_RE;

    REG(EMAC_MACADDR0LOW)  = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
                             ((uint32_t)mac[1] << 8)  |  (uint32_t)mac[0];
    REG(EMAC_MACADDR0HIGH) = ((uint32_t)mac[5] << 8)  |  (uint32_t)mac[4];

    if (rx_live) REG(EMAC_MACCONFIG) = cfg;
}

static void emac_apply_link(const emac_link_t *l) {
    uint32_t div = (l->speed_mbit == 10u) ? EMAC_CLK_DIV_10M : EMAC_CLK_DIV_100M;
    reg_modify(PERI_CLK_CTRL01, EMAC_RX_CLK_DIV_NUM_M, div);
    reg_modify(PERI_CLK_CTRL01, EMAC_TX_CLK_DIV_NUM_M,
               (div << EMAC_TX_CLK_DIV_NUM_S) & EMAC_TX_CLK_DIV_NUM_M);

    uint32_t cfg = REG(EMAC_MACCONFIG);
    cfg &= ~(MACCFG_FES | MACCFG_DM);
    if (l->speed_mbit == 100u) cfg |= MACCFG_FES;
    if (l->full_duplex)        cfg |= MACCFG_DM;
    REG(EMAC_MACCONFIG) = cfg;

    /* (The station address is re-applied by emac_link_poll(), not here: this
     * function only runs on a *change*, and the case that bit hardest was a
     * MAC reset with the link never appearing to change. See the long note
     * there.)
     *
     * Historical note kept because it explains the mechanism.
     *
     * Address0 is written once during bring-up, before auto-negotiation has
     * finished. At that point there is no link, so the RMII receive clock is
     * not running -- the divisors two lines above are what start it -- and
     * the address filter, which lives in that clock domain, never picks the
     * value up. The register itself takes the write: reading it back gives
     * exactly what was written, which is what makes this so slow to find.
     * The *filter* goes on comparing against its reset value, all ones.
     *
     * The symptom that produces is precise and misleading: broadcast frames
     * are accepted (ff:ff:ff:ff:ff:ff is a perfect match against a filter
     * still holding all ones), so ARP works, the link looks healthy, the peer
     * resolves us and caches our address -- and then every unicast frame
     * addressed to us is silently dropped. `emac stats` shows the right
     * address in the right register while nothing arrives.
     *
     * Re-applying it here fixes it for every link-up, not just the first, so
     * a replugged cable or a renegotiated speed cannot resurrect the problem.
     * Measured on the bench 2026-09-15: before this, ping was 0/4 with the
     * filter on and 4/4 with it off; after, 4/4 with the filter on. */
}

/* Starts auto-negotiation. Advertises everything this MAC can actually do --
 * 100 and 10, full and half -- rather than forcing a speed, because the
 * far end is not ours to assume. */
int emac_phy_autoneg_start(void) {
    int bmcr = emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_BMCR);
    if (bmcr < 0) return -1;

    /* Clear ISOLATE and POWERDOWN if a previous run or the straps left them
     * set: an isolated PHY answers MDIO perfectly and passes no traffic,
     * which is a memorably confusing state. */
    if (emac_mdio_write(CONFIG_EMAC_PHY_ADDR, PHY_BMCR,
                        (uint16_t)(bmcr & ~(BMCR_ISOLATE | BMCR_POWERDOWN |
                                            BMCR_LOOPBACK))) != 0) return -1;

    if (emac_mdio_write(CONFIG_EMAC_PHY_ADDR, PHY_ANAR,
                        AN_SELECTOR_802_3 | AN_100BASE_TX_FD | AN_100BASE_TX_HD |
                        AN_10BASE_T_FD | AN_10BASE_T_HD) != 0) return -1;

    if (emac_mdio_write(CONFIG_EMAC_PHY_ADDR, PHY_BMCR,
                        BMCR_AN_ENABLE | BMCR_AN_RESTART) != 0) return -1;
    g_link_valid = false;
    return 0;
}

/* Reads link state, resolves speed and duplex, and applies them if they
 * changed.
 *
 * Never blocks in the scheduler's sense: no task_block(), no lock, no wait on
 * another task. It does spin on the MDIO busy bit for a bounded few
 * microseconds, which is the same bounded spin every register access in this
 * kernel makes.
 *
 * Rate-limited, because netif_t's poll() is called in a tight loop and an
 * MDIO transaction per call would be a slow bus transaction per frame for no
 * information -- link state changes on human timescales. Between refreshes
 * the cached answer is returned. */
bool emac_link_poll(emac_link_t *out) {
    uint64_t now = time_get_ms();
    if (g_link_valid && (now - g_link_checked_ms) < 200u) {
        if (out) *out = g_link;
        return g_link.up;
    }
    g_link_checked_ms = now;

    /* BMSR's link bit latches low: it reports 0 if the link has been down at
     * any point since the last read, and the *current* state only on a second
     * read. One read would therefore report a link that has recovered as
     * still down, and would do so once per recovery -- an intermittent bug
     * with a latency of exactly one poll. Read twice, trust the second. */
    (void)emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_BMSR);
    int bmsr = emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_BMSR);

    emac_link_t l = { 0 };
    if (bmsr >= 0 && (bmsr & BMSR_LINK_UP)) {
        l.up = true;
        l.an_complete = (bmsr & BMSR_AN_COMPLETE) != 0;
        int anar = emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_ANAR);
        int lpar = emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_ANLPAR);
        if (l.an_complete && anar >= 0 && lpar >= 0) {
            /* The resolution is the intersection of what both ends
             * advertised, taken in the priority order 802.3 clause 28.1.4.5
             * defines: 100 full, 100 half, 10 full, 10 half. */
            uint32_t common = (uint32_t)anar & (uint32_t)lpar;
            if (common & AN_100BASE_TX_FD)      { l.speed_mbit = 100; l.full_duplex = true;  }
            else if (common & AN_100BASE_TX_HD) { l.speed_mbit = 100; l.full_duplex = false; }
            else if (common & AN_10BASE_T_FD)   { l.speed_mbit = 10;  l.full_duplex = true;  }
            else if (common & AN_10BASE_T_HD)   { l.speed_mbit = 10;  l.full_duplex = false; }
            else                                { l.speed_mbit = 10;  l.full_duplex = false; }
        } else {
            /* Link without a completed negotiation is parallel detect: the
             * far end is not negotiating, so 802.3 says assume half duplex at
             * whatever speed the PHY managed. Reported honestly rather than
             * papered over -- it is also exactly what the laptop's e1000e
             * prints for a moment each time this board resets. */
            l.speed_mbit = 10;
            l.full_duplex = false;
        }
    }

    bool changed = !g_link_valid ||
                   l.up != g_link.up ||
                   l.speed_mbit != g_link.speed_mbit ||
                   l.full_duplex != g_link.full_duplex;
    g_link = l;
    g_link_valid = true;
    if (changed && l.up) emac_apply_link(&l);

    /* Re-apply the station address to the receive filter on every poll that
     * sees the link up. Two register writes per poll, and the poll above is
     * rate-limited to one MDIO exchange per 200 ms, so the cost is nil.
     *
     * Unconditional, and that is the point. The filter only latches Address0
     * while the RMII receive clock runs -- i.e. while the link is up -- so a
     * write made at any other time reads back byte-perfect and never reaches
     * the filter, which goes on comparing against its all-ones reset value.
     * Broadcast then passes, ARP works, the peer caches our address, and
     * every unicast frame to us is dropped: see the long note in
     * emac_apply_link() for the full symptom.
     *
     * Three narrower versions were tried, and each one looked right and
     * failed on hardware. They are listed because every one of them is the
     * obvious thing to try again:
     *
     *   - in emac_apply_link(), i.e. on a link *change*. Wrong because the
     *     worst case has no change at all -- `emac link` re-probes, the probe
     *     resets the MAC and wipes Address0, the cable never moved, so the
     *     next poll sees the same speed and duplex and nothing is rewritten.
     *   - once per link-up, keyed on a flag emac_start() cleared. Wrong
     *     because the one write it makes happens on the first poll that sees
     *     the link up, which is exactly when the receive clock has just
     *     started and has not settled. The write does not latch, the flag
     *     says it did, and it is never retried. This was tried twice: the
     *     second time it was reintroduced as an "optimisation" after the
     *     periodic write was wrongly blamed for frame loss (the real cause
     *     was the receive race above), and the hardware suite caught it again
     *     within one run -- `emac loopback` and `emac link` both reset the
     *     MAC, and after them the board answered ARP and dropped every
     *     unicast echo, 100 of 100.
     *   - unconditionally, here, but left to whoever happened to call this
     *     function. Wrong because after the interface has come up once, that
     *     is nobody: net/stack.c asks netif_link_up() only until it first
     *     answers yes. Which is why emac_netif_poll() now drives this --
     *     poll() is the only callback the stack keeps calling.
     *
     * Retrying forever is immune to all three: a write that does not take is
     * simply followed by another one 200 ms later. */
    if (l.up && g_station_mac_valid) emac_set_mac(g_station_mac);

    if (out) *out = l;
    return l.up;
}

void emac_link_report(void) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    emac_start(false);
    if (emac_phy_autoneg_start() != 0) {
        cprintf("[EMAC] could not start auto-negotiation (PHY at %d not "
                "answering?)\n", (int)CONFIG_EMAC_PHY_ADDR);
        return;
    }

    cprintf("EMAC: negotiating...\n");
    emac_link_t l = { 0 };
    /* Auto-negotiation is a few hundred milliseconds when the far end is
     * ready and forever when the cable is out, so this is bounded and the
     * bound is part of the answer. */
    for (unsigned i = 0; i < 40u; i++) {
        g_link_valid = false;           /* defeat the rate limit while waiting */
        if (emac_link_poll(&l) && l.an_complete) break;
        time_delay_us(100000);
    }

    if (!l.up) {
        cprintf("EMAC: link DOWN (no carrier)\n");
        return;
    }
    cprintf("EMAC: link UP, %u Mbit/s %s duplex%s, MACCONFIG 0x%08lx\n",
            (unsigned)l.speed_mbit, l.full_duplex ? "full" : "half",
            l.an_complete ? "" : " (parallel detect)",
            (unsigned long)REG(EMAC_MACCONFIG));
}

/* --- Z3's other half: does link-down actually get noticed? ---------------
 *
 * "It reports the link is up" is the easy half, and a driver that returned a
 * hardcoded `true` would pass it. The half that matters for netif_t is that
 * link_up() goes false when the carrier goes away, promptly, without
 * spinning, and comes back when it returns.
 *
 * Testing that normally means a human unplugging a cable, which cannot live
 * in a test suite. So it is done from the PHY's side instead: BMCR's
 * POWERDOWN bit drops the link exactly as pulling the cable does -- BMSR's
 * link bit clears either way, which is all the MAC and this driver can see.
 * The cable stays in and the far end notices a real carrier loss, which is
 * also a useful check that this board is visible to the switch it is plugged
 * into.
 *
 * Latency is measured rather than assumed, because the rate limit in
 * emac_link_poll() puts a floor under it and a floor that drifts is worth
 * catching. */
void emac_link_updown_test(void) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    emac_start(false);
    if (emac_phy_autoneg_start() != 0) {
        cprintf("[EMAC] auto-negotiation would not start.\n");
        return;
    }

    emac_link_t l;
    for (unsigned i = 0; i < 40u && !(emac_link_poll(&l) && l.an_complete); i++) {
        g_link_valid = false;
        time_delay_us(100000);
    }
    if (!l.up) {
        cprintf("EMAC linktest: SKIPPED -- no carrier to begin with.\n");
        return;
    }
    cprintf("EMAC linktest: up at %u Mbit/s %s duplex; dropping the PHY...\n",
            (unsigned)l.speed_mbit, l.full_duplex ? "full" : "half");

    /* Down. */
    int bmcr = emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_BMCR);
    emac_mdio_write(CONFIG_EMAC_PHY_ADDR, PHY_BMCR,
                    (uint16_t)(bmcr | BMCR_POWERDOWN));

    uint64_t t0 = time_get_ms();
    bool went_down = false;
    for (unsigned i = 0; i < 300u; i++) {
        if (!emac_link_poll(&l)) { went_down = true; break; }
        time_delay_us(10000);
    }
    uint64_t down_ms = time_get_ms() - t0;

    /* Back up, whatever happened above -- leaving the PHY powered down would
     * be a far worse failure than the one being tested for. */
    emac_mdio_write(CONFIG_EMAC_PHY_ADDR, PHY_BMCR,
                    (uint16_t)(bmcr & ~BMCR_POWERDOWN));
    emac_phy_autoneg_start();

    t0 = time_get_ms();
    bool came_back = false;
    for (unsigned i = 0; i < 100u; i++) {
        g_link_valid = false;
        if (emac_link_poll(&l) && l.an_complete) { came_back = true; break; }
        time_delay_us(100000);
    }
    uint64_t up_ms = time_get_ms() - t0;

    if (!went_down) {
        cprintf("EMAC linktest: FAILED -- link never went down with the PHY "
                "powered off.\n");
        return;
    }
    if (!came_back) {
        cprintf("EMAC linktest: FAILED -- link did not come back (down was "
                "noticed in %u ms).\n", (unsigned)down_ms);
        return;
    }
    cprintf("EMAC linktest: down noticed in %u ms, back up in %u ms "
            "at %u Mbit/s %s duplex\n",
            (unsigned)down_ms, (unsigned)up_ms, (unsigned)l.speed_mbit,
            l.full_duplex ? "full" : "half");
    cprintf("EMAC linktest: PASSED\n");
}

/* `emac loopback phy [N]`: the same frames, looped back inside the **PHY**.
 *
 * This is the experiment MAC-internal loopback cannot perform. The internal
 * path never leaves the MAC, so it exonerates the DMA, the descriptors and
 * the cache maintenance -- and it did, at 2100 of 2100 -- while saying
 * nothing about the RMII pins, their timing, or the PHY itself. Setting
 * BMCR's LOOPBACK bit sends the frame all the way out through RMII into the
 * PHY, which turns it around internally: everything our board owns, and
 * nothing it does not. The cable, the magnetics and the far end are excluded.
 *
 * So: loss here means the fault is ours, in the RMII path or the PHY. A clean
 * run of thousands means the ~1% the wire loses happens beyond the PHY --
 * cable, or peer. See plan/open_issues.md.
 *
 * Two details that make the difference between measuring this and measuring
 * nothing. The MAC runs in **normal** mode (emac_start(false)), because the
 * point is to use the real transmit path -- MACCFG_LM would short-circuit
 * exactly the wiring under test. And the receive filter goes to FILTER_RA,
 * because the test frames are addressed to 02:00:00:00:00:01 rather than to
 * us, so perfect filtering would drop every one of them and report a total
 * loss that meant nothing at all. */
void emac_loopback_phy(uint32_t rounds) {
    if (emac_probe() != 0) {
        cprintf("[EMAC] probe failed -- see `emac scan`.\n");
        return;
    }
    g_diag_owns_rings = true;
    emac_start(false);
    REG(EMAC_MACFRAMEFILTER) = FILTER_RA;

    /* Force the PHY to 100 Mbit/s full duplex and loop it back. Auto-
     * negotiation is switched off with the same write: negotiating against
     * one's own reflection is not meaningful, and leaving AN enabled makes
     * the loopback bit's effect depend on what the far end happens to do. */
    if (emac_mdio_write(CONFIG_EMAC_PHY_ADDR, PHY_BMCR,
                        (uint16_t)(BMCR_LOOPBACK | BMCR_SPEED_100 |
                                   BMCR_FULL_DUPLEX)) != 0) {
        cprintf("[EMAC] PHY at %d did not accept the loopback write\n",
                (int)CONFIG_EMAC_PHY_ADDR);
        g_diag_owns_rings = false;
        return;
    }
    time_delay_us(50000);          /* the PHY re-times its path; let it settle */

    int bmcr = emac_mdio_read(CONFIG_EMAC_PHY_ADDR, PHY_BMCR);
    if (bmcr < 0 || !((uint32_t)bmcr & BMCR_LOOPBACK)) {
        cprintf("[EMAC] PHY loopback did not stick (BMCR 0x%04x) -- not "
                "measuring, because a PHY that is not looped back would "
                "simply report every frame lost\n", (unsigned)(bmcr & 0xffff));
        emac_phy_autoneg_start();
        g_diag_owns_rings = false;
        return;
    }

    uint32_t sent, ok, corrupt, refused;
    loopback_run(rounds, &sent, &ok, &corrupt, &refused);

    /* Put the PHY back on the wire. */
    emac_phy_autoneg_start();
    g_link_valid = false;
    g_diag_owns_rings = false;
    loopback_report("PHY", sent, ok, corrupt, refused);
}

/* --- Z4: the netif ------------------------------------------------------
 *
 * Everything below turns the three primitives Z2 proved -- emac_tx_buffer() /
 * emac_tx_submit(), emac_rx_peek() / emac_rx_release() -- into the shape
 * net/netif.h specifies, and points them at the real PHY instead of the MAC's
 * internal loopback.
 *
 * The FCS is not handled here. emac_rx_peek() already strips it (and explains
 * at length why ACS cannot), so what this layer hands up carries no FCS, as
 * netif.h requires. The *check* is the MAC's: a frame whose CRC failed sets
 * RDES0_ES and emac_rx_peek() never returns it.
 *
 * Destination filtering is likewise not a memcmp() here -- emac_start() puts
 * it in the address filter, where the hardware compares every arriving frame
 * against Address0 for free. A frame that is neither ours nor broadcast is
 * dropped before it ever occupies a descriptor.
 */

static netif_t g_netif;
static bool    g_netif_registered;

/* The zero-copy hand-off between poll() and recv_frame().
 *
 * poll() peeks at the head descriptor and *holds* it -- the bytes stay in the
 * DMA buffer, and nothing is copied until recv_frame() asks for them. The
 * ENC28J60 driver latches into a private 1514-byte buffer instead; it has to,
 * because its frame lives behind an SPI bus rather than in memory the CPU can
 * address. Here the buffer already is memory, so the copy would be pure cost.
 *
 * The obligation that comes with holding a descriptor is that it has to be
 * released on *every* path out, including the ones that are not a successful
 * receive -- a frame too big for the caller's buffer, or a caller that polls
 * and never receives. Miss one and the ring stops at that descriptor
 * permanently, which presents as "the link is up and no traffic arrives".
 * g_rx_held is what makes that auditable. */
static const uint8_t *g_rx_held;
static uint32_t       g_rx_held_len;

/* Frames dropped with RDES0_ES set, or split across descriptors. Counted
 * here and not in netif_t: net/netif.h reserves its counters for what the
 * netif_send()/netif_recv() wrappers can see, precisely so two drivers cannot
 * disagree about what they mean. A hardware-level error is this driver's own
 * business, and its own number. */
static uint32_t g_rx_hw_errors;
/* RDES0 of the last rejected frame, and a running OR of every one of them, so
 * a rate of rejections can be attributed to a cause instead of guessed at. */
static uint32_t g_rx_err_last_des0;
static uint32_t g_rx_err_seen_bits;

/* The Ethernet header of the last frame the MAC accepted, so `emac stats` can
 * answer "what is actually addressed to us on this wire" without a capture
 * host. Fourteen bytes: destination, source, EtherType. */
static uint8_t  g_last_hdr[14];
static bool     g_last_hdr_valid;

static int emac_netif_poll(netif_t *nif) {
    (void)nif;
    if (!g_netif_registered) return -1;
    if (g_diag_owns_rings) return 0;    /* a loopback diagnostic has the rings */

    /* Pump the link, which also rewrites the station address into the receive
     * filter (see emac_link_poll()). Rate-limited inside emac_link_poll() to
     * one MDIO exchange per 200 ms, so calling it from this hot path costs
     * nothing in the common case.
     *
     * It has to be driven from *here*, and that is the whole point. The
     * obvious homes for it are all functions nobody calls often enough:
     * net/stack.c asks netif_link_up() only until the first time it answers
     * yes (net_autoconfig_once() latches `done`), so after the interface has
     * come up once, the only thing that ever calls emac_link_poll() again is
     * a human typing `net` or `emac link`. Anything that resets the MAC after
     * that point -- `emac link` itself does, via emac_probe() -- would wipe
     * Address0 out of the filter with nothing left running to put it back,
     * and the board would answer ARP and drop every unicast frame until the
     * next reboot. poll() is the one call that keeps coming. */
    (void)emac_link_poll(NULL);

    if (g_rx_held) return 1;            /* already holding one; idempotent */

    /* Bounded, because a burst of errored frames must not turn a poll into an
     * unbounded loop -- netif_t's poll() is called from tight loops and is
     * required never to block. One pass over the ring is the natural bound:
     * anything still queued is picked up by the next poll. */
    for (unsigned i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        /* Ownership first, and the order is the whole correctness argument.
         *
         * emac_rx_peek() returns NULL for two unrelated reasons -- "the DMA
         * still owns this descriptor, nothing has arrived" and "a frame
         * arrived and it is broken" -- and the caller has to tell them apart
         * to know whether releasing the descriptor is right. Asking peek
         * first and re-reading the descriptor afterwards to find out is a
         * race, because the DMA can complete a frame in between: the re-read
         * then finds OWN clear, the code concludes "errored", and a perfectly
         * good frame is counted as a hardware error and released without ever
         * being handed up.
         *
         * That is not hypothetical -- it is what this loop did when Z4
         * shipped, and it cost 3-15% of every ping depending on rate, with
         * RDES0 reading 0x00660320 (FS | LS | frame-type, length 102, not one
         * error bit set) on the frames it threw away. Small pings passed
         * because five packets rarely hit the window.
         *
         * Reading OWN first removes the window: once the DMA has cleared it
         * the descriptor belongs to us and nothing else writes it. */
        emac_desc_t *d = &g_rx_desc[g_rx_next];
        desc_from_dma(d);
        if (d->des0 & RDES0_OWN) return 0;      /* genuinely nothing ready */

        uint32_t len = 0;
        const uint8_t *p = emac_rx_peek(&len);
        if (p) {
            if (len > NETIF_FRAME_MAX) {
                /* Longer than anything this stack can carry. The receive
                 * watchdog already cuts frames above 2048 bytes, so this is
                 * the band between 1514 and that -- a jumbo frame from a
                 * misconfigured peer, not a hardware fault. */
                g_rx_hw_errors++;
                emac_rx_release();
                continue;
            }
            if (len >= sizeof(g_last_hdr)) {
                memcpy(g_last_hdr, p, sizeof(g_last_hdr));
                g_last_hdr_valid = true;
            }
            g_rx_held = p;
            g_rx_held_len = len;
            return 1;
        }
        /* We own the descriptor and peek refused it, so it is genuinely a
         * bad frame: errored, or split across descriptors. Drop it and look
         * at the next one -- that is how the ring recovers. */
        g_rx_err_last_des0 = d->des0;
        g_rx_err_seen_bits |= d->des0;
        g_rx_hw_errors++;
        emac_rx_release();                       /* drop it and look again */
    }
    return 0;
}

static int emac_netif_recv_frame(netif_t *nif, uint8_t *buf, uint32_t max_len) {
    (void)nif;
    if (!g_rx_held || !buf) return -1;
    if (g_rx_held_len > max_len) {
        /* Does not fit. Release anyway -- see g_rx_held's comment; a frame we
         * refuse must not become a frame that stops the ring. */
        g_rx_hw_errors++;
        g_rx_held = NULL;
        g_rx_held_len = 0;
        emac_rx_release();
        return -1;
    }
    uint32_t n = g_rx_held_len;
    memcpy(buf, g_rx_held, n);
    g_rx_held = NULL;
    g_rx_held_len = 0;
    emac_rx_release();
    return (int)n;
}

static int emac_netif_send_frame(netif_t *nif, const uint8_t *buf, uint32_t len) {
    (void)nif;
    if (!g_netif_registered || !buf) return -1;
    if (len == 0 || len > NETIF_FRAME_MAX) return -1;

    /* Frames shorter than 60 bytes are NOT rejected and NOT padded here: the
     * MAC pads to the 60-byte minimum itself and appends the FCS, which is
     * the same division of labour the FCS has on the way in. Padding in
     * software would send the wire a frame with our own zeros in it and hide
     * whether the hardware was doing its job. */

    /* Wait for the DMA to release the descriptor. Bounded, and the bound is
     * generous by design: at 100 Mbit/s a full 1514-byte frame is ~121 us on
     * the wire, so 10 ms is roughly eighty frame times -- long enough that
     * hitting it means the DMA is wedged rather than busy. netif.h permits
     * blocking "for as long as the hardware needs to accept the buffer", and
     * this is that, with a ceiling. */
    uint8_t *dst = NULL;
    for (unsigned i = 0; i < 1000u; i++) {
        dst = emac_tx_buffer();
        if (dst) break;
        time_delay_us(10);
    }
    if (!dst) return -1;

    memcpy(dst, buf, len);
    if (emac_tx_submit(len) != 0) return -1;
    return (int)len;
}

static bool emac_netif_link_up(netif_t *nif) {
    (void)nif;
    if (!g_netif_registered) return false;
    emac_link_t l = { 0 };
    return emac_link_poll(&l);
}

int emac_netif_init(void) {
    if (g_netif_registered) return 0;       /* idempotent, like emac_probe() */

    int rc = emac_probe();
    if (rc != 0) {
        printk("[EMAC] probe failed (%d) -- no interface registered; try `emac scan`\n", rc);
        return -1;
    }

    memset(&g_netif, 0, sizeof(g_netif));
    g_netif.name       = "eth0";
    g_netif.poll       = emac_netif_poll;
    g_netif.send_frame = emac_netif_send_frame;
    g_netif.recv_frame = emac_netif_recv_frame;
    g_netif.link_up    = emac_netif_link_up;

    /* .mac is left zeroed deliberately: this MAC has no address of its own
     * (there is no EEPROM on the RMII side and the eFuse MAC belongs to the
     * radio), so netif_register() fills it from the node identity -- exactly
     * the case its comment describes. Same as the ENC28J60. */
    if (netif_register(&g_netif) != 0) {
        printk("[EMAC] netif_register() failed -- no free interface slot\n");
        return -1;
    }
    g_netif_registered = true;

    /* This write is not what makes the filter work -- emac_link_poll() is,
     * on every poll that sees the link up, and the long comment there says
     * why. It is done here anyway so g_station_mac is populated before
     * anything can poll, and so the register holds the right value from the
     * first instant rather than the all-ones the probe's reset left.
     *
     * Do not be tempted to "simplify" this by trusting it: a readback here
     * succeeds whether or not the filter ever saw the address, which is
     * precisely what made this bug take an afternoon. `emac stats` is the
     * honest diagnostic, and a ping is the only real test. */
    emac_set_mac(g_netif.mac);
    emac_start(false);

    if (emac_phy_autoneg_start() != 0) {
        printk("[EMAC] auto-negotiation did not start (PHY at %d not answering)\n",
               (int)CONFIG_EMAC_PHY_ADDR);
        /* Not fatal: the interface stays registered and link_up() will keep
         * reporting false, which is the honest answer and the one net/stack.c
         * already knows how to wait on. */
    }

    char macstr[18];
    netif_mac_str(g_netif.mac, macstr);
    emac_link_t l = { 0 };
    bool up = emac_link_poll(&l);
    printk("[EMAC] eth0 registered, mac %s, link %s\n", macstr,
           up ? "UP" : "negotiating");
    return 0;
}

netif_t *emac_get_netif(void) {
    return g_netif_registered ? &g_netif : NULL;
}

/* --- Z4 diagnostics ------------------------------------------------------
 *
 * `emac stats`. A netif that is up and silent has several possible causes
 * that look identical from the outside -- the address filter rejecting
 * everything, the DMA running out of descriptors, the FIFO overflowing, the
 * PHY having renegotiated to something the MAC is not set to -- and this
 * prints the register that distinguishes each. Written when unicast frames
 * were not arriving while broadcast was, which is exactly the case where
 * "the link is up" tells you nothing.
 */
void emac_stats_report(void) {
    if (!g_netif_registered) {
        cprintf("EMAC: no interface registered (`emac link` for the PHY).\n");
        return;
    }
    uint32_t lo = REG(EMAC_MACADDR0LOW), hi = REG(EMAC_MACADDR0HIGH);
    uint32_t missed = REG(EMAC_DMA_MISSEDFRAME);
    char macstr[18];
    netif_mac_str(g_netif.mac, macstr);

    cprintf("EMAC %s: mac %s\n", g_netif.name, macstr);
    cprintf("  filter   0x%08lx  %s\n", (unsigned long)REG(EMAC_MACFRAMEFILTER),
            (REG(EMAC_MACFRAMEFILTER) & FILTER_RA) ? "PROMISCUOUS (receive all)"
                                                   : "our unicast + broadcast");
    cprintf("  addr0    hi 0x%08lx lo 0x%08lx -> %02x:%02x:%02x:%02x:%02x:%02x\n",
            (unsigned long)hi, (unsigned long)lo,
            (unsigned)(lo & 0xff), (unsigned)((lo >> 8) & 0xff),
            (unsigned)((lo >> 16) & 0xff), (unsigned)((lo >> 24) & 0xff),
            (unsigned)(hi & 0xff), (unsigned)((hi >> 8) & 0xff));
    cprintf("  pads     txen 0x%04lx txd0 0x%04lx txd1 0x%04lx | clk 0x%04lx "
            "crsdv 0x%04lx rxd0 0x%04lx rxd1 0x%04lx (drv = bits 11:10)\n",
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_TX_EN_GPIO)) & 0xffffu),
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_TXD0_GPIO)) & 0xffffu),
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_TXD1_GPIO)) & 0xffffu),
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_RMII_CLK_GPIO)) & 0xffffu),
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_CRS_DV_GPIO)) & 0xffffu),
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_RXD0_GPIO)) & 0xffffu),
            (unsigned long)(REG(IOMUX_PAD(CONFIG_EMAC_RXD1_GPIO)) & 0xffffu));
    cprintf("  maccfg   0x%08lx   dma status 0x%08lx   opmode 0x%08lx\n",
            (unsigned long)REG(EMAC_MACCONFIG), (unsigned long)REG(EMAC_DMA_STATUS),
            (unsigned long)REG(EMAC_DMA_OPMODE));
    cprintf("  missed   %lu no-descriptor, %lu fifo-overflow%s\n",
            (unsigned long)(missed & 0xffffu), (unsigned long)((missed >> 17) & 0x7ffu),
            (missed & (1u << 16)) ? " (counter wrapped)" : "");
    if (g_rx_hw_errors) {
        uint32_t b = g_rx_err_seen_bits;
        cprintf("  rx errs  last RDES0 0x%08lx, bits seen:%s%s%s%s%s%s%s%s\n",
                (unsigned long)g_rx_err_last_des0,
                (b & RDES0_CE)  ? " CRC"        : "",
                (b & RDES0_DBE) ? " dribble"    : "",
                (b & RDES0_RE)  ? " rx-error"   : "",
                (b & RDES0_RWT) ? " watchdog"   : "",
                (b & RDES0_OE)  ? " overflow"   : "",
                (b & RDES0_LE)  ? " length"     : "",
                (b & RDES0_SAF) ? " sa-filter"  : "",
                (b & RDES0_DE)  ? " descriptor" : "");
    }
    cprintf("  rx       %lu frames, %lu bytes, %lu hardware errors/oversize\n",
            (unsigned long)g_netif.rx_frames, (unsigned long)g_netif.rx_bytes,
            (unsigned long)g_rx_hw_errors);
    cprintf("  tx       %lu frames, %lu bytes, %lu refused, %lu failed on the wire\n",
            (unsigned long)g_netif.tx_frames, (unsigned long)g_netif.tx_bytes,
            (unsigned long)g_netif.tx_errors, (unsigned long)g_tx_hw_errors);
    if (g_tx_hw_errors) {
        uint32_t b = g_tx_err_seen_bits;
        cprintf("  tx errs  last TDES0 0x%08lx, bits seen:%s%s%s%s%s%s%s\n",
                (unsigned long)g_tx_err_last_des0,
                (b & TDES0_UF)  ? " underflow"      : "",
                (b & TDES0_ED)  ? " exc-deferral"   : "",
                (b & TDES0_EC)  ? " exc-collision"  : "",
                (b & TDES0_LC)  ? " late-collision" : "",
                (b & TDES0_NC)  ? " no-carrier"     : "",
                (b & TDES0_LOC) ? " carrier-lost"   : "",
                (b & TDES0_IHE) ? " ip-header"      : "");
    }
    if (g_last_hdr_valid) {
        cprintf("  last rx  dst %02x:%02x:%02x:%02x:%02x:%02x  src "
                "%02x:%02x:%02x:%02x:%02x:%02x  type 0x%02x%02x\n",
                g_last_hdr[0], g_last_hdr[1], g_last_hdr[2],
                g_last_hdr[3], g_last_hdr[4], g_last_hdr[5],
                g_last_hdr[6], g_last_hdr[7], g_last_hdr[8],
                g_last_hdr[9], g_last_hdr[10], g_last_hdr[11],
                g_last_hdr[12], g_last_hdr[13]);
    } else {
        cprintf("  last rx  (nothing received yet)\n");
    }
}

/* `emac promisc on|off`. Turning the address filter off is the one
 * measurement that separates "the wire is quiet" from "we are refusing what
 * is on it": if traffic appears the moment filtering stops, the frames were
 * always arriving and the filter was rejecting them. Left as a command
 * rather than a build option because the question recurs on every new
 * network this board is plugged into. */
void emac_set_promiscuous(bool on) {
    if (!g_netif_registered) {
        cprintf("EMAC: no interface registered.\n");
        return;
    }
    REG(EMAC_MACFRAMEFILTER) = on ? FILTER_RA : 0u;
    cprintf("EMAC: receive filter %s (0x%08lx)\n",
            on ? "OFF -- accepting every frame on the wire"
               : "on -- our unicast and broadcast only",
            (unsigned long)REG(EMAC_MACFRAMEFILTER));
}

/* Called from emac_start(), i.e. every time the rings are reinitialised.
 * Separate from the statics it touches only because emac_start() is defined
 * long before the netif is. The station address needs nothing here: it is
 * rewritten by every link-up poll regardless. */
static void emac_netif_restarted(void) {
    g_rx_held = NULL;
    g_rx_held_len = 0;
}
