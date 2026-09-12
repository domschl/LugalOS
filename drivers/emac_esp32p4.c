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

#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/time.h"
#include "drivers/emac_esp32p4.h"
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
static void pad_iomux(uint32_t gpio, uint32_t func, bool ie) {
    uint32_t v = REG(IOMUX_PAD(gpio));
    v &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_IE | IOMUX_FUN_DRV_M);
    v |= (func << IOMUX_MCU_SEL_S);
    v |= (3u << IOMUX_FUN_DRV_S);   /* strongest drive; RMII runs at 50 MHz */
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
        cprintf("[EMAC] probe failed (%d): the MAC's software reset never "
                "completed.\n", rc);
        cprintf("       That bit cannot clear until the PHY supplies the "
                "50 MHz RMII reference,\n");
        cprintf("       so suspect the reference clock (GPIO%d), the PHY's "
                "reset (GPIO%d), or\n",
                CONFIG_EMAC_RMII_CLK_GPIO, CONFIG_EMAC_PHY_RST_GPIO);
        cprintf("       the clock gates -- not MDIO, which has not been tried "
                "yet.\n");
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
        cprintf("  %2u: PHYIDR1=0x%04x PHYIDR2=0x%04x  "
                "OUI %02x-%02x-%02x model %u rev %u\n",
                (unsigned)a, (unsigned)id1, (unsigned)id2,
                (unsigned)((oui >> 16) & 0xffu), (unsigned)((oui >> 8) & 0xffu),
                (unsigned)(oui & 0xffu),
                (unsigned)((id2 >> 4) & 0x3f), (unsigned)(id2 & 0xf));
        found++;
        if (a == (uint8_t)CONFIG_EMAC_PHY_ADDR) {
            seen_expected = true;
            if (id1 != CONFIG_EMAC_PHY_ID1 || id2 != CONFIG_EMAC_PHY_ID2) {
                cprintf("      ^ WRONG PART: the board file expects "
                        "0x%04x/0x%04x here\n",
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
        cprintf("  nothing answered. The MAC reset completed, so the clock is "
                "arriving;\n");
        cprintf("  suspect MDC/MDIO routing or the PHY's address straps.\n");
    } else if (!seen_expected) {
        cprintf("  %u PHY%s responded, but NOT at %d where the board file "
                "says it should be.\n",
                found, found == 1 ? "" : "s", (int)CONFIG_EMAC_PHY_ADDR);
    } else if (found == 1) {
        cprintf("  1 PHY at %d, as the board file says. OK.\n",
                (int)CONFIG_EMAC_PHY_ADDR);
    } else {
        cprintf("  %u PHYs responded, including the expected one at %d. "
                "More than one is unexpected on this board.\n",
                found, (int)CONFIG_EMAC_PHY_ADDR);
    }
}
