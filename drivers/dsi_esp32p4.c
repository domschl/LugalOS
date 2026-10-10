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
#include "kernel/console.h"
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

    /* The bridge: a whole RGB888 frame per DMA block (47.5 supplies the DMA),
     * RGB888 in and out, the DMA as flow controller, IDF's burst and
     * threshold. The DPI output stays off: the pattern generator needs none. */
    uint32_t bits = DSI_LCD_H_RES * DSI_LCD_V_RES * 24u;
    FSET(DSI_BRG_RAW_NUM_CFG_REG, DSI_BRG_RAW_NUM_TOTAL, (bits + 63u) / 64u);
    FSET(DSI_BRG_RAW_NUM_CFG_REG, DSI_BRG_UNALIGN_64BIT_EN, (bits % 64u) ? 1 : 0);
    FSET(DSI_BRG_RAW_NUM_CFG_REG, DSI_BRG_RAW_NUM_TOTAL_SET, 1);
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_FIFO_UNDERRUN_DISCARD_VCNT, DSI_LCD_H_RES);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_RAW_TYPE, 0);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DATA_IN_TYPE, 0);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DPI_TYPE, 0);
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DPI_CONFIG, 0);
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
    if (!g_up) { cprintf("lcd: not up -- %s\n", g_reason); return; }

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
    cprintf("usage: lcd [pattern bars|hbars|ber|off | bl on|off | cmd CC [PP..] | id | rd CC [N]]\n");
}

#endif
