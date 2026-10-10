/* The ESP32-P4's MIPI-DSI host and the LCD-7B's EK79007 panel. 47.4,
 * plan/phase47_esp32p4_lcd7b_ribbon.md.
 *
 * ## What this milestone does
 *
 * Pixels on the glass with no frame buffer and no DMA: the DSI host's own
 * video pattern generator (VPG) drives the panel. So the link, the PHY's PLL,
 * the panel's init sequence and the video timing are proven before 47.5 adds
 * a frame in PSRAM and the DW-GDMA that streams it.
 *
 * ## Where every write comes from
 *
 * ESP-IDF (Apache-2.0), traced call by call, in the order IDF makes them:
 *
 *   esp_ldo_acquire_channel()          LDO channel 3 at 2500 mV (the PHY)
 *   esp_lcd_new_dsi_bus()              bus clock, PHY clocks, mipi_dsi_hal_init(),
 *     + mipi_dsi_hal.c                 the PHY PLL, lanes, command-mode defaults
 *   esp_lcd_new_panel_io_dbi()         command-mode packet speeds (all LP)
 *   esp_lcd_new_panel_dpi()            DPI clock, host video config, bridge
 *   panel reset + EK79007 commands     Waveshare's sequence (displays_config.h)
 *   dpi_panel_init() minus the DMA     video mode on
 *   esp_lcd_dpi_panel_set_pattern()    VPG on, bridge DPI output off
 *
 * with the LL layers (esp_hal_lcd/esp32p4/include/hal/mipi_dsi_*_ll.h) taken
 * at their v3 (CHIP_SUPPORT_MIN_REV >= 300) branches. Register addresses and
 * fields come from the generated drivers/include/drivers/esp32p4_dsi_regs.h.
 *
 * The panel facts are Waveshare's (examples/arduino/libraries/displays/
 * displays_config.h and examples/esp-idf/07_color_panel): EK79007, 1024x600,
 * 2 lanes at 1000 Mbps, DPI 52 MHz, HSYNC 10/160/160, VSYNC 1/23/12; reset
 * GPIO33 (held low by R45 until we drive it), backlight GPIO32 active low
 * (cmake/board-esp32p4-lcd7b.cmake has the schematic reading).
 *
 * The floats IDF uses (lane rate, DPI clock ratios) are integers here, with
 * the same roundf() results: the kernel runs with the FPU off. */

#include "lugalos_config.h"
#include "drivers/dsi_esp32p4.h"   /* outside the guard: never an empty unit */

#if defined(CONFIG_BOARD_ESP32P4) && defined(CONFIG_DSI_LCD_RST_GPIO)

#if !defined(CONFIG_ESP32P4_REV) || CONFIG_ESP32P4_REV < 300
#error "drivers/dsi_esp32p4.c follows IDF's v3 DSI bridge and clock registers"
#endif

#include "drivers/esp32p4_dsi_regs.h"
#include "drivers/font8x16.h"
#include "arch/esp32p4_intr.h"
#include "arch/trap.h"
#include "kernel/console.h"
#include "kernel/devirq.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static inline void field_set(uintptr_t reg, unsigned s, uint32_t v_mask, uint32_t val) {
    uint32_t x = REG(reg);
    x &= ~(v_mask << s);
    x |= (val & v_mask) << s;
    REG(reg) = x;
}
#define FSET(reg, f, val) field_set((reg), f##_S, f##_V, (uint32_t)(val))
#define FGET(reg, f)      ((REG(reg) >> f##_S) & f##_V)

/* ---- the panel and the link ---------------------------------------------- */

#define LANES          2u
#define LANE_MBPS      1000u      /* EK79007_PANEL_BUS_DSI_2CH_CONFIG, Waveshare */
#define DPI_KHZ        52000u     /* what the panel wants */
#define HSW 10u
#define HBP 160u
#define HFP 160u
#define VSW 1u
#define VBP 23u
#define VFP 12u

#define XTAL_MHZ       40u        /* the PHY PLL's reference on v3 (DEFAULT, not _LEGACY) */
#define F240M_MHZ      240u       /* MIPI_DSI_DPI_CLK_SRC_DEFAULT = PLL_F240M */

/* esp_lcd_mipi_dsi_bus.c */
#define TIMEOUT_CLOCK_MHZ   10u
#define ESCAPE_CLOCK_MHZ    18u
#define LP_RX_TIMEOUT       0x7FFFu

/* hal/mipi_dsi_types.h */
#define DT_DCS_SHORT_WRITE_0      0x05u
#define DT_DCS_SHORT_WRITE_1      0x15u
#define DT_DCS_READ_0             0x06u
#define DT_SET_MAX_RETURN_PKT     0x37u
#define DT_DCS_LONG_WRITE         0x39u

/* The DSI host's DPI colour codings (MIPI_DSI_LL_COLOR_CODE_*). */
#define COLOR_CODE_24BIT          5u
/* VID_MODE_TYPE: burst mode with sync pulses, IDF's choice. */
#define VIDEO_BURST_WITH_SYNC_PULSES 2u

/* GPIO matrix and IO_MUX (as drivers/sdmmc_esp32p4.c). */
#define GPIO_BASE           0x500E0000u
#define IOMUX_BASE          0x500E1000u
#define IOMUX_PAD(n)        (IOMUX_BASE + 0x4 + 4u * (uint32_t)(n))
#define  IOMUX_MCU_SEL_S        12
#define  IOMUX_MCU_SEL_M        (7u << IOMUX_MCU_SEL_S)
#define  IOMUX_FUN_PU           (1u << 8)
#define  IOMUX_FUN_PD           (1u << 7)
#define  IOMUX_FUNC_GPIO        1u
#define GPIO_OUT_SEL(n)     (GPIO_BASE + 0x558 + 4u * (uint32_t)(n))
#define  GPIO_OUT_SEL_GPIO      256u
#define GPIO_OUT_W1TS_HI    (GPIO_BASE + 0x14)
#define GPIO_OUT_W1TC_HI    (GPIO_BASE + 0x18)
#define GPIO_ENABLE_W1TS_HI (GPIO_BASE + 0x30)

#define RST_GPIO   CONFIG_DSI_LCD_RST_GPIO
#define BL_GPIO    CONFIG_DSI_LCD_BL_GPIO
_Static_assert(RST_GPIO >= 32 && BL_GPIO >= 32, "the _HI GPIO registers are used");

/* eFuse BLK1 words (drivers/psram_esp32p4.c reads the same two). */
#define EFUSE_RD_MAC_SYS_2   0x5012d04cu
#define EFUSE_RD_MAC_SYS_3   0x5012d050u

/* ---- state -------------------------------------------------------------- */

static bool        g_up, g_tried;
static const char *g_reason = "not initialised";
static uint8_t     g_ldo_dref, g_ldo_mul;
static int         g_ldo_k, g_ldo_vos, g_ldo_c;      /* x1000, as IDF */
static uint16_t    g_pll_m;
static uint8_t     g_pll_n, g_hsfreq;
static uint32_t    g_dpi_div;
static uint32_t    g_host_hsa, g_host_hbp, g_host_hline, g_brg_hfp;
static uint32_t    g_cmd_errs;
static uint32_t    g_init_us;
static bool        g_bl_on;
static int         g_pwr_mode = -1;  /* DCS 0x0A after sleep out, read in command mode */
static const char *g_pattern = "none";

static uint32_t div_round(uint64_t a, uint64_t b) { return (uint32_t)((a + b / 2u) / b); }

static void gpio_out(unsigned gpio, bool high) {
    uint32_t pad = REG(IOMUX_PAD(gpio));
    pad &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_PU | IOMUX_FUN_PD);
    pad |= (IOMUX_FUNC_GPIO << IOMUX_MCU_SEL_S);
    REG(IOMUX_PAD(gpio)) = pad;
    REG(GPIO_OUT_SEL(gpio)) = GPIO_OUT_SEL_GPIO;
    if (high) REG(GPIO_OUT_W1TS_HI) = 1u << (gpio - 32u);
    else      REG(GPIO_OUT_W1TC_HI) = 1u << (gpio - 32u);
    REG(GPIO_ENABLE_W1TS_HI) = 1u << (gpio - 32u);
}

static bool wait_for(bool (*cond)(void), uint32_t timeout_us) {
    uint64_t t0 = time_get_us();
    while (!cond()) {
        if (time_get_us() - t0 > timeout_us) return false;
    }
    return true;
}

/* ---- 1. power: LDO channel 3 at 2500 mV --------------------------------------
 *
 * IDF's ldo_ll_voltage_to_dref_mul() for unit 2 (channel 3): no stored
 * dref/mul pair as channels 1 and 2 have, but per-chip K, Vos and C for the
 * formula, from BLK1, and the (dref, mul) pair that best meets the target.
 * Then esp_ldo_acquire_channel()'s order, as drivers/psram_esp32p4.c's
 * ldo2_on() and drivers/sdmmc_esp32p4.c's channel 4. */
#define LDO3_MV 2500

static void ldo3_on(void) {
    uint32_t w2 = REG(EFUSE_RD_MAC_SYS_2), w3 = REG(EFUSE_RD_MAC_SYS_3);
    unsigned blk = ((w2 >> 11) & 0x3u) * 100u + ((w2 >> 8) & 0x7u);
    int k = 1000, vos = 0, c = 1000;
    if (blk >= 1u) {
        unsigned ek = (w3 >> 6) & 0xFFu, ev = (w3 >> 14) & 0x3Fu, ec = (w3 >> 20) & 0x3Fu;
        if (ek) k   = (ek & 0x80u) ? -(int)(ek & 0x7Fu) + 975 : (int)ek + 975;
        if (ev) vos = (ev & 0x20u) ? -(int)(ev & 0x1Fu) - 3   : (int)ev - 3;
        if (ec) c   = (ec & 0x20u) ? -(int)(ec & 0x1Fu) + 990 : (int)ec + 990;
    }
    int best = 0x7FFFFFFF;
    uint8_t dref = 0, mul = 0;
    for (uint8_t d = 0; d < 16; d++) {
        int vref_20 = (d < 9) ? (10 + d) : (20 + (d - 9) * 2);
        for (uint8_t m = 0; m < 8; m++) {
            int vout = (vref_20 * k + 20 * vos) * (4000 + m * c);
            int diff = LDO3_MV * 80000 - vout;
            if (diff < 0) diff = -diff;
            if (diff < best) { best = diff; dref = d; mul = m; }
        }
    }
    g_ldo_dref = dref; g_ldo_mul = mul; g_ldo_k = k; g_ldo_vos = vos; g_ldo_c = c;

    FSET(PMU_EXT_LDO_P0_0P2A_ANA_REG, PMU_ANA_0P2A_EN_CUR_LIM_0, 1);
    FSET(PMU_EXT_LDO_P0_0P2A_REG, PMU_0P2A_TIEH_0, 0);               /* Vref x mul, not the rail */
    FSET(PMU_EXT_LDO_P0_0P2A_ANA_REG, PMU_ANA_0P2A_DREF_0, dref);
    FSET(PMU_EXT_LDO_P0_0P2A_ANA_REG, PMU_ANA_0P2A_MUL_0, mul);
    FSET(PMU_EXT_LDO_P0_0P2A_REG, PMU_0P2A_FORCE_TIEH_SEL_0, 1);     /* by software */
    FSET(PMU_EXT_LDO_P0_0P2A_REG, PMU_0P2A_TIEH_SEL_0, 0);
    FSET(PMU_EXT_LDO_P0_0P2A_ANA_REG, PMU_ANA_0P2A_EN_VDET_0, 1);
    FSET(PMU_EXT_LDO_P0_0P2A_REG, PMU_0P2A_XPD_0, 1);
    FSET(PMU_EXT_LDO_P0_0P2A_ANA_REG, PMU_ANA_0P2A_EN_CUR_LIM_0, 0);
    time_delay_us(200);    /* CONFIG_ESP_LDO_VOLTAGE_STABLE_DELAY_US */
}

/* ---- 2. the PHY: test-interface register writes -------------------------- */

/* mipi_dsi_hal_phy_write_register(): address on TESTCLK's falling edge with
 * TESTEN high, data on its rising edge with TESTEN low. */
static void phy_write(uint8_t addr, uint8_t val) {
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 0;                          /* clk 0, clear 0 */
    REG(DSI_HOST_PHY_TST_CTRL1_REG) = (1u << DSI_HOST_PHY_TESTEN_S) | addr;
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 1u << DSI_HOST_PHY_TESTCLK_S;
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 0;
    REG(DSI_HOST_PHY_TST_CTRL1_REG) = val;
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 1u << DSI_HOST_PHY_TESTCLK_S;
    REG(DSI_HOST_PHY_TST_CTRL0_REG) = 0;
}

/* soc_mipi_dsi_phy_pll_ranges[] (esp_hal_lcd/esp32p4/mipi_dsi_periph.c):
 * hsfreqrange for a lane rate, Mbps, from the first entry whose range holds it. */
static const struct { uint16_t lo, hi; uint8_t sel; } k_pll_ranges[] = {
    {80, 89, 0x00}, {90, 99, 0x10}, {100, 109, 0x20}, {110, 129, 0x01},
    {130, 139, 0x11}, {140, 149, 0x21}, {150, 169, 0x02}, {170, 179, 0x12},
    {180, 199, 0x22}, {200, 219, 0x03}, {220, 239, 0x13}, {240, 249, 0x23},
    {250, 269, 0x04}, {270, 299, 0x14}, {300, 329, 0x05}, {330, 359, 0x15},
    {360, 399, 0x25}, {400, 449, 0x06}, {450, 499, 0x16}, {500, 549, 0x07},
    {550, 599, 0x17}, {600, 649, 0x08}, {650, 699, 0x18}, {700, 749, 0x09},
    {750, 799, 0x19}, {800, 849, 0x29}, {850, 899, 0x39}, {900, 949, 0x0A},
    {950, 999, 0x1A}, {1000, 1049, 0x2A}, {1050, 1099, 0x3A}, {1100, 1149, 0x0B},
    {1150, 1199, 0x1B}, {1200, 1249, 0x2B}, {1250, 1299, 0x3B}, {1300, 1349, 0x0C},
    {1350, 1399, 0x1C}, {1400, 1449, 0x2C}, {1450, 1500, 0x3C},
};

/* mipi_dsi_hal_configure_phy_pll(): f_vco = M/N * f_ref, 5 <= f_ref/N <= 40
 * MHz, M even; the first (N, M) that hits the rate exactly wins. For 1000
 * Mbps from 40 MHz that is N 2, M 50. */
static bool phy_pll_config(void) {
    unsigned min_n = XTAL_MHZ / 40u ? XTAL_MHZ / 40u : 1u, max_n = XTAL_MHZ / 5u;
    uint32_t best = 0xFFFFFFFFu;
    g_pll_m = 0; g_pll_n = 1;
    for (unsigned n = min_n; n <= max_n; n++) {
        uint32_t m = LANE_MBPS * n / XTAL_MHZ;
        if (m & 1u) continue;
        uint32_t got_x1000 = XTAL_MHZ * 1000u * m / n;
        uint32_t delta = got_x1000 > LANE_MBPS * 1000u ? got_x1000 - LANE_MBPS * 1000u
                                                       : LANE_MBPS * 1000u - got_x1000;
        if (delta < best) {
            best = delta; g_pll_m = (uint16_t)m; g_pll_n = (uint8_t)n;
            if (delta < 10u) break;
        }
    }
    if (!g_pll_m) return false;
    g_hsfreq = 0;
    for (size_t i = 0; i < sizeof k_pll_ranges / sizeof k_pll_ranges[0]; i++) {
        if (LANE_MBPS >= k_pll_ranges[i].lo && LANE_MBPS <= k_pll_ranges[i].hi) {
            g_hsfreq = k_pll_ranges[i].sel;
            break;
        }
    }
    phy_write(0x44, (uint8_t)(g_hsfreq << 1));
    phy_write(0x22, LANE_MBPS >= 1000u ? 0x88 : 0x80);   /* 1-1.5 Gbps analog support */
    phy_write(0x19, 0x30);                               /* use N and M from 0x17/0x18 */
    phy_write(0x17, (uint8_t)(g_pll_n - 1u));
    phy_write(0x18, (uint8_t)((g_pll_m - 1u) & 0x1Fu));
    phy_write(0x18, (uint8_t)(0x80u | (((g_pll_m - 1u) >> 5) & 0x0Fu)));
    return true;
}

static bool pll_locked(void) { return FGET(DSI_HOST_PHY_STATUS_REG, DSI_HOST_PHY_LOCK) != 0; }

static bool lanes_stopped(void) {
    uint32_t s = REG(DSI_HOST_PHY_STATUS_REG);
    uint32_t mask = (1u << DSI_HOST_PHY_STOPSTATECLKLANE_S) | (1u << DSI_HOST_PHY_STOPSTATE0LANE_S);
    if (LANES > 1u) mask |= 1u << DSI_HOST_PHY_STOPSTATE1LANE_S;
    return (s & mask) == mask;
}

/* ---- 3. the bus: esp_lcd_new_dsi_bus() ------------------------------------ */

static bool dsi_bus_init(void) {
    /* The APB clock for the host and bridge registers, and the bridge's reset. */
    FSET(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_DSI_SYS_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DSI_BRG, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_DSI_BRG, 0);

    /* The PHY's configuration clock: PLL_F20M (gate opened), source 0. Its
     * PLL reference: XTAL (source 0), undivided. */
    FSET(HP_SYS_CLKRST_REF_CLK_CTRL2_REG, HP_SYS_CLKRST_REG_REF_20M_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL02_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CLK_SRC_SEL, 0);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CFG_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_SRC_SEL, 0);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_DIV_NUM, 0);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN, 1);

    /* mipi_dsi_hal_init() */
    FSET(DSI_HOST_PHY_IF_CFG_REG, DSI_HOST_N_LANES, LANES - 1u);
    FSET(DSI_HOST_PWR_UP_REG, DSI_HOST_SHUTDOWNZ, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_SHUTDOWNZ, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_RSTZ, 0);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_RSTZ, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_ENABLECLK, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_FORCEPLL, 1);
    FSET(DSI_BRG_EN_REG, DSI_BRG_DSI_BRIG_RST, 1);
    FSET(DSI_BRG_EN_REG, DSI_BRG_DSI_BRIG_RST, 0);

    if (!phy_pll_config()) { g_reason = "no PHY PLL M/N for the lane rate"; return false; }
    if (!wait_for(pll_locked, 100000u)) { g_reason = "the DSI PHY PLL did not lock"; return false; }
    if (!wait_for(lanes_stopped, 100000u)) { g_reason = "the DSI lanes did not reach stop state"; return false; }

    /* Command mode; clock lane managed by the host (AUTO). */
    FSET(DSI_HOST_MODE_CFG_REG, DSI_HOST_CMD_VIDEO_MODE, 1);
    FSET(DSI_HOST_LPCLK_CTRL_REG, DSI_HOST_AUTO_CLKLANE_CTRL, 1);
    FSET(DSI_HOST_LPCLK_CTRL_REG, DSI_HOST_PHY_TXREQUESTCLKHS, 1);
    /* HS<->LP switch times: data 50/104, clock 46/128 (byte clocks). */
    FSET(DSI_HOST_PHY_TMR_CFG_REG, DSI_HOST_PHY_HS2LP_TIME, 50);
    FSET(DSI_HOST_PHY_TMR_CFG_REG, DSI_HOST_PHY_LP2HS_TIME, 104);
    FSET(DSI_HOST_PHY_TMR_LPCLK_CFG_REG, DSI_HOST_PHY_CLKHS2LP_TIME, 46);
    FSET(DSI_HOST_PHY_TMR_LPCLK_CFG_REG, DSI_HOST_PHY_CLKLP2HS_TIME, 128);
    FSET(DSI_HOST_PCKHDL_CFG_REG, DSI_HOST_CRC_RX_EN, 1);
    FSET(DSI_HOST_PCKHDL_CFG_REG, DSI_HOST_ECC_RX_EN, 1);
    FSET(DSI_HOST_PCKHDL_CFG_REG, DSI_HOST_EOTP_TX_EN, 1);
    FSET(DSI_HOST_PCKHDL_CFG_REG, DSI_HOST_EOTP_TX_LP_EN, 0);
    /* Both dividers from the HS byte clock, LANE_MBPS / 8: roundf(12.5) = 13
     * for the 10 MHz timeout clock, roundf(6.94) = 7 for the 18 MHz escape. */
    FSET(DSI_HOST_CLKMGR_CFG_REG, DSI_HOST_TO_CLK_DIVISION, div_round(LANE_MBPS, 8u * TIMEOUT_CLOCK_MHZ));
    FSET(DSI_HOST_CLKMGR_CFG_REG, DSI_HOST_TX_ESC_CLK_DIVISION, div_round(LANE_MBPS, 8u * ESCAPE_CLOCK_MHZ));
    FSET(DSI_HOST_TO_CNT_CFG_REG, DSI_HOST_HSTX_TO_CNT, 0);
    FSET(DSI_HOST_TO_CNT_CFG_REG, DSI_HOST_LPRX_TO_CNT, LP_RX_TIMEOUT);
    REG(DSI_HOST_HS_RD_TO_CNT_REG) = 0;
    REG(DSI_HOST_LP_RD_TO_CNT_REG) = 0;
    REG(DSI_HOST_HS_WR_TO_CNT_REG) = 0;
    REG(DSI_HOST_LP_WR_TO_CNT_REG) = 0;
    REG(DSI_HOST_BTA_TO_CNT_REG) = 0;
    FSET(DSI_HOST_PHY_TMR_RD_CFG_REG, DSI_HOST_MAX_RD_TIME, 6000);
    FSET(DSI_HOST_PHY_IF_CFG_REG, DSI_HOST_PHY_STOP_WAIT_TIME, 0x3F);

    /* esp_lcd_new_panel_io_dbi(): no tearing effect, ACKs requested, and
     * every generic and DCS packet type sent in low-power mode. */
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_TEAR_FX_EN, 0);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_ACK_RQST_EN, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_SW_0P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_SW_1P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_SW_2P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_LW_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_SR_0P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_SR_1P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_GEN_SR_2P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_DCS_SW_0P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_DCS_SW_1P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_DCS_LW_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_DCS_SR_0P_TX, 1);
    FSET(DSI_HOST_CMD_MODE_CFG_REG, DSI_HOST_MAX_RD_PKT_SIZE, 1);
    return true;
}

/* ---- 4. the video side: esp_lcd_new_panel_dpi() minus DMA and interrupts -- */

/* The bridge's input pixel format and frame size: mipi_dsi_brg_ll_set_num_
 * pixel_bits() and _set_input_color_format() (v3 branch). raw_type 0 =
 * RGB888, 2 = RGB565, 12 = GRAY8; the output to the host is always RGB888
 * (dpi_type 0), so the bridge expands gray to R = G = B. */
static void bridge_format(uint32_t bpp, uint32_t raw_type) {
    uint32_t bits = DSI_LCD_H_RES * DSI_LCD_V_RES * bpp;
    FSET(DSI_BRG_RAW_NUM_CFG_REG, DSI_BRG_RAW_NUM_TOTAL, (bits + 63u) / 64u);
    FSET(DSI_BRG_RAW_NUM_CFG_REG, DSI_BRG_UNALIGN_64BIT_EN, (bits % 64u) ? 1 : 0);
    FSET(DSI_BRG_RAW_NUM_CFG_REG, DSI_BRG_RAW_NUM_TOTAL_SET, 1);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_RAW_TYPE, raw_type);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DATA_IN_TYPE, 0);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DPI_TYPE, 0);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DPI_CONFIG, 0);
}

static void dpi_config(void) {
    /* The DPI clock: PLL_F240M (gate opened) / roundf(240 / 52) = 5, so 48
     * MHz real against 52 expected; the timings below absorb the difference
     * exactly as IDF's do. */
    g_dpi_div = div_round(F240M_MHZ * 1000u, DPI_KHZ);
    uint32_t real_khz = F240M_MHZ * 1000u / g_dpi_div;
    FSET(HP_SYS_CLKRST_REF_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_REF_240M_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_SRC_SEL, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_DIV_NUM, g_dpi_div - 1u);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPICLK_EN, 1);

    FSET(DSI_HOST_DPI_VCID_REG, DSI_HOST_DPI_VCID, 0);
    FSET(DSI_HOST_DPI_COLOR_CODING_REG, DSI_HOST_DPI_COLOR_CODING, COLOR_CODE_24BIT);
    REG(DSI_HOST_DPI_CFG_POL_REG) = 0;                      /* all active high */
    /* LP allowed in every blanking period, commands in LP (flags.disable_lp 0). */
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_HBP_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_HFP_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_VSA_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_VBP_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_VFP_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_VACT_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_LP_CMD_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_FRAME_BTA_ACK_EN, 1);
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VID_MODE_TYPE, VIDEO_BURST_WITH_SYNC_PULSES);
    FSET(DSI_HOST_VID_PKT_SIZE_REG, DSI_HOST_VID_PKT_SIZE, DSI_LCD_H_RES);
    FSET(DSI_HOST_VID_NUM_CHUNKS_REG, DSI_HOST_VID_NUM_CHUNKS, 0);
    FSET(DSI_HOST_VID_NULL_SIZE_REG, DSI_HOST_VID_NULL_SIZE, 0);

    /* mipi_dsi_hal_host_dpi_set_horizontal_timing(): the host counts in lane
     * byte clocks, so each DPI-clock span is scaled by LANE_MBPS / DPI / 8 =
     * 1000 / 52 / 8, and the line's rounding error is taken out of the active
     * span. The bridge counts at the *real* DPI clock, so its front porch
     * shrinks until a line lasts as long as the host's. */
    uint32_t htotal = HSW + HBP + DSI_LCD_H_RES + HFP;
    uint64_t num = (uint64_t)LANE_MBPS * 1000u, den = (uint64_t)DPI_KHZ * 8u;
    uint32_t h_hsw = div_round(HSW * num, den);
    uint32_t h_hbp = div_round(HBP * num, den);
    uint32_t h_act = div_round((uint64_t)DSI_LCD_H_RES * num, den);
    uint32_t h_hfp = div_round(HFP * num, den);
    uint32_t h_tot = div_round((uint64_t)htotal * num, den);
    h_act = h_act + h_tot - (h_hsw + h_hbp + h_act + h_hfp);   /* compensation, may be -1 */
    g_host_hsa = h_hsw; g_host_hbp = h_hbp; g_host_hline = h_hsw + h_hbp + h_act + h_hfp;
    FSET(DSI_HOST_VID_HSA_TIME_REG, DSI_HOST_VID_HSA_TIME, g_host_hsa);
    FSET(DSI_HOST_VID_HBP_TIME_REG, DSI_HOST_VID_HBP_TIME, g_host_hbp);
    FSET(DSI_HOST_VID_HLINE_TIME_REG, DSI_HOST_VID_HLINE_TIME, g_host_hline);
    int32_t comp = (int32_t)div_round((uint64_t)real_khz * htotal, DPI_KHZ) - (int32_t)htotal;
    g_brg_hfp = (uint32_t)((int32_t)HFP + comp);
    FSET(DSI_BRG_DPI_H_CFG0_REG, DSI_BRG_HDISP, DSI_LCD_H_RES);
    FSET(DSI_BRG_DPI_H_CFG0_REG, DSI_BRG_HTOTAL, HSW + HBP + DSI_LCD_H_RES + g_brg_hfp);
    FSET(DSI_BRG_DPI_H_CFG1_REG, DSI_BRG_HSYNC, HSW);
    FSET(DSI_BRG_DPI_H_CFG1_REG, DSI_BRG_HBANK, HBP);

    /* Vertical: lines on both sides. */
    FSET(DSI_HOST_VID_VSA_LINES_REG, DSI_HOST_VSA_LINES, VSW);
    FSET(DSI_HOST_VID_VBP_LINES_REG, DSI_HOST_VBP_LINES, VBP);
    FSET(DSI_HOST_VID_VACTIVE_LINES_REG, DSI_HOST_V_ACTIVE_LINES, DSI_LCD_V_RES);
    FSET(DSI_HOST_VID_VFP_LINES_REG, DSI_HOST_VFP_LINES, VFP);
    FSET(DSI_BRG_DPI_V_CFG0_REG, DSI_BRG_VDISP, DSI_LCD_V_RES);
    FSET(DSI_BRG_DPI_V_CFG0_REG, DSI_BRG_VTOTAL, VSW + VBP + DSI_LCD_V_RES + VFP);
    FSET(DSI_BRG_DPI_V_CFG1_REG, DSI_BRG_VSYNC, VSW);
    FSET(DSI_BRG_DPI_V_CFG1_REG, DSI_BRG_VBANK, VBP);

    /* The bridge: a whole frame per DMA block, the DMA as flow controller,
     * IDF's burst and threshold; the input format is bridge_format()'s
     * (RGB888 until 47.5's frame buffer picks one). The DPI output stays
     * off: the pattern generator needs none. */
    bridge_format(24u, 0u);
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_FIFO_UNDERRUN_DISCARD_VCNT, DSI_LCD_H_RES);
    FSET(DSI_BRG_DMA_FLOW_CTRL_REG, DSI_BRG_DSI_DMA_FLOW_CONTROLLER, 0);
    FSET(DSI_BRG_DMA_FLOW_CTRL_REG, DSI_BRG_DMA_FLOW_MULTIBLK_NUM, 1);
    FSET(DSI_BRG_DMA_FRAME_INTERVAL_REG, DSI_BRG_DMA_MULTIBLK_EN, 0);
    FSET(DSI_BRG_DMA_REQ_CFG_REG, DSI_BRG_DMA_BURST_LEN, 256);
    FSET(DSI_BRG_RAW_BUF_ALMOST_EMPTY_THRD_REG, DSI_BRG_DSI_RAW_BUF_ALMOST_EMPTY_THRD, 1024 - 256);
    FSET(DSI_BRG_EN_REG, DSI_BRG_DSI_EN, 1);
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_DPI_EN, 0);
    FSET(DSI_BRG_DPI_CONFIG_UPDATE_REG, DSI_BRG_DPI_CONFIG_UPDATE, 1);
}

/* ---- 5. commands: mipi_dsi_hal_host_gen_write_dcs_command() -------------- */

static bool cmd_fifo_not_full(void)  { return !FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_CMD_FULL); }
static bool pld_fifo_not_full(void)  { return !FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_PLD_W_FULL); }
static bool cmd_fifo_drained(void) {
    return FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_CMD_EMPTY) &&
           FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_PLD_W_EMPTY);
}

static void gen_header(uint8_t dt, uint8_t msb, uint8_t lsb) {
    wait_for(cmd_fifo_not_full, 10000u);
    REG(DSI_HOST_GEN_HDR_REG) = ((uint32_t)msb << 16) | ((uint32_t)lsb << 8) | (0u << 6) | dt;
}

static void gen_payload(uint32_t w) {
    wait_for(pld_fifo_not_full, 10000u);
    REG(DSI_HOST_GEN_PLD_DATA_REG) = w;
}

/* One DCS write, VC 0, then wait until the host has sent it (IDF does not
 * wait; we do, so an error is pinned to the command that caused it). Returns
 * false if the FIFOs did not drain or the host flagged an error. */
static bool dcs_write(uint8_t cmd, const uint8_t *param, unsigned n) {
    (void)REG(DSI_HOST_INT_ST0_REG);                     /* read-to-clear */
    (void)REG(DSI_HOST_INT_ST1_REG);
    if (n == 0) {
        gen_header(DT_DCS_SHORT_WRITE_0, 0, cmd);
    } else if (n == 1) {
        gen_header(DT_DCS_SHORT_WRITE_1, param[0], cmd);
    } else {
        unsigned total = n + 1u, i = 0;
        uint32_t w = cmd;
        for (unsigned b = 1; b < total; b++) {
            w |= (uint32_t)param[b - 1u] << (8u * (b & 3u));
            if ((b & 3u) == 3u) { gen_payload(w); w = 0; }
            i = b;
        }
        if ((i & 3u) != 3u) gen_payload(w);
        gen_header(DT_DCS_LONG_WRITE, (uint8_t)(total >> 8), (uint8_t)total);
    }
    bool drained = wait_for(cmd_fifo_drained, 20000u);
    uint32_t e0 = REG(DSI_HOST_INT_ST0_REG), e1 = REG(DSI_HOST_INT_ST1_REG);
    if (!drained || e0 || e1) {
        g_cmd_errs++;
        printk("[LCD] DCS 0x%02x: %s, int_st0 0x%08lx int_st1 0x%08lx\n", (unsigned)cmd,
               drained ? "sent" : "FIFO did not drain", (unsigned long)e0, (unsigned long)e1);
        return false;
    }
    return true;
}

static bool rd_not_busy(void) {
    return !FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_RD_CMD_BUSY);
}

/* mipi_dsi_hal_host_gen_read_dcs_command(): max return size, BTA on, the
 * read, then whatever arrived in the read FIFO. Returns the bytes read. */
static int dcs_read(uint8_t cmd, uint8_t *out, unsigned n) {
    (void)REG(DSI_HOST_INT_ST0_REG);
    (void)REG(DSI_HOST_INT_ST1_REG);
    gen_header(DT_SET_MAX_RETURN_PKT, (uint8_t)(n >> 8), (uint8_t)n);
    FSET(DSI_HOST_PCKHDL_CFG_REG, DSI_HOST_BTA_EN, 1);
    FSET(DSI_HOST_GEN_VCID_REG, DSI_HOST_GEN_VCID_RX, 0);
    gen_header(DT_DCS_READ_0, 0, cmd);
    time_delay_us(100);
    if (!wait_for(rd_not_busy, 50000u) || FGET(DSI_HOST_INT_ST1_REG, DSI_HOST_TO_LP_RX)) return -1;
    unsigned got = 0;
    uint64_t t0 = time_get_us();
    while (FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_PLD_R_EMPTY)) {
        if (time_get_us() - t0 > 50000u) return 0;
    }
    while (!FGET(DSI_HOST_CMD_PKT_STATUS_REG, DSI_HOST_GEN_PLD_R_EMPTY)) {
        uint32_t w = REG(DSI_HOST_GEN_PLD_DATA_REG);
        for (unsigned i = 0; i < 4u; i++)
            if (got < n) out[got++] = (uint8_t)(w >> (8u * i));
    }
    return (int)got;
}

/* ---- 6. the panel ----------------------------------------------------------- */

/* Waveshare's EK79007 sequence (displays_config.h, which says it follows
 * their BSP): 0xB2 0x10 selects 2-lane pad control, then vendor registers,
 * then sleep out and 120 ms. */
static const struct { uint8_t cmd, n, p, delay_ms; } k_ek79007_init[] = {
    {0xB2, 1, 0x10, 0},
    {0x80, 1, 0x8B, 0},
    {0x81, 1, 0x78, 0},
    {0x82, 1, 0x84, 0},
    {0x83, 1, 0x88, 0},
    {0x84, 1, 0xA8, 0},
    {0x85, 1, 0xE3, 0},
    {0x86, 1, 0x88, 0},
    {0x11, 0, 0x00, 120},
};

static void panel_reset(void) {
    gpio_out(RST_GPIO, false);
    task_sleep_ms(10);
    gpio_out(RST_GPIO, true);
    task_sleep_ms(120);
}

static bool panel_init(void) {
    bool ok = true;
    for (size_t i = 0; i < sizeof k_ek79007_init / sizeof k_ek79007_init[0]; i++) {
        ok &= dcs_write(k_ek79007_init[i].cmd, &k_ek79007_init[i].p, k_ek79007_init[i].n);
        if (k_ek79007_init[i].delay_ms) task_sleep_ms(k_ek79007_init[i].delay_ms);
    }
    return ok;
}

static void backlight(bool on) {
    gpio_out(BL_GPIO, !on);          /* active low: low = full brightness */
    g_bl_on = on;
}

/* ---- 7. the pattern generator --------------------------------------------- */

typedef enum { PAT_NONE, PAT_BARS, PAT_HBARS, PAT_BER } pattern_t;

static void set_pattern(pattern_t p) {
    /* The bridge's DPI stream off whenever the generator is on. */
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_DPI_EN, 0);
    FSET(DSI_BRG_DPI_CONFIG_UPDATE_REG, DSI_BRG_DPI_CONFIG_UPDATE, 1);
    switch (p) {
    case PAT_BARS:
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_MODE, 0);
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_ORIENTATION, 0);
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 1);
        g_pattern = "vertical colour bars";
        break;
    case PAT_HBARS:
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_MODE, 0);
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_ORIENTATION, 1);
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 1);
        g_pattern = "horizontal colour bars";
        break;
    case PAT_BER:
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_MODE, 1);
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_ORIENTATION, 0);
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 1);
        g_pattern = "BER pattern";
        break;
    case PAT_NONE:
        /* No frame source until 47.5: what the panel shows is undefined. */
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 0);
        g_pattern = "none";
        break;
    }
}


/* ---- 8. scan-out from a PSRAM frame: DW-GDMA into the bridge (47.5) --------
 *
 * IDF's esp_lcd_new_panel_dpi() / dpi_panel_init() / dw_gdma.c, with one
 * frame buffer: a single link-list item carries the whole frame from PSRAM
 * (memory master port 1) to the bridge's FIFO window (master port 0,
 * MIPI_DSI_BRG_MEM_BASE), 64-bit beats, the bridge's hardware handshake,
 * the DMA as flow controller. The item is marked last, so the channel stops
 * after each frame and the transfer-done interrupt re-arms it -- IDF's
 * mipi_dsi_dma_trans_done_cb(), with ~0.9 ms of vertical blanking plus the
 * bridge FIFO to do it in.
 *
 * The item lives in L2MEM and is only ever touched through the non-cached
 * alias (SOC_NON_CACHEABLE_OFFSET_SRAM, as IDF's DW_GDMA_GET_NON_CACHE_ADDR),
 * so the write-back L1 data cache never holds a stale copy of what the DMA
 * reads. The frame buffer is cached PSRAM: whoever draws writes it back
 * (dsi_lcd_fb_flush()) before the DMA can be trusted to see it. */

#define MIPI_DSI_BRG_MEM_BASE      0x50105000u   /* hw_ver3/soc/reg_base.h */
#define SRAM_NONCACHE_OFFSET       0x40000000u   /* SOC_NON_CACHEABLE_OFFSET_SRAM */
#define GDMA_INTR_SOURCE           24u           /* ETS_DW_GDMA_INTR_SOURCE */
#define GDMA_CH                    0u            /* "CH1" in the register names */

/* dw_gdma_ll.h: channel events. */
#define GDMA_EV_BLOCK_TFR_DONE     (1u << 0)
#define GDMA_EV_DMA_TFR_DONE       (1u << 1)
#define GDMA_EV_ERRORS             (0x3fe0u)     /* bits 5..13: decode/slave/LLI errors */

/* CTL0 (ctrl_lo) and CTL1 (ctrl_hi) of the item, from IDF's dpi_panel_init()
 * transfer config: src memory port 1, increment, 64-bit, burst 512; dst DSI
 * port 0, fixed, 64-bit, burst 256; AXI burst length 16 both ways. */
#define LLI_CTL0  ((1u << 0) /* SMS = memory */ | (0u << 2) /* DMS = DSI */ | \
                   (0u << 4) /* SINC inc */ | (1u << 6) /* DINC fixed */ | \
                   (3u << 8) /* SRC 64-bit */ | (3u << 11) /* DST 64-bit */ | \
                   (8u << 14) /* SRC_MSIZE 512 */ | (7u << 18) /* DST_MSIZE 256 */)
#define LLI_CTL1  ((1u << 6) | (16u << 7) /* ARLEN 16 */ | (1u << 15) | (16u << 16) /* AWLEN 16 */ | \
                   (1u << 30) /* LAST */ | (1u << 31) /* VALID */)

typedef struct {
    uint32_t sar_lo, sar_hi, dar_lo, dar_hi;
    uint32_t block_ts, reserved_14;
    uint32_t llp_lo, llp_hi;
    uint32_t ctl_lo, ctl_hi;
    uint32_t sstat, dstat, status_lo, status_hi;
    uint32_t reserved_38, reserved_3c;
} gdma_lli_t;
_Static_assert(sizeof(gdma_lli_t) == 64, "DW_GDMA_LL_LINK_LIST_ALIGNMENT");

static gdma_lli_t g_lli_mem __attribute__((aligned(64)));
#define LLI_NC ((volatile gdma_lli_t *)((uintptr_t)&g_lli_mem + SRAM_NONCACHE_OFFSET))

typedef enum { FB_NONE = 0, FB_GRAY8, FB_RGB565 } fb_fmt_t;
static fb_fmt_t          g_fb_fmt;
static uint8_t          *g_fb;
static uint32_t          g_fb_bytes, g_fb_pages, g_fb_bpp;
static volatile uint32_t g_frames, g_dma_errs, g_dma_last_err;
static volatile bool     g_stop_at_frame_end;   /* ISR: finish this frame, do not re-arm */
static bool              g_gdma_ready;

static void lli_arm(void) {
    volatile gdma_lli_t *l = LLI_NC;
    l->sar_lo = (uint32_t)(uintptr_t)g_fb;
    l->sar_hi = 0;
    l->dar_lo = MIPI_DSI_BRG_MEM_BASE;
    l->dar_hi = 0;
    l->block_ts = g_fb_bytes / 8u - 1u;          /* 64-bit items */
    l->llp_lo = 1u;                              /* LMS = memory port; next = none */
    l->llp_hi = 0;
    l->ctl_lo = LLI_CTL0;
    l->ctl_hi = LLI_CTL1;
}

/* The re-arm, IDF's mipi_dsi_dma_trans_done_cb(): valid and last again (the
 * DMA clears VALID as it consumes the item), the head pointer, enable. */
static void gdma_restart(void) {
    LLI_NC->ctl_hi = LLI_CTL1;
    REG(DMAC_CH1_LLP0_REG) = ((uint32_t)(uintptr_t)&g_lli_mem & ~0x3fu) | 1u;   /* LOC0, LMS = memory */
    REG(DMAC_CH1_LLP1_REG) = 0;
    REG(DMAC_CHEN0_REG) = 0x101u << GDMA_CH;
}

static void gdma_isr(void *ctx) {
    (void)ctx;
    uint32_t st = REG(DMAC_CH1_INTSTATUS0_REG);
    REG(DMAC_CH1_INTCLEAR0_REG) = st;
    if (st & GDMA_EV_ERRORS) { g_dma_errs++; g_dma_last_err = st; }
    if (st & GDMA_EV_DMA_TFR_DONE) {
        g_frames++;
        if (g_fb && !g_stop_at_frame_end) gdma_restart();
    }
}

/* esp_lcd's dw_gdma_new_channel() for the DPI panel, once. */
static void gdma_init(void) {
    if (g_gdma_ready) return;
    /* The item was zeroed with .bss, through L1D: push that line out and
     * drop it, so no cached copy can later be evicted over what is written
     * through the non-cached alias (IDF's esp_cache_msync(C2M | INVALIDATE)
     * in dw_gdma_new_link_list()). */
    esp32p4_dcache_writeback((uintptr_t)&g_lli_mem, sizeof g_lli_mem);
    esp32p4_dcache_invalidate((uintptr_t)&g_lli_mem, sizeof g_lli_mem);
    FSET(HP_SYS_CLKRST_SOC_CLK_CTRL0_REG, HP_SYS_CLKRST_REG_GDMA_CPU_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_GDMA_SYS_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_GDMA, 1);
    FSET(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_GDMA, 0);
    FSET(DMAC_RESET0_REG, DMAC_DMAC_RST, 1);
    uint64_t t0 = time_get_us();
    while (FGET(DMAC_RESET0_REG, DMAC_DMAC_RST) && time_get_us() - t0 < 1000u) { }
    FSET(DMAC_CFG0_REG, DMAC_DMAC_EN, 1);
    FSET(DMAC_CFG0_REG, DMAC_INT_EN, 1);

    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_TT_FC, 1);              /* memory -> peripheral, DMA controls */
    FSET(DMAC_CH1_CFG0_REG, DMAC_CH1_SRC_MULTBLK_TYPE, 3);   /* link list */
    FSET(DMAC_CH1_CFG0_REG, DMAC_CH1_DST_MULTBLK_TYPE, 3);
    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_HS_SEL_SRC, 0);         /* hardware handshake */
    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_HS_SEL_DST, 0);
    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_DST_PER, 0);            /* the DSI bridge */
    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_CH_PRIOR, 1);
    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_SRC_OSR_LMT, 5u - 1u);
    FSET(DMAC_CH1_CFG1_REG, DMAC_CH1_DST_OSR_LMT, 2u - 1u);
    REG(DMAC_CH1_INTSTATUS_ENABLE0_REG) = 0xffffffffu;
    REG(DMAC_CH1_INTCLEAR0_REG) = 0xffffffffu;
    REG(DMAC_CH1_INTSIGNAL_ENABLE0_REG) = GDMA_EV_DMA_TFR_DONE | GDMA_EV_ERRORS;

    if (esp32p4_intmtx_route(GDMA_INTR_SOURCE, ESP32P4_CLIC_IRQ_GDMA) == 0 &&
        devirq_attach(ESP32P4_CLIC_IRQ_GDMA, gdma_isr, NULL) == 0) {
        arch_irq_enable(ESP32P4_CLIC_IRQ_GDMA);
        g_gdma_ready = true;
    }
}

static void gdma_stop(void) {
    REG(DMAC_CHEN0_REG) = 0x100u << GDMA_CH;                 /* disable, with write-enable */
    uint64_t t0 = time_get_us();
    while ((REG(DMAC_CHEN0_REG) & (1u << GDMA_CH)) && time_get_us() - t0 < 50000u) { }
}

/* Write back [y0, y1) of the frame so the DMA sees it (§4: the CPU writes
 * PSRAM through L1D and L2; the DMA reads the chip). */
void dsi_lcd_fb_flush(unsigned y0, unsigned y1) {
    if (!g_fb || y0 >= y1) return;
    if (y1 > DSI_LCD_V_RES) y1 = DSI_LCD_V_RES;
    uint32_t row = DSI_LCD_H_RES * g_fb_bpp / 8u;
    esp32p4_extmem_writeback((uintptr_t)g_fb + y0 * row, (y1 - y0) * row);
}

uint8_t *dsi_lcd_fb(void) { return g_fb; }

/* dpi_panel_init() for one frame buffer: frame in PSRAM, bridge format, the
 * item, channel on, bridge DPI output on, pattern generator off. */
static bool fb_start(fb_fmt_t fmt) {
    if (!dsi_lcd_init()) return false;
    gdma_init();
    if (!g_gdma_ready) { cprintf("lcd: the DW-GDMA interrupt could not be routed\n"); return false; }

    /* Stop whatever runs, at a frame boundary: the bridge has no notion of
     * where a frame starts other than "the next byte after the last one",
     * so a DMA cut off mid-frame leaves the rest in its FIFO and every
     * later frame comes out shifted by that remainder (seen: the rightmost
     * 2-3 pixels wrapped to the left edge after a GRAY8 -> RGB565 -> GRAY8
     * switch). So: let the running frame finish, let the bridge drain its
     * FIFO onto the glass, and only then stop the DPI stream. */
    uint8_t *old = g_fb; uint32_t old_pages = g_fb_pages;
    if (old) {
        g_stop_at_frame_end = true;
        uint32_t f0 = g_frames;
        uint64_t t0 = time_get_us();
        while (g_frames == f0 && time_get_us() - t0 < 50000u) { }
        t0 = time_get_us();
        while (FGET(DSI_BRG_FIFO_FLOW_STATUS_REG, DSI_BRG_RAW_BUF_DEPTH) && time_get_us() - t0 < 20000u) { }
    }
    g_fb = NULL;
    gdma_stop();
    g_stop_at_frame_end = false;
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_DPI_EN, 0);
    FSET(DSI_BRG_DPI_CONFIG_UPDATE_REG, DSI_BRG_DPI_CONFIG_UPDATE, 1);
    if (old) palloc_free(old, old_pages);

    uint32_t bpp = (fmt == FB_RGB565) ? 16u : 8u;
    uint32_t bytes = DSI_LCD_H_RES * DSI_LCD_V_RES * bpp / 8u;
    uint32_t pages = (bytes + 4095u) / 4096u;
    uint8_t *fb = palloc_pages_bulk(pages);
    if (!fb || !palloc_is_bulk(fb)) {
        if (fb) palloc_free(fb, pages);
        cprintf("lcd: no %lu KB run of PSRAM for the frame\n", (unsigned long)(bytes / 1024u));
        g_fb_fmt = FB_NONE;
        return false;
    }
    memset(fb, 0, bytes);
    esp32p4_extmem_writeback((uintptr_t)fb, bytes);

    g_fb_bpp = bpp; g_fb_bytes = bytes; g_fb_pages = pages; g_fb_fmt = fmt;
    bridge_format(bpp, fmt == FB_RGB565 ? 2u : 12u);
    g_fb = fb;
    lli_arm();
    REG(DMAC_CH1_INTCLEAR0_REG) = 0xffffffffu;
    gdma_restart();

    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 0);
    g_pattern = "none (frame buffer)";
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_DPI_EN, 1);
    FSET(DSI_BRG_DPI_CONFIG_UPDATE_REG, DSI_BRG_DPI_CONFIG_UPDATE, 1);
    REG(DSI_BRG_INT_CLR_REG) = 0xffffffffu;
    return true;
}

/* ---- 9. drawing for the tests: gray levels, into whichever format runs ---- */

static inline void px(unsigned x, unsigned y, uint8_t g) {
    if (g_fb_fmt == FB_RGB565) {
        uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        ((uint16_t *)g_fb)[y * DSI_LCD_H_RES + x] = v;
    } else {
        g_fb[y * DSI_LCD_H_RES + x] = g;
    }
}

static void fill(uint8_t g) {
    if (g_fb_fmt == FB_RGB565) {
        for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
            for (unsigned x = 0; x < DSI_LCD_H_RES; x++) px(x, y, g);
    } else {
        memset(g_fb, g, g_fb_bytes);
    }
}

static void rect(unsigned x0, unsigned y0, unsigned w, unsigned h, uint8_t g) {
    for (unsigned y = y0; y < y0 + h && y < DSI_LCD_V_RES; y++)
        for (unsigned x = x0; x < x0 + w && x < DSI_LCD_H_RES; x++) px(x, y, g);
}

/* Glyph rows are LSB-first: bit 0 is the leftmost pixel, the order the
 * RP2350's PIO scans canvas1_t bytes in (fbtext copies glyph bytes straight
 * into that buffer). 47.5's first text page read them MSB-first and drew
 * every letter mirrored. */
static void text(unsigned col, unsigned row, const char *s, uint8_t fg, uint8_t bg) {
    for (; *s; s++, col++) {
        unsigned c = (uint8_t)*s;
        if (c < FONT8X16_FIRST || c > FONT8X16_LAST) c = FONT8X16_REPLACEMENT;
        const uint8_t *gl = font8x16_glyphs[c - FONT8X16_FIRST];
        for (unsigned r = 0; r < FONT8X16_H; r++)
            for (unsigned b = 0; b < 8u; b++)
                px(col * 8u + b, row * FONT8X16_H + r, (gl[r] & (1u << b)) ? fg : bg);
    }
}

/* The patterns, as functions of (x, y) where that is cheap, so `lcd verify`
 * can recompute what should be there. */
static uint8_t pat_checker(unsigned x, unsigned y) { return (((x >> 3) ^ (y >> 3)) & 1u) ? 0xffu : 0x00u; }
static uint8_t pat_grid(unsigned x, unsigned y) {
    if (x == 0 || y == 0 || x == DSI_LCD_H_RES - 1u || y == DSI_LCD_V_RES - 1u) return 0xffu;
    if ((x % 32u) == 0 || (y % 32u) == 0) return 0xffu;
    if (x == y) return 0xffu;                                 /* a 45-degree line: 1-px steps */
    return 0x00u;
}

typedef uint8_t (*pat_fn)(unsigned x, unsigned y);
static pat_fn g_last_pat;

static void draw(pat_fn f) {
    for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
        for (unsigned x = 0; x < DSI_LCD_H_RES; x++) px(x, y, f(x, y));
    g_last_pat = f;
}

static void draw_text_page(void) {
    fill(0x00);
    static const char *const lines[] = {
        "LugalOS 47.5 -- frame buffer in PSRAM, DW-GDMA into the DSI bridge, EK79007 1024x600",
        "The quick brown fox jumps over the lazy dog.  0123456789  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
        "126 x 37 cells of 8x16 Spleen -- the RP2350-LCD-7's font, one byte per pixel here.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++) text(1, 1 + i, lines[i], 0xffu, 0x00u);
    for (unsigned r = 5; r < DSI_LCD_V_RES / FONT8X16_H; r++) {
        char ln[DSI_LCD_H_RES / 8u + 1u];
        for (unsigned c = 0; c < DSI_LCD_H_RES / 8u; c++) ln[c] = (char)(0x21u + (r * 7u + c) % 94u);
        ln[DSI_LCD_H_RES / 8u] = '\0';
        text(0, r, ln, (r & 1u) ? 0xffu : 0xc0u, 0x00u);
    }
    rect(0, 4u * FONT8X16_H + 6u, DSI_LCD_H_RES, 4u, 0x80u);   /* a mid-gray rule */
    g_last_pat = NULL;
}

/* ---- the whole bring-up --------------------------------------------------- */

bool dsi_lcd_init(void) {
    if (g_tried) return g_up;
    g_tried = true;
    uint64_t t0 = time_get_us();

    backlight(false);                 /* dark until there is something to show */
    ldo3_on();
    if (!dsi_bus_init()) goto fail;
    dpi_config();
    panel_reset();
    if (!panel_init()) {
        /* Report and carry on: a NACKed vendor register is worth seeing on
         * the glass, and `lcd` says which command it was. */
        g_reason = "panel init commands reported errors (see the log)";
    }
    /* One read while still in command mode, as IDF's reads are: the power
     * mode (0x0A) should now say sleep out. */
    {
        uint8_t pm = 0;
        if (dcs_read(0x0A, &pm, 1) == 1) g_pwr_mode = pm;
    }
    /* dpi_panel_init() without the DMA: video mode on. */
    FSET(DSI_HOST_MODE_CFG_REG, DSI_HOST_CMD_VIDEO_MODE, 0);
    set_pattern(PAT_BARS);
    backlight(true);

    g_up = true;
    if (!g_cmd_errs) g_reason = "up";
    g_init_us = (uint32_t)(time_get_us() - t0);
    printk("[LCD] EK79007 %ux%u, DSI %u lanes x %u Mbps (PLL N %u M %u, hsfreq 0x%02x), "
           "DPI 240/%lu MHz, LDO3 dref %u mul %u, %lu us\n",
           DSI_LCD_H_RES, DSI_LCD_V_RES, LANES, LANE_MBPS, g_pll_n, g_pll_m, g_hsfreq,
           (unsigned long)g_dpi_div, g_ldo_dref, g_ldo_mul, (unsigned long)g_init_us);
    return true;
fail:
    printk("[LCD] bring-up failed: %s (PHY status 0x%08lx)\n", g_reason,
           (unsigned long)REG(DSI_HOST_PHY_STATUS_REG));
    return false;
}

bool dsi_lcd_is_up(void) { return g_up; }

/* ---- the shell command ------------------------------------------------------ */

static const char *skip_ws(const char *s) { while (*s == ' ') s++; return s; }

static int parse_hex(const char **sp) {
    const char *s = skip_ws(*sp);
    int v = 0, digits = 0;
    for (;; s++, digits++) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
        if (digits > 1) return -1;
    }
    *sp = s;
    return digits ? v : -1;
}

static void report(void) {
    cprintf("lcd: %s -- EK79007 %ux%u, %u lanes x %u Mbps\n", g_up ? "up" : "down", DSI_LCD_H_RES,
            DSI_LCD_V_RES, LANES, LANE_MBPS);
    if (!g_tried) { cprintf("     not initialised (`lcd pattern bars` brings it up)\n"); return; }
    cprintf("     %s; pattern: %s; backlight %s; init %lu us, %lu command errors\n", g_reason,
            g_pattern, g_bl_on ? "on" : "off", (unsigned long)g_init_us, (unsigned long)g_cmd_errs);
    if (g_pwr_mode >= 0) cprintf("     power mode (DCS 0x0A, at init) 0x%02x\n", (unsigned)g_pwr_mode);
    else cprintf("     power mode (DCS 0x0A, at init): no answer\n");
    cprintf("     LDO3 %u mV: dref %u mul %u (K %d Vos %d C %d)\n", LDO3_MV, g_ldo_dref, g_ldo_mul,
            g_ldo_k, g_ldo_vos, g_ldo_c);
    cprintf("     PHY PLL: XTAL %u MHz x %u / %u, hsfreqrange 0x%02x; status 0x%08lx\n", XTAL_MHZ,
            g_pll_m, g_pll_n, g_hsfreq, (unsigned long)REG(DSI_HOST_PHY_STATUS_REG));
    cprintf("     DPI 240/%lu MHz; host hsa %lu hbp %lu hline %lu; bridge hfp %lu\n",
            (unsigned long)g_dpi_div, (unsigned long)g_host_hsa, (unsigned long)g_host_hbp,
            (unsigned long)g_host_hline, (unsigned long)g_brg_hfp);
    cprintf("     host version 0x%08lx, mode %s, vid_mode_cfg 0x%08lx; bridge 0x%08lx, int_raw 0x%lx\n",
            (unsigned long)REG(DSI_HOST_VERSION_REG),
            FGET(DSI_HOST_MODE_CFG_REG, DSI_HOST_CMD_VIDEO_MODE) ? "command" : "video",
            (unsigned long)REG(DSI_HOST_VID_MODE_CFG_REG), (unsigned long)REG(DSI_BRG_VER_DATE_REG),
            (unsigned long)REG(DSI_BRG_INT_RAW_REG));
    if (g_fb)
        cprintf("     frame: %s %lu KB at 0x%08lx; %lu frames, %lu DMA errors (last 0x%lx); "
                "bridge underrun %s\n",
                g_fb_fmt == FB_RGB565 ? "RGB565" : "GRAY8", (unsigned long)(g_fb_bytes / 1024u),
                (unsigned long)(uintptr_t)g_fb, (unsigned long)g_frames, (unsigned long)g_dma_errs,
                (unsigned long)g_dma_last_err,
                FGET(DSI_BRG_INT_RAW_REG, DSI_BRG_UNDERRUN_INT_RAW) ? "SEEN" : "none");
}

static void cmd_fps(void) {
    uint32_t f0 = g_frames;
    uint64_t t0 = time_get_us();
    task_sleep_ms(1000);
    uint32_t f1 = g_frames;
    uint64_t dt = time_get_us() - t0;
    uint32_t mhz100 = (uint32_t)((uint64_t)(f1 - f0) * 100000000ull / (dt ? dt : 1u));
    cprintf("lcd: %lu frames in %lu ms = %lu.%02lu Hz; DMA errors %lu; bridge underrun %s\n",
            (unsigned long)(f1 - f0), (unsigned long)(dt / 1000u), (unsigned long)(mhz100 / 100u),
            (unsigned long)(mhz100 % 100u), (unsigned long)g_dma_errs,
            FGET(DSI_BRG_INT_RAW_REG, DSI_BRG_UNDERRUN_INT_RAW) ? "SEEN" : "none");
}

/* CPU fill rate: a full-frame memset plus write-back, and a full redraw of
 * the grid through px(). */
static void cmd_bench(void) {
    uint64_t t0 = time_get_us();
    fill(0x00);
    uint64_t t1 = time_get_us();
    dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
    uint64_t t2 = time_get_us();
    draw(pat_grid);
    dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
    uint64_t t3 = time_get_us();
    uint32_t kb = g_fb_bytes / 1024u;
    cprintf("lcd: fill %lu KB %lu us (%lu MB/s), write-back %lu us, per-pixel grid redraw + write-back %lu us\n",
            (unsigned long)kb, (unsigned long)(t1 - t0),
            (unsigned long)((uint64_t)g_fb_bytes / ((t1 - t0) ? (t1 - t0) : 1u)),
            (unsigned long)(t2 - t1), (unsigned long)(t3 - t2));
}

/* "Screenshot-style readback": write back, drop every cached copy, read the
 * frame from the chip and compare with what the pattern says should be
 * there. */
static void cmd_verify(void) {
    if (!g_last_pat) { cprintf("lcd verify: draw `lcd test checker` or `grid` first\n"); return; }
    dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
    esp32p4_extmem_invalidate((uintptr_t)g_fb, g_fb_bytes);
    uint32_t bad = 0;
    for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
        for (unsigned x = 0; x < DSI_LCD_H_RES; x++) {
            uint8_t g = g_last_pat(x, y);
            bool ok;
            if (g_fb_fmt == FB_RGB565) {
                uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                ok = ((uint16_t *)g_fb)[y * DSI_LCD_H_RES + x] == v;
            } else {
                ok = g_fb[y * DSI_LCD_H_RES + x] == g;
            }
            if (!ok && bad++ < 4) cprintf("  (%u,%u) differs\n", x, y);
        }
    cprintf("lcd verify: %s, %lu of %u pixels differ\n", bad ? "FAIL" : "PASS", (unsigned long)bad,
            DSI_LCD_H_RES * DSI_LCD_V_RES);
}

void dsi_lcd_command(const char *args) {
    args = skip_ws(args);
    if (*args == '\0') { report(); return; }

    if (strncmp(args, "pattern", 7) == 0) {
        const char *p = skip_ws(args + 7);
        pattern_t pat;
        if (*p == '\0' || strcmp(p, "bars") == 0) pat = PAT_BARS;
        else if (strcmp(p, "hbars") == 0) pat = PAT_HBARS;
        else if (strcmp(p, "ber") == 0) pat = PAT_BER;
        else if (strcmp(p, "off") == 0) pat = PAT_NONE;
        else { cprintf("usage: lcd pattern [bars|hbars|ber|off]\n"); return; }
        if (!dsi_lcd_init()) { cprintf("lcd: bring-up failed: %s\n", g_reason); return; }
        set_pattern(pat);
        cprintf("lcd: %s\n", g_pattern);
        return;
    }
    if (strncmp(args, "bl", 2) == 0) {
        const char *p = skip_ws(args + 2);
        if (strcmp(p, "on") == 0) backlight(true);
        else if (strcmp(p, "off") == 0) backlight(false);
        else { cprintf("usage: lcd bl on|off\n"); return; }
        cprintf("lcd: backlight %s\n", g_bl_on ? "on" : "off");
        return;
    }
    if (strncmp(args, "fb", 2) == 0) {
        const char *p = skip_ws(args + 2);
        fb_fmt_t f = FB_GRAY8;
        if (strcmp(p, "rgb565") == 0) f = FB_RGB565;
        else if (*p && strcmp(p, "gray") != 0) { cprintf("usage: lcd fb [gray|rgb565]\n"); return; }
        if (!fb_start(f)) { cprintf("lcd: frame buffer not started\n"); return; }
        draw(pat_grid);
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        cprintf("lcd: frame buffer %s, %lu KB at 0x%08lx, scanning out (grid drawn)\n",
                f == FB_RGB565 ? "RGB565" : "GRAY8", (unsigned long)(g_fb_bytes / 1024u),
                (unsigned long)(uintptr_t)g_fb);
        return;
    }
    if (!g_up) { cprintf("lcd: not up -- %s\n", g_reason); return; }
    if (strcmp(args, "fps") == 0 || strncmp(args, "test", 4) == 0 || strcmp(args, "bench") == 0 ||
        strcmp(args, "verify") == 0 || strncmp(args, "wbtest", 6) == 0) {
        if (!g_fb) { cprintf("lcd: no frame buffer -- `lcd fb [gray|rgb565]` first\n"); return; }
    }
    if (strcmp(args, "fps") == 0) { cmd_fps(); return; }
    if (strcmp(args, "bench") == 0) { cmd_bench(); return; }
    if (strcmp(args, "verify") == 0) { cmd_verify(); return; }
    if (strncmp(args, "test", 4) == 0) {
        const char *p = skip_ws(args + 4);
        if (strcmp(p, "checker") == 0) draw(pat_checker);
        else if (strcmp(p, "grid") == 0) draw(pat_grid);
        else if (strcmp(p, "text") == 0) draw_text_page();
        else if (strcmp(p, "white") == 0) { fill(0xff); g_last_pat = NULL; }
        else if (strcmp(p, "black") == 0) { fill(0x00); g_last_pat = NULL; }
        else if (strcmp(p, "ramp") == 0) {
            for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
                for (unsigned x = 0; x < DSI_LCD_H_RES; x++) px(x, y, (uint8_t)(x * 256u / DSI_LCD_H_RES));
            g_last_pat = NULL;
        } else { cprintf("usage: lcd test checker|grid|text|ramp|white|black\n"); return; }
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        cprintf("lcd: drawn and written back\n");
        return;
    }
    if (strncmp(args, "wbtest", 6) == 0) {
        /* §4's coherence test, both ways: white, written back; then a black
         * box drawn *without* write-back -- the glass should not show it (or
         * only the lines L1/L2 happened to evict); then the write-back, and
         * the box appears. */
        const char *q = skip_ws(args + 6);
        unsigned secs = 0;
        while (*q >= '0' && *q <= '9') secs = secs * 10u + (unsigned)(*q++ - '0');
        if (!secs) secs = 10;
        fill(0xff);
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        task_sleep_ms(5000);
        rect(312, 150, 400, 300, 0x00);
        cprintf("lcd wbtest: black 400x300 box drawn, NOT written back -- look now (%u s)\n", secs);
        task_sleep_ms(secs * 1000u);
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        cprintf("lcd wbtest: written back -- the box is complete now\n");
        g_last_pat = NULL;
        return;
    }

    if (strncmp(args, "cmd", 3) == 0) {
        /* `lcd cmd CC [PP..]`: one DCS write, sent in LP mode between frames. */
        const char *p = args + 3;
        int c = parse_hex(&p);
        uint8_t prm[16];
        unsigned n = 0;
        int v;
        while (n < sizeof prm && (v = parse_hex(&p)) >= 0) prm[n++] = (uint8_t)v;
        if (c < 0 || *skip_ws(p)) { cprintf("usage: lcd cmd CC [PP ...] (hex)\n"); return; }
        bool ok = dcs_write((uint8_t)c, prm, n);
        cprintf("lcd: DCS 0x%02x (%u params) %s\n", (unsigned)c, n, ok ? "sent" : "FAILED");
        return;
    }
    if (strncmp(args, "id", 2) == 0 || strncmp(args, "rd", 2) == 0) {
        /* `lcd id` = DCS 0x04 (RDDID); `lcd rd CC [N]` any DCS read. */
        const char *p = args + 2;
        int c = 0x04, n = 3;
        if (args[0] == 'r') {
            c = parse_hex(&p);
            int nn = parse_hex(&p);
            if (nn > 0) n = nn;
            if (c < 0 || n > 16) { cprintf("usage: lcd rd CC [N] (hex)\n"); return; }
        }
        uint8_t buf[16];
        int got = dcs_read((uint8_t)c, buf, (unsigned)n);
        if (got < 0) { cprintf("lcd: DCS read 0x%02x timed out\n", (unsigned)c); return; }
        cprintf("lcd: DCS read 0x%02x ->", (unsigned)c);
        for (int i = 0; i < got; i++) cprintf(" %02x", buf[i]);
        cprintf("%s\n", got ? "" : " (nothing)");
        return;
    }
    cprintf("usage: lcd [pattern bars|hbars|ber|off | fb [gray|rgb565] | test checker|grid|text|ramp|white|black |\n"
            "            fps | bench | verify | wbtest [s] | bl on|off | cmd CC [PP..] | id | rd CC [N]]\n");
}

#endif
