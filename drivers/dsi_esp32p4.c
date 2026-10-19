/* The ESP32-P4's MIPI-DSI host and the LCD-7B's EK79007 panel. 47.4,
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

#include "kernel/sysreg.h"
#include "kernel/orient.h"
#include "lugalos_config.h"
#include "drivers/dsi_esp32p4.h"   /* outside the guard: never an empty unit */

#if defined(CONFIG_BOARD_ESP32P4) && (defined(CONFIG_DSI_LCD_RST_GPIO) || defined(CONFIG_BOARD_TAB5))

#include "drivers/esp32p4_dsi_regs.h"
#include "drivers/esp32p4_ledc_regs.h"
#include "drivers/font8x16.h"
#include "drivers/ribbon.h"
#include "drivers/i2c_bus.h"
#include "drivers/lcdterm.h"
#include "drivers/screen.h"
#include "drivers/reader_view.h"
#include "drivers/font_noto_mono.h"
#include "drivers/vtterm.h"
#include "arch/esp32p4_intr.h"
#include "arch/trap.h"
#include "kernel/console.h"
#include "kernel/devirq.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static inline void field_set(uintptr_t reg, unsigned s, uint32_t v_mask, uint32_t val) {
    if (sysreg_is_shared(reg)) { sysreg_update(reg, v_mask << s, (val & v_mask) << s); return; }
    uint32_t x = REG(reg);
    x &= ~(v_mask << s);
    x |= (val & v_mask) << s;
    REG(reg) = x;
}
#define FSET(reg, f, val) field_set((reg), f##_S, f##_V, (uint32_t)(val))
#define FGET(reg, f)      ((REG(reg) >> f##_S) & f##_V)

/* ---- the panel and the link ---------------------------------------------- */

#if defined(CONFIG_BOARD_TAB5)
#define LANES          2u
#define LANE_MBPS      900u      /* ST7121 2-lane 900 Mbps */
#define DPI_KHZ        70000u    /* 70 MHz */
#define HSW 2u
#define HBP 40u
#define HFP 40u
#define VSW 20u
#define VBP 24u
#define VFP 200u
#else
#define LANES          2u
#define LANE_MBPS      1000u      /* EK79007_PANEL_BUS_DSI_2CH_CONFIG, Waveshare */
#define DPI_KHZ        52000u     /* what the panel wants */
#define HSW 10u
#define HBP 160u
#define HFP 160u
#define VSW 1u
#define VBP 23u
#define VFP 12u
#endif

#define XTAL_MHZ       40u        /* the PHY PLL's reference on v3 (DEFAULT, not _LEGACY) */
#define F240M_MHZ      240u       /* MIPI_DSI_DPI_CLK_SRC_DEFAULT = PLL_F240M */

#if defined(CONFIG_ESP32P4_REV) && CONFIG_ESP32P4_REV >= 300
#define PHY_REF_MHZ    XTAL_MHZ  /* 40 MHz on v3 */
#else
#define PHY_REF_MHZ    20u       /* 20 MHz (PLL_F20M) on v1 */
#endif

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
#define GPIO_OUT_W1TS_LO    (GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC_LO    (GPIO_BASE + 0x0C)
#define GPIO_ENABLE_W1TS_LO (GPIO_BASE + 0x24)
#define GPIO_OUT_W1TS_HI    (GPIO_BASE + 0x14)
#define GPIO_OUT_W1TC_HI    (GPIO_BASE + 0x18)
#define GPIO_ENABLE_W1TS_HI (GPIO_BASE + 0x30)

#ifndef CONFIG_DSI_LCD_RST_GPIO
#define RST_GPIO 0
#else
#define RST_GPIO   CONFIG_DSI_LCD_RST_GPIO
#endif
#define BL_GPIO    CONFIG_DSI_LCD_BL_GPIO

/* eFuse BLK1 words (drivers/psram_esp32p4.c reads the same two). */
#define EFUSE_RD_MAC_SYS_2   0x5012d04cu
#define EFUSE_RD_MAC_SYS_3   0x5012d050u

/* ---- state -------------------------------------------------------------- */

/* guarded by: console_lock for the bring-up (dsi_lcd_init() runs its body
 * once, from fb_start() or `lcd pattern`, both under it; the boot's first call
 * comes before any other task draws). accepted: the backlight figures, set by
 * the boot and by `lcd bl|backlight` from a shell, single stores. */
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
static unsigned    g_bl_level = 100;      /* percent; what `on` returns to */
static bool        g_bl_pwm;              /* the LEDC drives the pin (47.11) */
static int         g_pwr_mode = -1;  /* DCS 0x0A after sleep out, read in command mode */
static const char *g_pattern = "none";

static uint32_t div_round(uint64_t a, uint64_t b) { return (uint32_t)((a + b / 2u) / b); }

static void __attribute__((unused)) gpio_out(unsigned gpio, bool high) {
    uint32_t pad = REG(IOMUX_PAD(gpio));
    pad &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_PU | IOMUX_FUN_PD);
    pad |= (IOMUX_FUNC_GPIO << IOMUX_MCU_SEL_S);
    REG(IOMUX_PAD(gpio)) = pad;
    REG(GPIO_OUT_SEL(gpio)) = GPIO_OUT_SEL_GPIO;
    if (gpio >= 32u) {
        if (high) REG(GPIO_OUT_W1TS_HI) = 1u << (gpio - 32u);
        else      REG(GPIO_OUT_W1TC_HI) = 1u << (gpio - 32u);
        REG(GPIO_ENABLE_W1TS_HI) = 1u << (gpio - 32u);
    } else {
        if (high) REG(GPIO_OUT_W1TS_LO) = 1u << gpio;
        else      REG(GPIO_OUT_W1TC_LO) = 1u << gpio;
        REG(GPIO_ENABLE_W1TS_LO) = 1u << gpio;
    }
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
 * MHz, M even; the first (N, M) that hits the rate exactly wins. */
static bool phy_pll_config(void) {
    unsigned min_n = PHY_REF_MHZ / 40u ? PHY_REF_MHZ / 40u : 1u, max_n = PHY_REF_MHZ / 5u;
    uint32_t best = 0xFFFFFFFFu;
    g_pll_m = 0; g_pll_n = 1;
    for (unsigned n = min_n; n <= max_n; n++) {
        uint32_t m = LANE_MBPS * n / PHY_REF_MHZ;
        if (m & 1u) continue;
        uint32_t got_x1000 = PHY_REF_MHZ * 1000u * m / n;
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

    /* The PHY's configuration clock: PLL_F20M (gate opened), source 0. */
    FSET(HP_SYS_CLKRST_REF_CLK_CTRL2_REG, HP_SYS_CLKRST_REG_REF_20M_CLK_EN, 1);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL02_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CLK_SRC_SEL, 0);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_CFG_CLK_EN, 1);
#if defined(CONFIG_ESP32P4_REV) && CONFIG_ESP32P4_REV >= 300
    /* Its PLL reference: XTAL (source 0), undivided (v3). */
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_SRC_SEL, 0);
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_DIV_NUM, 0);
#endif
    FSET(HP_SYS_CLKRST_PERI_CLK_CTRL03_REG, HP_SYS_CLKRST_REG_MIPI_DSI_DPHY_PLL_REFCLK_EN, 1);

    /* mipi_dsi_hal_init() */
    FSET(DSI_HOST_PHY_IF_CFG_REG, DSI_HOST_N_LANES, LANES - 1u);
    FSET(DSI_HOST_PWR_UP_REG, DSI_HOST_SHUTDOWNZ, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_SHUTDOWNZ, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_RSTZ, 0);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_RSTZ, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_ENABLECLK, 1);
    FSET(DSI_HOST_PHY_RSTZ_REG, DSI_HOST_PHY_FORCEPLL, 1);
#if defined(CONFIG_ESP32P4_REV) && CONFIG_ESP32P4_REV >= 300
    FSET(DSI_BRG_EN_REG, DSI_BRG_DSI_BRIG_RST, 1);
    FSET(DSI_BRG_EN_REG, DSI_BRG_DSI_BRIG_RST, 0);
#endif

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
#if defined(CONFIG_BOARD_TAB5)
    /* On Tab5 (ESP32-P4 v1.3), bridge input & output format is raw_type (2 = RGB565) */
#else
    FSET(DSI_BRG_PIXEL_TYPE_REG, DSI_BRG_DPI_TYPE, 0);
#endif
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
#if defined(CONFIG_BOARD_TAB5)
    FSET(DSI_HOST_DPI_COLOR_CODING_REG, DSI_HOST_DPI_COLOR_CODING, 0); /* 16-bit RGB565 config 1 */
#else
    FSET(DSI_HOST_DPI_COLOR_CODING_REG, DSI_HOST_DPI_COLOR_CODING, COLOR_CODE_24BIT);
#endif
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
#if defined(CONFIG_BOARD_TAB5)
    bridge_format(16u, 2u);
#else
    bridge_format(24u, 0u);
#endif
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
    uint32_t ack_err = e0 & 0x0000ffffu;
    uint32_t cmd_err = e1 & 0x00000180u;
    if (!drained || ack_err || cmd_err) {
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
#if !defined(CONFIG_BOARD_TAB5)
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
#else
/* M5Stack Tab5 ST7121 initialization sequence (from M5GFX Panel_ST7121.hpp). */
static const uint8_t k_st7121_init[] = {
    4,  0x60,  0x71, 0x21, 0xA2,
    4,  0x60,  0x71, 0x21, 0xA3,
    4,  0x60,  0x71, 0x21, 0xA4,
    2,  0x78,  0x21,
    2,  0x79,  0xEF,
    2,  0xA4,  0x31,
    7,  0xB7,  0x00, 0x00, 0x5F, 0x5F, 0x44,  0x1A,
    8,  0xB0,  0x22, 0x6B, 0x11, 0x89, 0x25,  0x43, 0x43,
    3,  0xBF,  0xA7, 0xA7,
    3,  0xA5,  0xF0, 0x03,
    7,  0xD7,  0x10, 0x2C, 0x14, 0x2A, 0x80,  0x80,
    8,  0x90,  0x71, 0x23, 0x5A, 0x20, 0x24,  0x11, 0x21,

    40, 0xA3,  0x80, 0x01, 0x8C, 0xFF, 0x45,  0x00, 0x00, 0x00, 0x00, 0x00,
               0x46, 0x00, 0x00, 0x1E, 0x5C,  0x1E, 0x80, 0x10, 0x00, 0x05,
               0x00, 0x00, 0x00, 0x00, 0x00,  0x46, 0x00, 0x00, 0x1E, 0x5C,
               0x1E, 0x80, 0x10, 0xEF, 0x58,  0x00, 0x00, 0x00, 0xFF,

    56, 0xA6,  0x0A, 0x00, 0x24, 0x71, 0x36,  0x00, 0x00, 0x00, 0x68, 0x68,
               0x91, 0xFF, 0x00, 0x24, 0x71,  0x37, 0x00, 0x00, 0x00, 0x68,
               0x68, 0x91, 0xFF, 0x00, 0x24,  0x71, 0x00, 0x00, 0x00, 0x00,
               0x68, 0x68, 0x91, 0xFF, 0x00,  0x2C, 0x71, 0x00, 0x01, 0x00,
               0x00, 0x68, 0x68, 0xFF, 0xFF,  0x00, 0x08, 0x80, 0x08, 0x80,
               0x06, 0x00, 0x00, 0x00, 0x00,

    61, 0xA7,  0x1A, 0x1A, 0xC0, 0x64, 0x40,  0x04, 0x15, 0x40, 0x00, 0x40,
               0x00, 0x68, 0x68, 0x91, 0xFF,  0x08, 0x80, 0x64, 0x40, 0x26,
               0x37, 0x40, 0x00, 0x00, 0x00,  0x68, 0x68, 0x91, 0xFF, 0x08,
               0x80, 0x64, 0x40, 0x8C, 0x9D,  0x40, 0x00, 0x00, 0x00, 0x68,
               0x68, 0x91, 0xFF, 0x08, 0x80,  0x64, 0x40, 0xAE, 0xBF, 0x00,
               0x00, 0x20, 0x00, 0x68, 0x68,  0x91, 0xFF, 0x08, 0x80, 0x79,

    45, 0xAC,  0x1D, 0x18, 0x19, 0x1D, 0x18,  0x19, 0x04, 0x1C, 0x1D, 0x08,
               0x0A, 0x10, 0x12, 0x0C, 0x0E,  0x14, 0x16, 0x00, 0x1D, 0x1D,
               0x1D, 0x1D, 0x1D, 0x18, 0x19,  0x1D, 0x18, 0x19, 0x06, 0x1C,
               0x1D, 0x09, 0x0B, 0x11, 0x13,  0x0D, 0x0F, 0x15, 0x17, 0x02,
               0x1D, 0x1D, 0x1D, 0x1D,

    26, 0xAD,  0x0C, 0x40, 0x46, 0x00, 0x07,  0x4B, 0x4B, 0xFF, 0xFF, 0xF0,
               0x40, 0x0E, 0x01, 0x07, 0x42,  0x42, 0xFF, 0xFF, 0x01, 0x00,
               0x00, 0xFF, 0xFF, 0xFF, 0xFF,

     8, 0xAE,  0xF0, 0xFF, 0x03, 0xF0, 0xFF,  0x03, 0x00,
    18, 0xB2,  0x15, 0x19, 0x05, 0x23, 0x49,  0x2D, 0x03, 0x2E, 0x5C, 0xD2,
               0xFF, 0x10, 0x60, 0xFD, 0x20,  0xC0, 0x00,
    15, 0xE8,  0x20, 0x60, 0x04, 0x8E, 0x8E,  0x3E, 0x04, 0xDC, 0xDC, 0x3E,
               0x06, 0xFA, 0x26, 0x3E,
     3, 0x75,  0x03, 0x04,

    43, 0xE7,  0x4B, 0x00, 0x00, 0xBE, 0x4B,  0x8C, 0x20, 0x1A, 0xF0, 0x7D,
               0x14, 0x7D, 0x14, 0x7D, 0x14,  0x7D, 0x14, 0xFF, 0x00, 0x32,
               0x30, 0x73, 0x00, 0x00, 0xC8,  0x6A, 0xFF, 0x5A, 0x64, 0x38,
               0x88, 0x15, 0xB1, 0x01, 0x01,  0x64, 0x01, 0x01, 0x7C, 0xFF,
               0x1A, 0x51,

     3, 0xE1,  0x0C, 0x0C,
     4, 0xEA,  0x15, 0x00, 0x01,
    38, 0xC8,  0x00, 0x00, 0x04, 0x08, 0x10,  0x00, 0x1F, 0x01, 0x39, 0x3E,
               0x00, 0x78, 0x06, 0xE2, 0x02,  0x11, 0x33, 0x01, 0x7A, 0x0D,
               0x21, 0xC4, 0x0B, 0x19, 0x08,  0x32, 0xA0, 0x08, 0x1A, 0x0A,
               0xF3, 0x7F, 0x0E, 0xC5, 0xE8,  0x03, 0xFF,
    38, 0xC9,  0x00, 0x00, 0x04, 0x08, 0x10,  0x00, 0x1F, 0x01, 0x39, 0x3E,
               0x00, 0x78, 0x06, 0xE2, 0x02,  0x11, 0x33, 0x01, 0x7A, 0x0D,
               0x21, 0xC4, 0x0B, 0x19, 0x08,  0x32, 0xA0, 0x08, 0x1A, 0x0A,
               0xF3, 0x7F, 0x0E, 0xC5, 0xE8,  0x03, 0xFF,
     4, 0x60,  0x71, 0x21, 0x00,
     0
};
#endif

/* 47.7: the bring-up runs at boot, before the scheduler, so it waits by the
 * clock; ~250 ms once. */
static void wait_ms(uint32_t ms) { time_delay_us((uint64_t)ms * 1000u); }

static void panel_reset(void) {
#if defined(CONFIG_BOARD_TAB5)
    i2c_tab5_panel_reset();
#else
    gpio_out(RST_GPIO, false);
    wait_ms(10);
    gpio_out(RST_GPIO, true);
    wait_ms(120);
#endif
}

static bool panel_init(void) {
    bool ok = true;
#if defined(CONFIG_BOARD_TAB5)
    const uint8_t *p = k_st7121_init;
    while (*p) {
        uint8_t len = *p++;
        uint8_t cmd = *p++;
        uint8_t n = len - 1u;
        ok &= dcs_write(cmd, p, n);
        p += n;
    }
    ok &= dcs_write(0x11, NULL, 0);   /* SLPOUT */
    wait_ms(120);
    ok &= dcs_write(0x29, NULL, 0);   /* DISPON */
    wait_ms(800);
    uint8_t te = 0x00;
    ok &= dcs_write(0x35, &te, 1);    /* TE off */
    uint8_t madctl = 0x00;
    ok &= dcs_write(0x36, &madctl, 1); /* MADCTL RGB order */
    uint8_t colmod = 0x55;
    ok &= dcs_write(0x3A, &colmod, 1); /* COLMOD 16-bit RGB565 */
    wait_ms(20);
#else
    for (size_t i = 0; i < sizeof k_ek79007_init / sizeof k_ek79007_init[0]; i++) {
        ok &= dcs_write(k_ek79007_init[i].cmd, &k_ek79007_init[i].p, k_ek79007_init[i].n);
        if (k_ek79007_init[i].delay_ms) wait_ms(k_ek79007_init[i].delay_ms);
    }
#endif
    return ok;
}

/* 47.11: the backlight's brightness, PWM from the LEDC.
 *
 * GPIO32 feeds the AP3032 boost converter's feedback, so a *higher* level is
 * a dimmer backlight (the board file; Waveshare drive it "inverted", LEDC
 * 5 kHz 10-bit). The same arrangement as the RP2350-LCD-7's LCD_BL. So the
 * GPIO matrix inverts the LEDC's signal, and the duty is the brightness:
 * duty 0 is a pin held high, dark; full duty a pin held low, full
 * brightness, which is what the plain GPIO did before.
 *
 * The sequence is IDF's (esp_hal_ledc/esp32p4/include/hal/ledc_ll.h,
 * esp_driver_ledc/src/ledc.c): bus clock and reset, the function clock from
 * XTAL, the register clock gate, timer 0 (resolution, divider, update,
 * reset), channel 0 on it. On the P4 even a plain duty goes through the
 * channel's fade table (ledc_set_duty() writes entry 0 as "one step of one
 * cycle" and the entry count 1) -- without it the channel never takes the
 * duty. The table is LEDC_GAMMA_RAM, 0x500D3400, 16 words per channel
 * (ld/esp32p4.peripherals.ld, ledc_struct.h). */
#define LEDC_GAMMA_RAM_CH0     0x500D3400u
#define LEDC_CH0_DUTY_R_REG    0x500D3010u  /* ledc_reg.h: the duty in force */
#define LEDC_TIMER0_VALUE_REG  0x500D30A4u  /* ledc_reg.h: timer 0's counter */
#define LEDC_SIG_OUT0_IDX      126u        /* LEDC_LS_SIG_OUT_PAD_OUT0_IDX */
#define BL_PWM_HZ              5000u
#define BL_DUTY_RES            10u
#define BL_XTAL_HZ             40000000u

static void bl_pwm_init(void) {
    sysreg_set(HP_SYS_CLKRST_SOC_CLK_CTRL3_REG, 1u << HP_SYS_CLKRST_REG_LEDC_APB_CLK_EN_S);
    sysreg_set(HP_SYS_CLKRST_HP_RST_EN1_REG, 1u << HP_SYS_CLKRST_REG_RST_EN_LEDC_S);
    sysreg_clear(HP_SYS_CLKRST_HP_RST_EN1_REG, 1u << HP_SYS_CLKRST_REG_RST_EN_LEDC_S);
    sysreg_update(HP_SYS_CLKRST_PERI_CLK_CTRL22_REG,
                  HP_SYS_CLKRST_REG_LEDC_CLK_SRC_SEL_V << HP_SYS_CLKRST_REG_LEDC_CLK_SRC_SEL_S,   /* 0: XTAL */
                  1u << HP_SYS_CLKRST_REG_LEDC_CLK_EN_S);
    REG(LEDC_CONF_REG) |= 1u << LEDC_CLK_EN_S;

    /* 40 MHz / (5 kHz * 1024) = 7.8125: the divider has 8 fraction bits. */
    uint32_t div = (uint32_t)(((uint64_t)BL_XTAL_HZ << 8) / ((uint64_t)BL_PWM_HZ << BL_DUTY_RES));
    REG(LEDC_TIMER0_CONF_REG) = (BL_DUTY_RES << LEDC_TIMER0_DUTY_RES_S) | (div << LEDC_CLK_DIV_TIMER0_S);
    REG(LEDC_TIMER0_CONF_REG) |= 1u << LEDC_TIMER0_PARA_UP_S;
    REG(LEDC_TIMER0_CONF_REG) |= 1u << LEDC_TIMER0_RST_S;
    REG(LEDC_TIMER0_CONF_REG) &= ~(1u << LEDC_TIMER0_RST_S);

    REG(LEDC_CH0_HPOINT_REG) = 0;
    REG(LEDC_CH0_CONF0_REG) = (0u << LEDC_TIMER_SEL_CH0_S) | (0u << LEDC_IDLE_LV_CH0_S);
    for (unsigned i = 0; i < 16; i++) REG(LEDC_GAMMA_RAM_CH0 + 4u * i) = 0;
    g_bl_pwm = true;
}

/* 0..100; 0 is dark. Routes the pin to the LEDC on first use. */
static void bl_set(unsigned percent) {
    if (percent > 100u) percent = 100u;
    if (!g_bl_pwm) bl_pwm_init();
    uint32_t duty = ((1u << BL_DUTY_RES) * percent) / 100u;
    REG(LEDC_CH0_DUTY_REG) = duty << 4;                     /* 4 fraction bits */
    /* Fade entry 0: increase 1, cycle 1, scale 0, steps 1 -- ledc_set_duty(). */
    REG(LEDC_GAMMA_RAM_CH0) = 1u | (1u << 1) | (0u << 11) | (1u << 21);
    REG(LEDC_CH0_GAMMA_CONF_REG) = 1u << LEDC_CH0_GAMMA_ENTRY_NUM_S;
    REG(LEDC_CH0_CONF0_REG) |= 1u << LEDC_SIG_OUT_EN_CH0_S;
    REG(LEDC_CH0_CONF1_REG) = 1u << LEDC_DUTY_START_CH0_S;
    REG(LEDC_CH0_CONF0_REG) |= 1u << LEDC_PARA_UP_CH0_S;
    /* The pad: GPIO function, its output from the matrix, inverted or normal. */
    uint32_t pad = REG(IOMUX_PAD(BL_GPIO));
    pad &= ~(IOMUX_MCU_SEL_M | IOMUX_FUN_PU | IOMUX_FUN_PD);
    pad |= (IOMUX_FUNC_GPIO << IOMUX_MCU_SEL_S);
    REG(IOMUX_PAD(BL_GPIO)) = pad;
    REG(GPIO_OUT_SEL(BL_GPIO)) = (LEDC_SIG_OUT0_IDX << GPIO_FUNC0_OUT_SEL_S) |
#if !defined(CONFIG_DSI_LCD_BL_ACTIVE_HIGH) || !CONFIG_DSI_LCD_BL_ACTIVE_HIGH
                                 (1u << GPIO_FUNC0_OUT_INV_SEL_S);
#else
                                 0;
#endif
#if BL_GPIO >= 32
    REG(GPIO_ENABLE_W1TS_HI) = 1u << (BL_GPIO - 32u);
#else
    REG(GPIO_ENABLE_W1TS_LO) = 1u << BL_GPIO;
#endif
}

static void backlight(bool on) {
    bl_set(on ? g_bl_level : 0u);
    g_bl_on = on;
}

/* ---- 7. the pattern generator --------------------------------------------- */

typedef enum { PAT_NONE, PAT_BARS, PAT_HBARS, PAT_BER } pattern_t;

static void scan_stop(void);
static void scan_start(void);

/* Double-buffering in PSRAM, against tearing and colour fringing.
 * interrupt-shared: the drawing side (under console_lock) fills the back
 * buffer, flushes it and sets g_flip_pending; gdma_isr() at the frame end
 * swaps g_front_idx, points the item at the new front and clears the flag.
 * The terminal never draws into the back buffer with the flag set:
 * term_convert() takes the flip back first and arms it again when done. The
 * `lcd test|bench` drawers hold the terminal and wait for their own flip; one
 * pending from the terminal can show a test frame part-drawn. g_front_idx and
 * g_double_buffered change elsewhere only under console_lock, with the
 * scan-out stopped (fb_start) or under g_scan_lock around the index and the
 * item (`lcd db off`). Tasks run the handler's work only through gdma_poll().
 * Taking the flip back is under g_scan_lock (see gdma_isr()); arming it is one
 * store, which the next frame end takes or the one after. */
static uint8_t          *g_fb_pool[2];
static volatile uint8_t  g_front_idx;
static volatile bool     g_flip_pending;
static volatile uint32_t g_flips;
static bool              g_double_buffered = true;

/* guarded by: console_lock, as every conversion and frame switch. */
#define DIRTY_WORDS ((DSI_LCD_V_RES + 63u) / 64u)
static uint64_t          g_dirty[2][DIRTY_WORDS];
static uint64_t          g_dirty_trows[2];
static uint16_t          g_dirty_x0[2];
static uint16_t          g_dirty_x1[2];
#define MAX_TERM_CELLS   2048u
static uint16_t          g_t_cell_drawn[2][MAX_TERM_CELLS];
static uint32_t          s_reader_drawn_seq[2][4];

static inline uint8_t *fb_front(void) {
    return g_fb_pool[g_front_idx];
}

static inline uint8_t *fb_back(void) {
    return g_fb_pool[g_double_buffered ? (1u - g_front_idx) : 0u];
}

static void set_pattern(pattern_t p) {
    /* The bridge's DPI stream off whenever the generator is on -- and since
     * 47.7 a frame is always being scanned out, so it stops at a frame
     * boundary (§6.6: a stream cut mid-frame shifts every later frame). */
    if (p != PAT_NONE) {
        scan_stop();
    }
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
        /* Back to the frame, if there is one (47.5); without, what the
         * panel shows is undefined. */
        FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 0);
        g_pattern = "none";
        if (g_fb_pool[0]) scan_start();
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
/* guarded by: console_lock (fb_start()'s callers hold it, or are the boot). */
static fb_fmt_t          g_fb_fmt;
static uint32_t          g_fb_bytes, g_fb_pages, g_fb_bpp;
/* interrupt-shared: the counters are written only by the frame end
 * (gdma_isr() or gdma_poll(), under g_scan_lock); g_stop_at_frame_end is set and cleared by
 * scan_stop() around its wait for the frame end. */
static volatile uint32_t g_frames, g_dma_errs, g_dma_last_err;
static volatile bool     g_stop_at_frame_end;   /* ISR: finish this frame, do not re-arm */
static bool              g_gdma_ready;

static void lli_arm(void) {
    volatile gdma_lli_t *l = LLI_NC;
    l->sar_lo = (uint32_t)(uintptr_t)fb_front();
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

/* interrupt-shared: g_scan_lock makes a frame end and a task's change to the
 * flip one step each: gdma_isr() holds it for the whole frame end, and every
 * task that tests and takes g_flip_pending, or moves g_front_idx with the
 * item, holds it too. Masking interrupts alone excluded the handler only on
 * the task's own hart; with a second hart running tasks (smpstart), the
 * terminal could take a flip back while the handler, past its test, swapped
 * the buffers -- and then draw into the new front. */
static spinlock_t g_scan_lock;

/* The frame end. Under g_scan_lock. */
static void gdma_frame_end_locked(void) {
    uint32_t st = REG(DMAC_CH1_INTSTATUS0_REG);
    REG(DMAC_CH1_INTCLEAR0_REG) = st;
    if (st & GDMA_EV_ERRORS) { g_dma_errs++; g_dma_last_err = st; }
    if (st & GDMA_EV_DMA_TFR_DONE) {
        g_frames++;
        if (g_fb_pool[g_front_idx] && !g_stop_at_frame_end) {
            if (g_flip_pending && g_double_buffered && g_fb_pool[1]) {
                g_front_idx = 1u - g_front_idx;
                LLI_NC->sar_lo = (uint32_t)(uintptr_t)g_fb_pool[g_front_idx];
                g_flip_pending = false;
                g_flips++;
            }
            __asm__ volatile ("fence" ::: "memory");
            gdma_restart();
        }
    }
}

static void gdma_isr(void *ctx) {
    (void)ctx;
    uintptr_t f = spin_lock_irqsave(&g_scan_lock);
    gdma_frame_end_locked();
    spin_unlock_irqrestore(&g_scan_lock, f);
}

/* The handler's work from a task that waits on the frame end, under the same
 * lock, so gdma_isr() cannot run between this status read and its clear and
 * finish the same frame twice (a second restart into a running transfer). */
static void gdma_poll(void) {
    uintptr_t f = spin_lock_irqsave(&g_scan_lock);
    if (REG(DMAC_CH1_INTSTATUS0_REG) & (GDMA_EV_DMA_TFR_DONE | GDMA_EV_ERRORS)) gdma_frame_end_locked();
    spin_unlock_irqrestore(&g_scan_lock, f);
}

/* Waits up to `us` for the frame end that takes a pending flip. */
static bool gdma_wait_flip(uint32_t us) {
    uint64_t t0 = time_get_us();
    while (g_flip_pending && time_get_us() - t0 < us) gdma_poll();
    return !g_flip_pending;
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

/* Write back [y0, y1) of the given frame so the DMA sees it (§4: the CPU writes
 * PSRAM through L1D and L2; the DMA reads the chip). Chunked into 32-row
 * blocks so the extmem writeback does not starve DW-GDMA of PSRAM bandwidth. */
static void fb_flush_buffer(uint8_t *fb, unsigned y0, unsigned y1) {
    if (!fb || y0 >= y1) return;
    if (y1 > DSI_LCD_V_RES) y1 = DSI_LCD_V_RES;
    uint32_t row = DSI_LCD_H_RES * g_fb_bpp / 8u;
    const unsigned CHUNK = 32;
    for (unsigned y = y0; y < y1; y += CHUNK) {
        unsigned end_y = y + CHUNK;
        if (end_y > y1) end_y = y1;
        esp32p4_extmem_writeback((uintptr_t)fb + y * row, (end_y - y) * row);
    }
}

void dsi_lcd_fb_flush(unsigned y0, unsigned y1) {
    fb_flush_buffer(fb_back(), y0, y1);
}

uint8_t *dsi_lcd_fb(void) { return fb_back(); }
uint8_t *dsi_lcd_fb_front(void) { return fb_front(); }
uint8_t *dsi_lcd_fb_back(void) { return fb_back(); }

static bool g_scanning;   /* guarded by: console_lock (scan_start/scan_stop's callers) */

/* Flip front/back buffers at hardware VSYNC. If wait_vsync is true,
 * blocks until the ISR switches the scanout address during vertical blanking. */
void dsi_lcd_flip(bool wait_vsync) {
    if (!g_double_buffered || !g_fb_pool[1] || !g_scanning) return;
    g_flip_pending = true;
    if (wait_vsync) (void)gdma_wait_flip(50000u);
}

bool dsi_lcd_is_double_buffered(void) {
    return g_double_buffered && (g_fb_pool[1] != NULL);
}

/* dpi_panel_init() for one frame buffer: frame in PSRAM, bridge format, the
 * item, channel on, bridge DPI output on, pattern generator off. */
/* The scan-out, stopped at a frame boundary and started at the frame's
 * first byte.
 *
 * Stop: the bridge has no notion of
     * where a frame starts other than "the next byte after the last one",
     * so a DMA cut off mid-frame leaves the rest in its FIFO and every
     * later frame comes out shifted by that remainder (seen: the rightmost
     * 2-3 pixels wrapped to the left edge after a GRAY8 -> RGB565 -> GRAY8
     * switch). So: let the running frame finish, let the bridge drain its
     * FIFO onto the glass, and only then stop the DPI stream. */
static void scan_stop(void) {
    if (g_scanning) {
        g_stop_at_frame_end = true;
        uint32_t f0 = g_frames;
        uint64_t t0 = time_get_us();
        while (g_frames == f0 && time_get_us() - t0 < 50000u) { }
        t0 = time_get_us();
        while (FGET(DSI_BRG_FIFO_FLOW_STATUS_REG, DSI_BRG_RAW_BUF_DEPTH) && time_get_us() - t0 < 20000u) { }
        gdma_stop();
        g_stop_at_frame_end = false;
        g_flip_pending = false;
        g_scanning = false;
    }
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_DPI_EN, 0);
    FSET(DSI_BRG_DPI_CONFIG_UPDATE_REG, DSI_BRG_DPI_CONFIG_UPDATE, 1);
}

static void scan_start(void) {
    if (g_scanning || !g_fb_pool[0]) return;
    lli_arm();
    REG(DMAC_CH1_INTCLEAR0_REG) = 0xffffffffu;
    gdma_restart();
    FSET(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VPG_EN, 0);
    g_pattern = "none (frame buffer)";
    FSET(DSI_BRG_DPI_MISC_CONFIG_REG, DSI_BRG_DPI_EN, 1);
    FSET(DSI_BRG_DPI_CONFIG_UPDATE_REG, DSI_BRG_DPI_CONFIG_UPDATE, 1);
    REG(DSI_BRG_INT_CLR_REG) = 0xffffffffu;
    g_scanning = true;
}

static bool fb_start(fb_fmt_t fmt) {
#if defined(CONFIG_BOARD_TAB5)
    fmt = FB_RGB565; /* Tab5 v1.3 bridge uses RGB565 natively */
#endif
    if (!dsi_lcd_init()) return false;
    gdma_init();
    if (!g_gdma_ready) { cprintf("lcd: the DW-GDMA interrupt could not be routed\n"); return false; }

    uint8_t *old0 = g_fb_pool[0];
    uint8_t *old1 = g_fb_pool[1];
    uint32_t old_pages = g_fb_pages;
    scan_stop();
    g_fb_pool[0] = NULL;
    g_fb_pool[1] = NULL;
    if (old0) palloc_free(old0, old_pages);
    if (old1) palloc_free(old1, old_pages);

    uint32_t bpp = (fmt == FB_RGB565) ? 16u : 8u;
    uint32_t bytes = DSI_LCD_H_RES * DSI_LCD_V_RES * bpp / 8u;
    uint32_t pages = (bytes + 4095u) / 4096u;
    uint8_t *fb0 = palloc_pages_bulk(pages);
    if (!fb0 || !palloc_is_bulk(fb0)) {
        if (fb0) palloc_free(fb0, pages);
        cprintf("lcd: no %lu KB run of PSRAM for the frame\n", (unsigned long)(bytes / 1024u));
        g_fb_fmt = FB_NONE;
        return false;
    }
    memset(fb0, 0, bytes);
    esp32p4_extmem_writeback((uintptr_t)fb0, bytes);
    g_fb_pool[0] = fb0;

    if (g_double_buffered) {
        uint8_t *fb1 = palloc_pages_bulk(pages);
        if (fb1 && palloc_is_bulk(fb1)) {
            memset(fb1, 0, bytes);
            esp32p4_extmem_writeback((uintptr_t)fb1, bytes);
            g_fb_pool[1] = fb1;
        } else {
            if (fb1) palloc_free(fb1, pages);
            cprintf("lcd: no PSRAM for second buffer; single-buffered\n");
            g_fb_pool[1] = NULL;
            g_double_buffered = false;
        }
    }

    g_front_idx = 0;
    g_flip_pending = false;
    memset(g_dirty, 0xff, sizeof g_dirty);
    g_dirty_trows[0] = ~0ull;
    g_dirty_trows[1] = ~0ull;
    g_dirty_x0[0] = 0; g_dirty_x1[0] = 2047u;
    g_dirty_x0[1] = 0; g_dirty_x1[1] = 2047u;
    memset(g_t_cell_drawn, 0xff, sizeof g_t_cell_drawn);
    memset(s_reader_drawn_seq, 0xff, sizeof s_reader_drawn_seq);

    g_fb_bpp = bpp; g_fb_bytes = bytes; g_fb_pages = pages; g_fb_fmt = fmt;
    bridge_format(bpp, fmt == FB_RGB565 ? 2u : 12u);
    scan_start();
    return true;
}

/* ---- 9. drawing for the tests: gray levels, into whichever format runs ---- */

static inline void px(unsigned x, unsigned y, uint8_t g) {
    uint8_t *fb = fb_back();
    if (!fb) return;
    if (g_fb_fmt == FB_RGB565) {
        uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
        ((uint16_t *)fb)[y * DSI_LCD_H_RES + x] = v;
    } else {
        fb[y * DSI_LCD_H_RES + x] = g;
    }
}

static void fill(uint8_t g) {
    uint8_t *fb = fb_back();
    if (!fb) return;
    if (g_fb_fmt == FB_RGB565) {
        for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
            for (unsigned x = 0; x < DSI_LCD_H_RES; x++) px(x, y, g);
    } else {
        memset(fb, g, g_fb_bytes);
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
/* accepted: a diagnostic command's state; two terminals running it at once mix their
 * output. */
static pat_fn g_last_pat;

static void draw(pat_fn f) {
    for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
        for (unsigned x = 0; x < DSI_LCD_H_RES; x++) px(x, y, f(x, y));
    g_last_pat = f;
}

static void draw_text_page(void) {
    fill(0x00);
    static const char *const lines[] = {
#if defined(CONFIG_BOARD_TAB5)
        "LugalOS 56 -- frame buffer in PSRAM, DW-GDMA into DSI bridge, ST7121 720x1280",
        "The quick brown fox jumps over the lazy dog.  0123456789  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
        "90 x 80 cells of 8x16 Spleen -- M5Stack Tab5 5\" IPS display.",
#else
        "LugalOS 47.5 -- frame buffer in PSRAM, DW-GDMA into the DSI bridge, EK79007 1024x600",
        "The quick brown fox jumps over the lazy dog.  0123456789  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",
        "126 x 37 cells of 8x16 Spleen -- the RP2350-LCD-7's font, one byte per pixel here.",
#endif
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


/* ---- 10. the terminal's backend (47.7) -----------------------------------
 *
 * drivers/lcdterm.c draws the terminal into a 1-bpp frame -- the RP2350's
 * format, leftmost pixel in bit 0 -- and calls term_flush() when it has
 * drawn. Here that frame lives in PSRAM with a shadow of what was last
 * converted; a flush compares each 128-byte row with its shadow and converts
 * only the rows that differ into the scanned-out frame, then writes those
 * rows back for the DMA. The plan's fallback to a per-row dirty bitmap in
 * canvas1_t (§4): no hook in the drawing code, 75 KB of PSRAM for the
 * shadow, and a compare of 75 KB per flush.
 *
 * The conversion is a table: an input byte is 8 pixels, so 8 output bytes
 * (GRAY8: one 64-bit store) or 8 RGB565 pixels. Foreground and background
 * are any two RGB565 colours, as `lcd colours` on the RP2350: a gray pair
 * runs in GRAY8, a coloured one switches the frame to RGB565 (47.5 §6.6). */

/* g_lcd_rot and the terminal state below -- guarded by: console_lock, which
 * every writer of the 1-bpp frame, every conversion and the rotation hold
 * (dsi_lcd_term_start() runs at boot, before any of them). */
#if defined(CONFIG_BOARD_TAB5)
#define T_BYTES    (160u * 720u)
static int g_lcd_rot = 270; /* Landscape by default */
#else
#define T_STRIDE   (DSI_LCD_H_RES / 8u)
#define T_BYTES    (T_STRIDE * DSI_LCD_V_RES)
static int g_lcd_rot = 0;   /* Native scanout (portrait or LCD-7B landscape) */
#endif

static inline uint8_t rev8(uint8_t v) {
    v = (uint8_t)(((v >> 1) & 0x55u) | ((v & 0x55u) << 1));
    v = (uint8_t)(((v >> 2) & 0x33u) | ((v & 0x33u) << 2));
    return (uint8_t)((v >> 4) | (v << 4));
}

/* guarded by: console_lock, as g_lcd_rot above. */
static lcdterm_backend_t g_tbe;
static uint8_t  *g_t1, *g_t1_shadow;            /* 1-bpp frame, its shadow: PSRAM */
static bool      g_t_force;                      /* convert every row next time */
static uint16_t  g_t_fg = 0x0000, g_t_bg = 0xffff;   /* black on white, the RP2350's */
static uint64_t  g_lut8[256];                    /* GRAY8: 8 pixels per byte */
static uint16_t (*g_lut565)[8];                  /* RGB565: 4 KB, one bulk page */
static uint32_t  g_t_flushes, g_t_rows, g_t_us_last, g_t_us_max, g_t_rows_last;

static uint8_t gray_of(uint16_t c, bool *is_gray) {
    unsigned r = ((c >> 11) & 0x1fu) * 255u / 31u, g = ((c >> 5) & 0x3fu) * 255u / 63u,
             b = (c & 0x1fu) * 255u / 31u;
    unsigned mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    unsigned mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *is_gray = (mx - mn) <= 8u;
    return (uint8_t)((r + g + b) / 3u);
}

static void lut_build(void) {
    bool gf, gb;
    uint8_t f8 = gray_of(g_t_fg, &gf), b8 = gray_of(g_t_bg, &gb);
    for (unsigned v = 0; v < 256u; v++) {
        uint64_t w = 0;
        for (unsigned bit = 0; bit < 8u; bit++)
            w |= (uint64_t)((v >> bit) & 1u ? f8 : b8) << (8u * bit);   /* bit 0 = leftmost = lowest byte */
        g_lut8[v] = w;
        if (g_lut565)
            for (unsigned bit = 0; bit < 8u; bit++) g_lut565[v][bit] = ((v >> bit) & 1u) ? g_t_fg : g_t_bg;
    }
}

/* A row against its shadow, 64 bits at a time: libc's memcmp goes byte by
 * byte, and this runs over all 600 rows at every flush -- it was most of a
 * character's 3.2 ms (47.7). Both buffers are page-aligned. */
static inline bool row_same(const uint8_t *a, const uint8_t *b, unsigned stride) {
    const uint64_t *x = (const uint64_t *)(const void *)a, *y = (const uint64_t *)(const void *)b;
    uint64_t d = 0;
    unsigned n = stride / 8u;
    for (unsigned i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}

/* 47.7: frame pacing. Once the lcdflush task runs, a flush only marks the
 * frame due, and the task converts at most once per frame: the panel shows
 * 60 frames a second, so converting a scroll more often than that is work
 * nobody sees -- and fast output scrolls far more often (the owner's "not
 * very fast", 2026-10-11). Before the scheduler, at boot, flushes convert
 * at once.
 * guarded by: console_lock; the flush task reads it without, as a hint, and
 * clears it and converts under the lock. */
static volatile bool g_t_due;
/* 47.7: a test owns the frame (`lcd test`, `fb`, `wbtest`, `bench`): the
 * terminal keeps drawing into its 1-bpp frame but converts nothing until
 * `lcd repaint` (or `lcd colours`) hands the glass back -- otherwise the
 * next scroll converts over the test's pixels, and `lcd verify` fails. */
static bool          g_t_hold;
static bool          g_t_paced;
static uint32_t      g_t_paced_passes;

static void term_convert(unsigned y0, unsigned y1);

static void term_flush(unsigned y0, unsigned y1) {
    if (g_t_paced) { g_t_due = true; return; }
    term_convert(y0, y1);
}

/* Widens [*lo, *hi], the physical rows written, by logical rows y0..y1 of a
 * portrait frame: the same rows at 0 deg, mirrored at 180. */
static inline void span_rows(int *lo, int *hi, int y0, int y1) {
    if (g_lcd_rot == 180) {
        int a = (int)DSI_LCD_V_RES - 1 - y1, b = (int)DSI_LCD_V_RES - 1 - y0;
        y0 = a;
        y1 = b;
    }
    if (*lo < 0 || y0 < *lo) *lo = y0;
    if (y1 > *hi) *hi = y1;
}

static void term_convert(unsigned y0, unsigned y1) {
    /* The frame once: every switch of it (fb_start(), from `lcd colours` or
     * `lcd fb`) holds console_lock, as this does -- the lcdflush task once
     * converted into a frame that `lcd colours` had just set to NULL (a
     * store to 0xad000, row 346 of an RGB565 frame; 47.7's HIL run). */
    if (!fb_back() || !g_t1 || g_fb_fmt == FB_NONE || g_t_hold) return;
    /* While a flip is pending the back buffer is the next front: drawing into
     * it would put a half-drawn frame on the glass at the frame end. So the
     * flip is taken back instead of waited for (under g_scan_lock, so a frame
     * end either swapped before it or does not swap), this pass draws on top
     * of the same back buffer, and the flip is armed again below. */
    bool rearm = false;
    if (g_double_buffered && g_flip_pending) {
        uintptr_t f = spin_lock_irqsave(&g_scan_lock);
        rearm = g_flip_pending;
        g_flip_pending = false;
        spin_unlock_irqrestore(&g_scan_lock, f);
    }
    uint8_t *fb = fb_back();
    fb_fmt_t fmt = g_fb_fmt;
    if (y1 > g_tbe.h) y1 = g_tbe.h;
    uint64_t t0 = time_get_us();

    unsigned stride = g_tbe.stride;
    unsigned total_bytes = stride * g_tbe.h;

    /* 1. Detect rows changed in 1-bpp frame and mark dirty in both buffers */
    bool all = g_t_force && y0 == 0 && y1 == g_tbe.h;
    if (all) {
        memset(g_dirty, 0xff, sizeof g_dirty);
        g_dirty_trows[0] = ~0ull;
        g_dirty_trows[1] = ~0ull;
        g_dirty_x0[0] = 0; g_dirty_x1[0] = (uint16_t)(g_tbe.w - 1);
        g_dirty_x0[1] = 0; g_dirty_x1[1] = (uint16_t)(g_tbe.w - 1);
        memset(g_t_cell_drawn, 0xff, sizeof g_t_cell_drawn);
        memset(s_reader_drawn_seq, 0xff, sizeof s_reader_drawn_seq);
        memcpy(g_t1_shadow, g_t1, total_bytes);
        g_t_force = false;
    } else {
        for (unsigned y = y0; y < y1; y++) {
            const uint8_t *src = g_t1 + y * stride;
            uint8_t *sh = g_t1_shadow + y * stride;
            if (g_t_force || !row_same(src, sh, stride)) {
                if (g_t_force) {
                    g_dirty_x0[0] = 0; g_dirty_x1[0] = (uint16_t)(g_tbe.w - 1);
                    g_dirty_x0[1] = 0; g_dirty_x1[1] = (uint16_t)(g_tbe.w - 1);
                } else {
                    const uint64_t *s64 = (const uint64_t *)src;
                    const uint64_t *h64 = (const uint64_t *)sh;
                    unsigned num_words = stride / 8u;
                    unsigned w_min = 0;
                    while (w_min < num_words && s64[w_min] == h64[w_min]) w_min++;
                    unsigned w_max = num_words - 1u;
                    while (w_max > w_min && s64[w_max] == h64[w_max]) w_max--;
                    unsigned b_min = w_min * 8u;
                    unsigned b_max = w_max * 8u + 7u;
                    while (b_min < stride && src[b_min] == sh[b_min]) b_min++;
                    while (b_max > b_min && src[b_max] == sh[b_max]) b_max--;
                    unsigned x_min = b_min * 8u;
                    unsigned x_max = b_max * 8u + 7u;
                    for (int b = 0; b < 2; b++) {
                        if (x_min < g_dirty_x0[b]) g_dirty_x0[b] = (uint16_t)x_min;
                        if (x_max > g_dirty_x1[b]) g_dirty_x1[b] = (uint16_t)x_max;
                    }
                }
                memcpy(sh, src, stride);
                unsigned w = y / 64u;
                uint64_t mask = 1ull << (y % 64u);
                g_dirty[0][w] |= mask;
                g_dirty[1][w] |= mask;
                if (y >= SCREEN_TEXT_Y) {
                    unsigned tr = (y - SCREEN_TEXT_Y) / 32u;
                    if (tr < 64u) {
                        uint64_t tmask = 1ull << tr;
                        g_dirty_trows[0] |= tmask;
                        g_dirty_trows[1] |= tmask;
                    }
                }
            }
        }
    }

    /* 2. Convert dirty rows for the back buffer */
    unsigned back_idx = g_double_buffered ? (1u - g_front_idx) : 0u;
    int lo = -1, hi = -1;
    int flush_py0 = -1, flush_py1 = -1;
    uint32_t n = 0;

    if (g_lcd_rot == 90 || g_lcd_rot == 270) {
        /* Landscape Mode (1280x720): logical rows y in 0..719, stride 160 bytes.
         * We convert dirty 8-row bands into physical 720x1280 back buffer. */
        unsigned bx_start = 0, bx_end = stride;
        if (g_dirty_x0[back_idx] <= g_dirty_x1[back_idx]) {
            bx_start = (unsigned)g_dirty_x0[back_idx] / 8u;
            bx_end = ((unsigned)g_dirty_x1[back_idx] / 8u) + 1u;
            if (bx_end > stride) bx_end = stride;
        }
        g_dirty_x0[back_idx] = 0xffff;
        g_dirty_x1[back_idx] = 0;

        for (unsigned w = 0; w < DIRTY_WORDS; w++) {
            uint64_t bits = g_dirty[back_idx][w];
            if (!bits) continue;
            g_dirty[back_idx][w] = 0;
            for (unsigned bit = 0; bit < 64u; bit++) {
                if (!(bits & (1ull << bit))) continue;
                unsigned y = w * 64u + bit;
                if (y >= g_tbe.h) break;
                unsigned y0_band = (y / 8u) * 8u;
                const uint8_t *r0 = g_t1_shadow + (y0_band + 0u) * stride;
                const uint8_t *r1 = g_t1_shadow + (y0_band + 1u) * stride;
                const uint8_t *r2 = g_t1_shadow + (y0_band + 2u) * stride;
                const uint8_t *r3 = g_t1_shadow + (y0_band + 3u) * stride;
                const uint8_t *r4 = g_t1_shadow + (y0_band + 4u) * stride;
                const uint8_t *r5 = g_t1_shadow + (y0_band + 5u) * stride;
                const uint8_t *r6 = g_t1_shadow + (y0_band + 6u) * stride;
                const uint8_t *r7 = g_t1_shadow + (y0_band + 7u) * stride;

                for (unsigned bx = bx_start; bx < bx_end; bx++) {
                    uint8_t b0 = r0[bx], b1 = r1[bx], b2 = r2[bx], b3 = r3[bx];
                    uint8_t b4 = r4[bx], b5 = r5[bx], b6 = r6[bx], b7 = r7[bx];

                    for (unsigned b = 0; b < 8u; b++) {
                        uint8_t c = ((b0 >> b) & 1u) |
                                   (((b1 >> b) & 1u) << 1) |
                                   (((b2 >> b) & 1u) << 2) |
                                   (((b3 >> b) & 1u) << 3) |
                                   (((b4 >> b) & 1u) << 4) |
                                   (((b5 >> b) & 1u) << 5) |
                                   (((b6 >> b) & 1u) << 6) |
                                   (((b7 >> b) & 1u) << 7);
                        if (g_lcd_rot == 270) {
                            unsigned py = 1279u - (bx * 8u + b);
                            uint16_t *dst = (uint16_t *)fb + (size_t)py * 720u + y0_band;
                            memcpy(dst, g_lut565[c], 16u);
                        } else {
                            unsigned py = bx * 8u + b;
                            uint16_t *dst = (uint16_t *)fb + (size_t)py * 720u + (712u - y0_band);
                            memcpy(dst, g_lut565[rev8(c)], 16u);
                        }
                    }
                }
                unsigned rem = 7u - (y % 8u);
                bit += rem;
                lo = 0;
                hi = 1279;
                n++;
            }
        }
        if (n && bx_start < bx_end) {
            if (g_lcd_rot == 270) {
                unsigned cur_pymin = 1279u - (bx_end * 8u - 1u);
                unsigned cur_pymax = 1279u - (bx_start * 8u);
                if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
                if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;
            } else {
                unsigned cur_pymin = bx_start * 8u;
                unsigned cur_pymax = bx_end * 8u - 1u;
                if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
                if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;
            }
        }
    } else {
        g_dirty_x0[back_idx] = 0xffff;
        g_dirty_x1[back_idx] = 0;
        /* Native orientation (0 deg, or the LCD-7B), or 180 deg: row y to
         * physical row V_RES-1-y, each byte's 8 pixels mirrored to the far
         * end of the row (rev8, as the 90 deg path does). */
        bool flip = g_lcd_rot == 180;
        for (unsigned w = 0; w < DIRTY_WORDS; w++) {
            uint64_t bits = g_dirty[back_idx][w];
            if (!bits) continue;
            g_dirty[back_idx][w] = 0;
            for (unsigned bit = 0; bit < 64u; bit++) {
                if (!(bits & (1ull << bit))) continue;
                unsigned y = w * 64u + bit;
                if (y >= g_tbe.h) break;
                const uint8_t *sh = g_t1_shadow + y * stride;
                unsigned py = flip ? DSI_LCD_V_RES - 1u - y : y;
                if (fmt == FB_RGB565) {
                    uint16_t *dst = (uint16_t *)fb + (size_t)py * DSI_LCD_H_RES;
                    if (flip) {
                        for (unsigned b = 0; b < stride; b++)
                            memcpy(dst + (DSI_LCD_H_RES - 8u - 8u * b), g_lut565[rev8(sh[b])], 16u);
                    } else {
                        for (unsigned b = 0; b < stride; b++) memcpy(dst + 8u * b, g_lut565[sh[b]], 16u);
                    }
                } else {
                    uint64_t *dst = (uint64_t *)(fb + (size_t)py * DSI_LCD_H_RES);
                    if (flip) {
                        for (unsigned b = 0; b < stride; b++) dst[stride - 1u - b] = g_lut8[rev8(sh[b])];
                    } else {
                        for (unsigned b = 0; b < stride; b++) dst[b] = g_lut8[sh[b]];
                    }
                }
                span_rows(&lo, &hi, (int)y, (int)y);
                n++;
            }
        }
    }

    /* 2b. Composite visible anti-aliased Reader windows onto fb_back() */
    uint8_t rids[4];
    int rx0[4], ry0[4], rx1[4], ry1[4];
    int rcount = lcdterm_get_visible_readers(rids, rx0, ry0, rx1, ry1, 4);
    for (int i = 0; i < rcount; i++) {
        uint32_t seq = reader_get_seq(rids[i]);
        if (s_reader_drawn_seq[back_idx][i] == seq && !g_t_force) continue;
        s_reader_drawn_seq[back_idx][i] = seq;

        uint32_t aa_stride = 0;
        int aa_w = 0, aa_h = 0;
        uint8_t *aa_buf = reader_get_aa_backing(rids[i], &aa_stride, &aa_w, &aa_h);
        if (!aa_buf) continue;

        int x0 = rx0[i], x1 = rx1[i];
        int y0 = ry0[i], y1 = ry1[i];

        int cl_x0 = x0 < 0 ? 0 : x0;
        int cl_x1 = x1 >= (int)g_tbe.w ? (int)g_tbe.w - 1 : x1;
        int cl_y0 = y0 < 0 ? 0 : y0;
        int cl_y1 = y1 >= (int)g_tbe.h ? (int)g_tbe.h - 1 : y1;
        if (cl_x0 > cl_x1 || cl_y0 > cl_y1) continue;

        int blit_w = cl_x1 - cl_x0 + 1;
        int src_x_off = cl_x0 - x0;

        for (int y = cl_y0; y <= cl_y1; y++) {
            int src_y = y - y0;
            if (src_y < 0 || src_y >= aa_h) continue;
            const uint8_t *src_line = aa_buf + (uint32_t)src_y * aa_stride + src_x_off;

            if (g_lcd_rot == 270) {
                for (int x = 0; x < blit_w; x++) {
                    uint8_t g = src_line[x];
                    uint16_t c565 = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                    unsigned py = 1279u - (cl_x0 + x);
                    unsigned px = y;
                    ((uint16_t *)fb)[(size_t)py * 720u + px] = c565;
                }
            } else if (g_lcd_rot == 90) {
                for (int x = 0; x < blit_w; x++) {
                    uint8_t g = src_line[x];
                    uint16_t c565 = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                    unsigned py = cl_x0 + x;
                    unsigned px = 719u - y;
                    ((uint16_t *)fb)[(size_t)py * 720u + px] = c565;
                }
            } else if (g_lcd_rot == 180) {
                unsigned py = DSI_LCD_V_RES - 1u - (unsigned)y;
                for (int x = 0; x < blit_w; x++) {
                    uint8_t g = src_line[x];
                    unsigned px = DSI_LCD_H_RES - 1u - (unsigned)(cl_x0 + x);
                    if (fmt == FB_RGB565)
                        ((uint16_t *)fb)[(size_t)py * DSI_LCD_H_RES + px] =
                            (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                    else
                        fb[(size_t)py * DSI_LCD_H_RES + px] = g;
                }
            } else {
                if (fmt == FB_RGB565) {
                    uint16_t *dst_line = (uint16_t *)fb + (size_t)y * DSI_LCD_H_RES + cl_x0;
                    for (int x = 0; x < blit_w; x++) {
                        uint8_t g = src_line[x];
                        dst_line[x] = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                    }
                } else {
                    uint8_t *dst_line = fb + (size_t)y * DSI_LCD_H_RES + cl_x0;
                    memcpy(dst_line, src_line, (size_t)blit_w);
                }
            }
        }
        span_rows(&lo, &hi, cl_y0, cl_y1);
        if (g_lcd_rot == 270) {
            unsigned cur_pymin = 1279u - (unsigned)cl_x1;
            unsigned cur_pymax = 1279u - (unsigned)cl_x0;
            if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
            if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;
        } else if (g_lcd_rot == 90) {
            unsigned cur_pymin = (unsigned)cl_x0;
            unsigned cur_pymax = (unsigned)cl_x1;
            if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
            if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;
        }
        n++;
    }

    /* 2c. Composite visible RGB565 color canvases onto fb_back() */
    uint8_t cids[4];
    int cx0[4], cy0[4], cx1[4], cy1[4];
    int ccount = lcdterm_get_visible_canvases565(cids, cx0, cy0, cx1, cy1, 4);
    if (ccount > 0 && g_fb_fmt != FB_RGB565) {
        if (!g_lut565) {
            g_lut565 = (uint16_t (*)[8])palloc_pages_bulk(1);
        }
        if (g_lut565 && fb_start(FB_RGB565)) {
            lut_build();
            fmt = g_fb_fmt;
            fb = fb_back();
            for (unsigned y = 0; y < DSI_LCD_V_RES; y++) {
                uint16_t *dst = (uint16_t *)fb + (size_t)y * DSI_LCD_H_RES;
                const uint8_t *sh = g_t1 + y * stride;
                for (unsigned b = 0; b < stride; b++) {
                    const uint16_t *pix = g_lut565[sh[b]];
                    for (unsigned p = 0; p < 8; p++) dst[b * 8 + p] = pix[p];
                }
            }
            lo = 0;
            hi = DSI_LCD_V_RES - 1;
            n = DSI_LCD_V_RES;
        }
    }
    for (int i = 0; i < ccount; i++) {
        uint32_t c_stride = 0;
        int c_w = 0, c_h = 0;
        const uint16_t *c_buf = lcdterm_get_canvas565_backing(cids[i], &c_stride, &c_w, &c_h);
        if (!c_buf) continue;

        int x0 = cx0[i], x1 = cx1[i];
        int y0 = cy0[i];
        int y1 = cy1[i];

        int cl_x0 = x0 < 0 ? 0 : x0;
        int cl_x1 = x1 >= (int)g_tbe.w ? (int)g_tbe.w - 1 : x1;
        int cl_y0 = y0 < 0 ? 0 : y0;
        int cl_y1 = y1 >= (int)g_tbe.h ? (int)g_tbe.h - 1 : y1;
        if (cl_x0 > cl_x1 || cl_y0 > cl_y1) continue;

        int blit_w = cl_x1 - cl_x0 + 1;
        int src_x_off = cl_x0 - x0;

        for (int y = cl_y0; y <= cl_y1; y++) {
            int src_y = y - y0;
            if (src_y < 0 || src_y >= c_h) continue;
            const uint16_t *src_line = c_buf + (size_t)src_y * c_stride + src_x_off;

            if (g_lcd_rot == 270) {
                for (int x = 0; x < blit_w; x++) {
                    unsigned py = 1279u - (cl_x0 + x);
                    unsigned px = y;
                    ((uint16_t *)fb)[(size_t)py * 720u + px] = src_line[x];
                }
            } else if (g_lcd_rot == 90) {
                for (int x = 0; x < blit_w; x++) {
                    unsigned py = cl_x0 + x;
                    unsigned px = 719u - y;
                    ((uint16_t *)fb)[(size_t)py * 720u + px] = src_line[x];
                }
            } else if (g_lcd_rot == 180) {
                unsigned py = DSI_LCD_V_RES - 1u - (unsigned)y;
                for (int x = 0; x < blit_w; x++) {
                    unsigned px = DSI_LCD_H_RES - 1u - (unsigned)(cl_x0 + x);
                    uint16_t pix = src_line[x];
                    if (fmt == FB_RGB565) {
                        ((uint16_t *)fb)[(size_t)py * DSI_LCD_H_RES + px] = pix;
                    } else {
                        uint8_t r = (uint8_t)((pix >> 11) & 0x1fu);
                        uint8_t g = (uint8_t)((pix >> 5) & 0x3fu);
                        uint8_t b = (uint8_t)(pix & 0x1fu);
                        fb[(size_t)py * DSI_LCD_H_RES + px] =
                            (uint8_t)(((r << 3) * 77 + (g << 2) * 150 + (b << 3) * 29) >> 8);
                    }
                }
            } else {
                if (fmt == FB_RGB565) {
                    uint16_t *dst_line = (uint16_t *)fb + (size_t)y * DSI_LCD_H_RES + cl_x0;
                    memcpy(dst_line, src_line, (size_t)blit_w * sizeof(uint16_t));
                } else {
                    uint8_t *dst_line = fb + (size_t)y * DSI_LCD_H_RES + cl_x0;
                    for (int x = 0; x < blit_w; x++) {
                        uint16_t pix = src_line[x];
                        uint8_t r = (uint8_t)((pix >> 11) & 0x1fu);
                        uint8_t g = (uint8_t)((pix >> 5) & 0x3fu);
                        uint8_t b = (uint8_t)(pix & 0x1fu);
                        dst_line[x] = (uint8_t)(((r << 3) * 77 + (g << 2) * 150 + (b << 3) * 29) >> 8);
                    }
                }
            }
        }
        span_rows(&lo, &hi, cl_y0, cl_y1);
        if (g_lcd_rot == 270) {
            unsigned cur_pymin = 1279u - (unsigned)cl_x1;
            unsigned cur_pymax = 1279u - (unsigned)cl_x0;
            if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
            if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;
        } else if (g_lcd_rot == 90) {
            unsigned cur_pymin = (unsigned)cl_x0;
            unsigned cur_pymax = (unsigned)cl_x1;
            if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
            if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;
        }
        n++;
    }

    /* 2d. Composite visible anti-aliased Terminal windows onto fb_back() */
#if defined(CONFIG_FONT_CELL_H) && (CONFIG_FONT_CELL_H == 32)
    if (fmt == FB_RGB565) {
        uint64_t trows_mask = g_dirty_trows[back_idx];
        g_dirty_trows[back_idx] = 0;
        if (trows_mask) {
            lcdterm_visible_term_t terms[4];
            int tcount = lcdterm_get_visible_terms(terms, 4);
            if (tcount > 0) {
                uint16_t norm_lut[16];
                uint16_t inv_lut[16];
                uint32_t fg_r = (g_t_fg >> 11) & 0x1Fu, fg_g = (g_t_fg >> 5) & 0x3Fu, fg_b = g_t_fg & 0x1Fu;
                uint32_t bg_r = (g_t_bg >> 11) & 0x1Fu, bg_g = (g_t_bg >> 5) & 0x3Fu, bg_b = g_t_bg & 0x1Fu;
                for (unsigned a = 0; a < 16; a++) {
                    unsigned inv_a = 15u - a;
                    uint32_t nr = (fg_r * a + bg_r * inv_a + 7u) / 15u;
                    uint32_t ng = (fg_g * a + bg_g * inv_a + 7u) / 15u;
                    uint32_t nb = (fg_b * a + bg_b * inv_a + 7u) / 15u;
                    norm_lut[a] = (uint16_t)((nr << 11) | (ng << 5) | nb);

                    uint32_t ir = (bg_r * a + fg_r * inv_a + 7u) / 15u;
                    uint32_t ig = (bg_g * a + fg_g * inv_a + 7u) / 15u;
                    uint32_t ib = (bg_b * a + fg_b * inv_a + 7u) / 15u;
                    inv_lut[a] = (uint16_t)((ir << 11) | (ig << 5) | ib);
                }

                for (int ti = 0; ti < tcount; ti++) {
                    const lcdterm_visible_term_t *vt = &terms[ti];
                    for (uint16_t row = 0; row < vt->rows; row++) {
                        if (!(trows_mask & (1ull << row))) continue;
                        int cell_y0 = vt->y0 + (int)row * 32;
                        if (cell_y0 + 31 < 0 || cell_y0 >= (int)g_tbe.h) continue;

                    for (uint16_t col = 0; col < vt->cols; col++) {
                        int cell_x0 = vt->x0 + (int)col * 16;
                        if (cell_x0 + 15 < 0 || cell_x0 >= (int)g_tbe.w) continue;

                        uint16_t sh_idx = row * vt->shadow_cols + (col + vt->col_offset);
                        uint16_t v = vt->shadow[sh_idx];
                        bool is_cursor = ((int)col == vt->cur_col && (int)row == vt->cur_row);
                        bool is_inv = (v & VT_CELL_INVERSE) != 0;
                        uint8_t code = (uint8_t)v;

                        unsigned cell_flat_idx = (unsigned)row * vt->cols + col;
                        uint16_t cur_val = v | (is_cursor ? 0x8000u : 0u);
                        if (cell_flat_idx < MAX_TERM_CELLS) {
                            if (g_t_cell_drawn[back_idx][cell_flat_idx] == cur_val) continue;
                            g_t_cell_drawn[back_idx][cell_flat_idx] = cur_val;
                        }

                        const uint16_t *lut = is_inv ? inv_lut : norm_lut;
                        const uint8_t (*glyph)[8] = NULL;
                        if (code >= FONT_NOTO_MONO_FIRST) {
                            glyph = font_noto_mono_aa_glyphs[code - FONT_NOTO_MONO_FIRST];
                        } else if (code > 0x20) {
                            glyph = font_noto_mono_aa_glyphs[FONT_NOTO_MONO_REPLACEMENT - FONT_NOTO_MONO_FIRST];
                        }

                        if (g_lcd_rot == 270) {
                            unsigned cur_pymin = 1279u - ((unsigned)cell_x0 + 15u);
                            unsigned cur_pymax = 1279u - (unsigned)cell_x0;
                            if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
                            if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;

                            for (unsigned dx = 0; dx < 16u; dx++) {
                                unsigned shift = (dx & 1u) ? 4u : 0u;
                                unsigned byte_idx = dx >> 1;
                                unsigned py = 1279u - ((unsigned)cell_x0 + dx);
                                uint16_t *dst = (uint16_t *)fb + (size_t)py * 720u + (unsigned)cell_y0;
                                unsigned dy_max = is_cursor ? 30u : 32u;
                                for (unsigned dy = 0; dy < dy_max; dy++) {
                                    uint8_t pair = glyph ? glyph[dy][byte_idx] : 0;
                                    uint8_t a = (pair >> shift) & 0x0Fu;
                                    dst[dy] = lut[a];
                                }
                                if (is_cursor) {
                                    uint16_t cpix = is_inv ? g_t_bg : g_t_fg;
                                    dst[30] = cpix;
                                    dst[31] = cpix;
                                }
                            }
                        } else if (g_lcd_rot == 90) {
                            unsigned cur_pymin = (unsigned)cell_x0;
                            unsigned cur_pymax = (unsigned)cell_x0 + 15u;
                            if (flush_py0 < 0 || (int)cur_pymin < flush_py0) flush_py0 = (int)cur_pymin;
                            if ((int)cur_pymax > flush_py1) flush_py1 = (int)cur_pymax;

                            for (unsigned dx = 0; dx < 16u; dx++) {
                                unsigned shift = (dx & 1u) ? 4u : 0u;
                                unsigned byte_idx = dx >> 1;
                                unsigned py = (unsigned)cell_x0 + dx;
                                uint16_t *dst = (uint16_t *)fb + (size_t)py * 720u + (719u - ((unsigned)cell_y0 + 31u));
                                unsigned dy_max = is_cursor ? 30u : 32u;
                                for (unsigned dy = 0; dy < dy_max; dy++) {
                                    uint8_t pair = glyph ? glyph[dy][byte_idx] : 0;
                                    uint8_t a = (pair >> shift) & 0x0Fu;
                                    dst[31u - dy] = lut[a];
                                }
                                if (is_cursor) {
                                    uint16_t cpix = is_inv ? g_t_bg : g_t_fg;
                                    dst[1] = cpix;
                                    dst[0] = cpix;
                                }
                            }
                        } else if (g_lcd_rot == 180) {
                            /* Clipped pixel by pixel: a window that is half
                             * off-screen (several terminals in the ribbon)
                             * has cells running past an edge, and mirrored
                             * coordinates past an edge are out of the frame. */
                            unsigned dy_max = is_cursor ? 30u : 32u;
                            for (unsigned dy = 0; dy < 32u; dy++) {
                                int ly = cell_y0 + (int)dy;
                                if (ly < 0 || ly >= (int)DSI_LCD_V_RES) continue;
                                unsigned py = DSI_LCD_V_RES - 1u - (unsigned)ly;
                                uint16_t *row = (uint16_t *)fb + (size_t)py * DSI_LCD_H_RES;
                                for (unsigned dx = 0; dx < 16u; dx++) {
                                    int lx = cell_x0 + (int)dx;
                                    if (lx < 0 || lx >= (int)DSI_LCD_H_RES) continue;
                                    uint16_t pix;
                                    if (dy < dy_max) {
                                        uint8_t pair = glyph ? glyph[dy][dx >> 1] : 0;
                                        pix = lut[(dx & 1u) ? (pair >> 4) : (pair & 0x0Fu)];
                                    } else {
                                        pix = is_inv ? g_t_bg : g_t_fg;
                                    }
                                    row[DSI_LCD_H_RES - 1u - (unsigned)lx] = pix;
                                }
                            }
                        } else {
                            unsigned dy_max = is_cursor ? 30u : 32u;
                            for (unsigned dy = 0; dy < dy_max; dy++) {
                                uint16_t *dst = (uint16_t *)fb + (size_t)((unsigned)cell_y0 + dy) * DSI_LCD_H_RES + (unsigned)cell_x0;
                                for (unsigned dx = 0; dx < 16u; dx++) {
                                    uint8_t pair = glyph ? glyph[dy][dx >> 1] : 0;
                                    uint8_t a = (dx & 1u) ? (pair >> 4) : (pair & 0x0Fu);
                                    dst[dx] = lut[a];
                                }
                            }
                            if (is_cursor) {
                                uint16_t cpix = is_inv ? g_t_bg : g_t_fg;
                                for (unsigned dy = 30u; dy < 32u; dy++) {
                                    uint16_t *dst = (uint16_t *)fb + (size_t)((unsigned)cell_y0 + dy) * DSI_LCD_H_RES + (unsigned)cell_x0;
                                    for (unsigned dx = 0; dx < 16u; dx++) {
                                        dst[dx] = cpix;
                                    }
                                }
                            }
                        }
                        span_rows(&lo, &hi, cell_y0, cell_y0 + 31);
                        n++;
                    }
                }
            }
        }
    }
}
#endif

    /* 3. Flush written rows and arm VSYNC flip */
    if (n) {
        if (g_lcd_rot == 90 || g_lcd_rot == 270) {
            if (flush_py0 >= 0 && flush_py1 >= flush_py0) {
                if (flush_py0 < 0) flush_py0 = 0;
                if (flush_py1 >= (int)DSI_LCD_V_RES) flush_py1 = (int)DSI_LCD_V_RES - 1;
                fb_flush_buffer(fb, (unsigned)flush_py0, (unsigned)flush_py1 + 1u);
            } else {
                fb_flush_buffer(fb, 0, DSI_LCD_V_RES);
            }
        } else {
            fb_flush_buffer(fb, (unsigned)lo, (unsigned)hi + 1u);
        }
    }
    if (g_double_buffered && (n || rearm)) dsi_lcd_flip(false);

    uint32_t us = (uint32_t)(time_get_us() - t0);
    g_t_flushes++;
    g_t_rows += n;
    if (n) { g_t_us_last = us; g_t_rows_last = n; }
    if (us > g_t_us_max) g_t_us_max = us;
}

/* 47.11: idle blanking -- dark, or back to what `lcd backlight` / `lcd bl`
 * last set, without changing either. */
static void term_blank(bool off) {
    bl_set(off || !g_bl_on ? 0u : g_bl_level);
}

/* Display synchronization hooks for intermediate animation frames:
 * 1-bpp rendering into SRAM is off-screen. In double-buffered mode, we convert
 * into the back buffer and flip synchronously at hardware VSYNC, eliminating
 * scanline collision and tearing. */
static void term_sync_pre(void) { }

static void term_sync_post(void) {
#if defined(CONFIG_ESP32P4_REV) && CONFIG_ESP32P4_REV >= 300
    if (!g_double_buffered && g_scanning) {
        REG(DSI_BRG_INT_CLR_REG) = (1u << DSI_BRG_VSYNC_INT_RAW_S);
        uint64_t t0 = time_get_us();
        while ((REG(DSI_BRG_INT_RAW_REG) & (1u << DSI_BRG_VSYNC_INT_RAW_S)) == 0) {
            if (time_get_us() - t0 > 25000u) break;
        }
    }
#endif
    console_lock();
    g_t_force = true;
    term_convert(0, DSI_LCD_V_RES);
    g_t_due = false;
    /* the flip term_convert() armed, taken at the frame end */
    if (g_double_buffered) (void)gdma_wait_flip(50000u);
    console_unlock();
}

static bool term_capture(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride, unsigned *format) {
    if (!g_fb_fmt) return false;
    uint8_t *front = fb_front();
    if (!front) return false;
    *fb = front;
    *w = DSI_LCD_H_RES;
    *h = DSI_LCD_V_RES;
    if (g_fb_fmt == FB_RGB565) {
        *stride = DSI_LCD_H_RES * 2u;
        if (format) *format = SCREEN_FORMAT_RGB565;
    } else {
        *stride = DSI_LCD_H_RES;
        if (format) *format = SCREEN_FORMAT_GRAY8;
    }
    return true;
}

static lcdterm_backend_t g_tbe = {
#if defined(CONFIG_BOARD_TAB5)
    .name            = "M5Tab5",
    .text_test_title = "LugalOS on M5Stack Tab5: Spleen 8x16, 160 x 45 cells, 1280x720 MIPI-DSI ST7121",
    .w               = 1280,
    .h               = 720,
    .stride          = 160,
#else
    .name            = "LCD-7B",
    .text_test_title = "LugalOS on the LCD-7B: Spleen 8x16, 128 x 37 cells, 1-bpp to GRAY8 over DSI at 60 Hz",
    .w               = DSI_LCD_H_RES,
    .h               = DSI_LCD_V_RES,
    .stride          = T_STRIDE,
#endif
    .fb_region_count = 0,              /* the terminal draws from the kernel here (lcdterm.h) */
    .flush           = term_flush,
    .blank           = term_blank,
    .sync_pre        = term_sync_pre,
    .sync_post       = term_sync_post,
    .capture         = term_capture,
    .is_double_buffered = dsi_lcd_is_double_buffered,
};

/* The whole terminal again, from its 1-bpp frame -- after a test pattern
 * drew over the glass, or a colour or format change. */
static void term_redraw(void) {
    console_lock();
    g_t_hold = false;
    g_t_force = true;
    term_convert(0, g_tbe.h);
    console_unlock();
}

/* Every 16 ms: if anything was drawn since the last pass, convert it. Under
 * console_lock, which every writer of the 1-bpp frame holds. */
static void term_flush_task(void *arg) {
    (void)arg;
    for (;;) {
        task_sleep_ms(16);
        if (!g_t_due || lcdterm_is_animating()) continue;
        console_lock();
        g_t_due = false;
        term_convert(0, g_tbe.h);
        g_t_paced_passes++;
        console_unlock();
    }
}

int dsi_lcd_term_task_start(void) {
    if (!g_t1) return -1;
    int pid = task_create("lcdflush", term_flush_task, NULL);
    if (pid < 0) {
        printk("[LCD] no lcdflush task; the terminal converts at every flush\n");
        return -1;
    }
    g_t_paced = true;
    return pid;
}

/* `lcd colours FG BG`: RGB565 hex, as on the RP2350. */
static void term_colours(uint16_t fg, uint16_t bg) {
    bool gf, gb;
    (void)gray_of(fg, &gf);
    (void)gray_of(bg, &gb);
#if defined(CONFIG_BOARD_TAB5)
    fb_fmt_t want = FB_RGB565;         /* v1.3 bridge uses RGB565 natively */
#else
    fb_fmt_t want = (gf && gb) ? FB_GRAY8 : FB_RGB565;
#endif
    console_lock();                     /* the lcdflush task converts under it */
    g_t_fg = fg;
    g_t_bg = bg;
    if (want == FB_RGB565 && !g_lut565) {
        g_lut565 = (uint16_t (*)[8])palloc_pages_bulk(1);
        if (!g_lut565) { console_unlock(); cprintf("lcd: no page for the RGB565 table\n"); return; }
    }
    if (want != g_fb_fmt && !fb_start(want)) { console_unlock(); return; }
    lut_build();
    term_redraw();
    console_unlock();
}

/* `lcd termbench`: what a character and a scroll cost on the glass, through
 * the real console path (console_putc, then the flush that runs at every
 * write boundary), conversion and write-back included. The tee is off
 * meanwhile: at 115200 baud the UART would be what is measured. */
static void term_bench(void) {
    unsigned tee = lcdterm_tee();
    lcdterm_set_tee(LCD_TEE_OFF);
    bool paced = g_t_paced;
    g_t_paced = false;                 /* the cost of one, not the pacing */
    uint32_t ch_best = 0xffffffffu, ch_worst = 0, ch_sum = 0;
    uint32_t sc_best = 0xffffffffu, sc_worst = 0, sc_sum = 0;
    enum { CH = 60, SC = 40 };
    console_lock();
    console_putc('\n');
    console_flush();
    for (unsigned i = 0; i < CH; i++) {
        uint64_t t0 = time_get_us();
        console_putc((char)('a' + i % 26u));
        console_flush();
        uint32_t d = (uint32_t)(time_get_us() - t0);
        ch_sum += d; if (d < ch_best) ch_best = d; if (d > ch_worst) ch_worst = d;
    }
    for (unsigned i = 0; i < SC; i++) {
        uint64_t t0 = time_get_us();
        console_putc('\n');
        console_flush();
        uint32_t d = (uint32_t)(time_get_us() - t0);
        sc_sum += d; if (d < sc_best) sc_best = d; if (d > sc_worst) sc_worst = d;
    }
    /* And throughput: 200 lines of 55 characters, paced as normally. */
    g_t_paced = paced;
    uint64_t t1 = time_get_us();
    for (unsigned i = 0; i < 200u; i++) {
        for (unsigned k = 0; k < 55u; k++) console_putc((char)('A' + (i + k) % 26u));
        console_putc('\n');
        console_flush();
    }
    uint32_t lines_us = (uint32_t)(time_get_us() - t1);
    /* The same with the tee as it was: what a busy console actually gets. */
    lcdterm_set_tee(tee);
    uint64_t t2 = time_get_us();
    for (unsigned i = 0; i < 200u; i++) {
        for (unsigned k = 0; k < 55u; k++) console_putc((char)('a' + (i + k) % 26u));
        console_putc('\n');
        console_flush();
    }
    uint32_t tee_us = (uint32_t)(time_get_us() - t2);
    console_unlock();
    task_sleep_ms(40);                 /* let the pacer show the last of it */
    cprintf("lcd termbench: 200 lines of 55 in %lu ms = %lu lines/s (%s); with the tee to %s: %lu ms = %lu lines/s\n",
            (unsigned long)(lines_us / 1000u),
            (unsigned long)(200000000ull / (lines_us ? lines_us : 1u)), paced ? "frame-paced" : "unpaced",
            tee == LCD_TEE_OFF ? "nowhere" : "UART0", (unsigned long)(tee_us / 1000u),
            (unsigned long)(200000000ull / (tee_us ? tee_us : 1u)));
    cprintf("lcd termbench: a character %lu us (best %lu, worst %lu); a scroll %lu us (best %lu, worst %lu); "
            "last flush %lu rows\n",
            (unsigned long)(ch_sum / CH), (unsigned long)ch_best, (unsigned long)ch_worst,
            (unsigned long)(sc_sum / SC), (unsigned long)sc_best, (unsigned long)sc_worst,
            (unsigned long)g_t_rows_last);
}

static void term_report(void) {
    if (!g_t1) return;
    cprintf("     terminal: 1-bpp frame 0x%08lx, fg 0x%04x bg 0x%04x; %lu conversions (%s, %lu paced passes), "
            "%lu rows converted, last %lu rows in %lu us, worst %lu us\n",
            (unsigned long)(uintptr_t)g_t1, (unsigned)g_t_fg, (unsigned)g_t_bg, (unsigned long)g_t_flushes,
            g_t_paced ? "frame-paced" : "at every flush", (unsigned long)g_t_paced_passes,
            (unsigned long)g_t_rows, (unsigned long)g_t_rows_last, (unsigned long)g_t_us_last,
            (unsigned long)g_t_us_max);
    cprintf("     front %u, flip %s, due %s, hold %s, force %s, animating %s, rot %d, %lu flips\n",
            (unsigned)g_front_idx, g_flip_pending ? "pending" : "-", g_t_due ? "yes" : "no",
            g_t_hold ? "yes" : "no", g_t_force ? "yes" : "no", lcdterm_is_animating() ? "yes" : "no",
            g_lcd_rot, (unsigned long)g_flips);
    lcdterm_report();
}

int dsi_lcd_term_start(void) {
#if defined(CONFIG_BOARD_TAB5)
    if (!fb_start(FB_RGB565)) return -1;
#else
    if (!fb_start(FB_GRAY8)) return -1;
#endif
    uint32_t pages = (2u * T_BYTES + 4095u) / 4096u;
    uint8_t *m = palloc_pages_bulk(pages);
    if (!m) { printk("[LCD] no PSRAM for the terminal's 1-bpp frame\n"); return -1; }
    memset(m, 0, 2u * T_BYTES);
    g_t1 = m;
    g_t1_shadow = m + T_BYTES;
    g_tbe.fb = g_t1;
    if (g_fb_fmt == FB_RGB565 && !g_lut565) {
        g_lut565 = (uint16_t (*)[8])palloc_pages_bulk(1);
    }
    lut_build();
    g_t_force = true;
    if (lcdterm_init(&g_tbe) != 0) return -1;
    term_redraw();
    printk("[LCD] terminal on the panel: %u x %u cells, 1-bpp frame at 0x%08lx, %s frame at 0x%08lx%s\n",
           g_tbe.w / (unsigned)FONT_CELL_W, g_tbe.h / (unsigned)FONT_CELL_H,
           (unsigned long)(uintptr_t)g_t1, g_fb_fmt == FB_RGB565 ? "RGB565" : "GRAY8",
           (unsigned long)(uintptr_t)fb_front(),
           g_double_buffered ? " (double-buffered)" : "");
    return 0;
}

int dsi_lcd_get_orientation(void) {
    return g_lcd_rot;
}

int dsi_lcd_set_orientation(int rot) {
#if defined(CONFIG_BOARD_TAB5)
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) return -1;
    if (g_lcd_rot == rot) return 0;
    console_lock();
    g_lcd_rot = rot;
    if (rot == 90 || rot == 270) {
        g_tbe.w = 1280;
        g_tbe.h = 720;
        g_tbe.stride = 160;
    } else {
        g_tbe.w = 720;
        g_tbe.h = 1280;
        g_tbe.stride = 90;
    }
    if (g_t1) {
        memset(g_t1, 0, T_BYTES);
        memset(g_t1_shadow, 0, T_BYTES);
        lcdterm_reinit(&g_tbe);
    }
    /* The frame buffers are not cleared: the full redraw below rewrites
     * every pixel of both, and clearing the one being scanned out put black
     * on the glass until then. */
    console_unlock();
    /* lcdterm_reinit() only marks the screen damaged; without a repaint the
     * panel stays black until the next console output. Then both frames:
     * the one not being shown is otherwise left blank until the next change
     * lands in it, and a second flip before then showed it for up to a
     * minute (until the menu bar's clock moved). */
    lcdterm_repaint();
    for (int i = 0; i < (g_double_buffered ? 2 : 1); i++) term_sync_post();
    console_sync_readers();
    return 0;
#else
    (void)rot;
    return 0;
#endif
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
    printk("[LCD] %s %ux%u, DSI %u lanes x %u Mbps (PLL N %u M %u, hsfreq 0x%02x), "
           "DPI 240/%lu MHz, LDO3 dref %u mul %u, %lu us\n",
#if defined(CONFIG_BOARD_TAB5)
           "ST7121",
#else
           "EK79007",
#endif
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

static void term_report(void);

static void report(void) {
    cprintf("lcd: %s -- %s %ux%u, %u lanes x %u Mbps\n", g_up ? "up" : "down",
#if defined(CONFIG_BOARD_TAB5)
            "ST7121",
#else
            "EK79007",
#endif
            DSI_LCD_H_RES, DSI_LCD_V_RES, LANES, LANE_MBPS);
    if (!g_tried) { cprintf("     not initialised (`lcd pattern bars` brings it up)\n"); return; }
    cprintf("     %s; pattern: %s; backlight %s; init %lu us, %lu command errors\n", g_reason,
            g_pattern, g_bl_on ? "on" : "off", (unsigned long)g_init_us, (unsigned long)g_cmd_errs);
    if (g_pwr_mode >= 0) cprintf("     power mode (DCS 0x0A, at init) 0x%02x\n", (unsigned)g_pwr_mode);
    else cprintf("     power mode (DCS 0x0A, at init): no answer\n");
    cprintf("     LDO3 %u mV: dref %u mul %u (K %d Vos %d C %d)\n", LDO3_MV, g_ldo_dref, g_ldo_mul,
            g_ldo_k, g_ldo_vos, g_ldo_c);
    cprintf("     PHY PLL: %s %u MHz x %u / %u, hsfreqrange 0x%02x; status 0x%08lx\n",
#if defined(CONFIG_ESP32P4_REV) && CONFIG_ESP32P4_REV >= 300
            "XTAL",
#else
            "PLL_F20M",
#endif
            PHY_REF_MHZ, g_pll_m, g_pll_n, g_hsfreq, (unsigned long)REG(DSI_HOST_PHY_STATUS_REG));
    cprintf("     DPI 240/%lu MHz; host hsa %lu hbp %lu hline %lu; bridge hfp %lu\n",
            (unsigned long)g_dpi_div, (unsigned long)g_host_hsa, (unsigned long)g_host_hbp,
            (unsigned long)g_host_hline, (unsigned long)g_brg_hfp);
    cprintf("     host version 0x%08lx, mode %s, vid_mode_cfg 0x%08lx; bridge 0x%08lx, int_raw 0x%lx\n",
            (unsigned long)REG(DSI_HOST_VERSION_REG),
            FGET(DSI_HOST_MODE_CFG_REG, DSI_HOST_CMD_VIDEO_MODE) ? "command" : "video",
            (unsigned long)REG(DSI_HOST_VID_MODE_CFG_REG), (unsigned long)REG(DSI_BRG_VER_DATE_REG),
            (unsigned long)REG(DSI_BRG_INT_RAW_REG));
    if (fb_front())
        cprintf("     frame: %s %lu KB at 0x%08lx (double-buffered: %s, %lu flips); %lu frames, %lu DMA errors (last 0x%lx); "
                "bridge underrun %s\n",
                g_fb_fmt == FB_RGB565 ? "RGB565" : "GRAY8", (unsigned long)(g_fb_bytes / 1024u),
                (unsigned long)(uintptr_t)fb_front(),
                g_double_buffered ? "yes" : "no", (unsigned long)g_flips,
                (unsigned long)g_frames, (unsigned long)g_dma_errs,
                (unsigned long)g_dma_last_err,
                FGET(DSI_BRG_INT_RAW_REG, DSI_BRG_UNDERRUN_INT_RAW) ? "SEEN" : "none");
    term_report();
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
    if (g_double_buffered) dsi_lcd_flip(true);
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
    uint8_t *fb = fb_front();
    if (!fb) return;
    fb_flush_buffer(fb, 0, DSI_LCD_V_RES);
    esp32p4_extmem_invalidate((uintptr_t)fb, g_fb_bytes);
    uint32_t bad = 0;
    for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
        for (unsigned x = 0; x < DSI_LCD_H_RES; x++) {
            uint8_t g = g_last_pat(x, y);
            bool ok;
            if (g_fb_fmt == FB_RGB565) {
                uint16_t v = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
                ok = ((uint16_t *)fb)[y * DSI_LCD_H_RES + x] == v;
            } else {
                ok = fb[y * DSI_LCD_H_RES + x] == g;
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
        console_lock();                 /* as fb_start()'s callers: the scan-out state */
        if (!dsi_lcd_init()) { console_unlock(); cprintf("lcd: bring-up failed: %s\n", g_reason); return; }
        set_pattern(pat);
        console_unlock();
        cprintf("lcd: %s\n", g_pattern);
        return;
    }
    if (strncmp(args, "backlight", 9) == 0) {           /* before "bl", which it starts with */
        const char *p = skip_ws(args + 9);
        if (*p < '0' || *p > '9') {
            cprintf("lcd: backlight %u%%%s\n", g_bl_on ? g_bl_level : 0u, g_bl_on ? "" : " (off)");
            if (g_bl_pwm) {
                /* What the channel really runs (DUTY_R, 4 fraction bits), and
                 * the timer moving: two reads of its counter. */
                /* 37 us apart: ~190 counts at 5.12 MHz, never a whole
                 * 1024-count period, so a running timer always reads two
                 * different values (back to back can land inside one count). */
                uint32_t t0 = REG(LEDC_TIMER0_VALUE_REG);
                time_delay_us(37);
                uint32_t t1 = REG(LEDC_TIMER0_VALUE_REG);
                cprintf("lcd:   LEDC ch0: duty %lu/%u at %u Hz (inverted on GPIO%u), timer %lu -> %lu\n",
                        (unsigned long)(REG(LEDC_CH0_DUTY_R_REG) >> 4), 1u << BL_DUTY_RES, BL_PWM_HZ,
                        (unsigned)BL_GPIO, (unsigned long)(t0 & 0xFFFFFu), (unsigned long)(t1 & 0xFFFFFu));
            }
            return;
        }
        unsigned v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10u + (unsigned)(*p++ - '0');
        if (v > 100u) v = 100u;
        /* The RP2350's semantics (lcd7_set_backlight()): 0 is off, and `bl on`
         * then comes back at the last level that was not 0. */
        if (v) g_bl_level = v;
        backlight(v != 0);
        cprintf("lcd: backlight %u%%\n", v);
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
        g_t_hold = (g_t1 != NULL);
        console_lock();                 /* as term_colours(): no conversion mid-switch */
        bool started = fb_start(f);
        console_unlock();
        if (!started) { cprintf("lcd: frame buffer not started\n"); return; }
        draw(pat_grid);
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        if (g_double_buffered) dsi_lcd_flip(true);
        cprintf("lcd: frame buffer %s, %lu KB at 0x%08lx, scanning out (grid drawn)\n",
                f == FB_RGB565 ? "RGB565" : "GRAY8", (unsigned long)(g_fb_bytes / 1024u),
                (unsigned long)(uintptr_t)fb_front());
        return;
    }
    if (strncmp(args, "db", 2) == 0) {
        const char *p = skip_ws(args + 2);
        if (*p == '\0') {
            cprintf("lcd: double-buffering %s (front 0x%08lx, back 0x%08lx, %lu flips)\n",
                    g_double_buffered ? "enabled" : "disabled",
                    (unsigned long)(uintptr_t)fb_front(),
                    (unsigned long)(uintptr_t)fb_back(),
                    (unsigned long)g_flips);
            return;
        }
        if (strcmp(p, "on") == 0) {
            if (g_double_buffered && g_fb_pool[1]) {
                cprintf("lcd: double-buffering already enabled\n");
                return;
            }
            if (!g_fb_pool[0]) {
                cprintf("lcd: no active frame buffer\n");
                return;
            }
            console_lock();
            uint8_t *fb1 = palloc_pages_bulk(g_fb_pages);
            if (!fb1 || !palloc_is_bulk(fb1)) {
                if (fb1) palloc_free(fb1, g_fb_pages);
                console_unlock();
                cprintf("lcd: no PSRAM for second buffer\n");
                return;
            }
            memcpy(fb1, g_fb_pool[0], g_fb_bytes);
            esp32p4_extmem_writeback((uintptr_t)fb1, g_fb_bytes);
            g_fb_pool[1] = fb1;
            g_double_buffered = true;
            memset(g_dirty, 0, sizeof g_dirty);
            g_dirty_trows[0] = 0;
            g_dirty_trows[1] = 0;
            g_dirty_x0[0] = 0xffff; g_dirty_x1[0] = 0;
            g_dirty_x0[1] = 0xffff; g_dirty_x1[1] = 0;
            console_unlock();
            cprintf("lcd: double-buffering enabled (front 0x%08lx, back 0x%08lx)\n",
                    (unsigned long)(uintptr_t)fb_front(),
                    (unsigned long)(uintptr_t)fb_back());
            return;
        }
        if (strcmp(p, "off") == 0) {
            if (!g_double_buffered) {
                cprintf("lcd: double-buffering already disabled\n");
                return;
            }
            console_lock();
            (void)gdma_wait_flip(50000u);
            g_double_buffered = false;
            if (g_front_idx != 0) {
                memcpy(g_fb_pool[0], g_fb_pool[1], g_fb_bytes);
                esp32p4_extmem_writeback((uintptr_t)g_fb_pool[0], g_fb_bytes);
                /* index and item together, so a frame end in between cannot
                 * re-arm on the buffer about to be freed */
                uintptr_t f = spin_lock_irqsave(&g_scan_lock);
                g_front_idx = 0;
                LLI_NC->sar_lo = (uint32_t)(uintptr_t)g_fb_pool[0];
                spin_unlock_irqrestore(&g_scan_lock, f);
                /* the frame in flight still reads buffer 1: let it finish */
                uint32_t f0 = g_frames;
                uint64_t t0 = time_get_us();
                while (g_scanning && g_frames == f0 && time_get_us() - t0 < 50000u) gdma_poll();
            }
            if (g_fb_pool[1]) {
                palloc_free(g_fb_pool[1], g_fb_pages);
                g_fb_pool[1] = NULL;
            }
            console_unlock();
            cprintf("lcd: double-buffering disabled\n");
            return;
        }
        cprintf("usage: lcd db [on|off]\n");
        return;
    }
    if (!g_up) { cprintf("lcd: not up -- %s\n", g_reason); return; }

    /* 47.7: the terminal's commands, as on the RP2350. */
    if (strncmp(args, "colours", 7) == 0 || strncmp(args, "colors", 6) == 0) {
        const char *h = skip_ws(args + (args[4] == 'u' ? 7 : 6));   /* "colours" vs "colors" */
        uint32_t v[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            h = skip_ws(h);
            for (; *h && *h != ' '; h++) {
                char c = *h;
                if (c >= '0' && c <= '9') v[k] = v[k] * 16u + (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') v[k] = v[k] * 16u + (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v[k] = v[k] * 16u + (uint32_t)(c - 'A' + 10);
            }
        }
        if (!g_t1) { cprintf("lcd: no terminal on the panel\n"); return; }
        term_colours((uint16_t)v[0], (uint16_t)v[1]);
        cprintf("lcd: fg 0x%04x bg 0x%04x, frame %s\n", (unsigned)(v[0] & 0xffffu),
                (unsigned)(v[1] & 0xffffu), g_fb_fmt == FB_RGB565 ? "RGB565" : "GRAY8");
        return;
    }
    if (strcmp(args, "termbench") == 0) {
        if (!g_t1) { cprintf("lcd: no terminal on the panel\n"); return; }
        term_bench();
        return;
    }
    if (strcmp(args, "repaint") == 0) {
        lcdterm_repaint();
        term_redraw();
        return;
    }
    if (strcmp(args, "test text") == 0) {
        if (lcdterm_text_test() != 0) cprintf("lcd test text: no terminal on the panel\n");
        return;
    }
    if (strncmp(args, "scroll", 6) == 0) {
        const char *q = skip_ws(args + 6);
        unsigned n = 0;
        while (*q >= '0' && *q <= '9') n = n * 10u + (unsigned)(*q++ - '0');
        lcdterm_scroll_test(n);
        return;
    }
    if (strncmp(args, "tee", 3) == 0) {
        const char *m = skip_ws(args + 3);
        if (strcmp(m, "uart") == 0) lcdterm_set_tee(LCD_TEE_UART);
        else if (strcmp(m, "off") == 0) lcdterm_set_tee(LCD_TEE_OFF);
        else if (*m) { cprintf("lcd tee: uart or off\n"); return; }
        cprintf("lcd tee: %s\n", lcdterm_tee() == LCD_TEE_OFF ? "off" : "uart (UART0)");
        return;
    }
    if (strcmp(args, "fps") == 0 || strncmp(args, "test", 4) == 0 || strcmp(args, "bench") == 0 ||
        strcmp(args, "verify") == 0 || strncmp(args, "wbtest", 6) == 0) {
        if (!g_fb_pool[0]) { cprintf("lcd: no frame buffer -- `lcd fb [gray|rgb565]` first\n"); return; }
    }
    if (strcmp(args, "fps") == 0) { cmd_fps(); return; }
    if (strcmp(args, "bench") == 0) { g_t_hold = (g_t1 != NULL); cmd_bench(); return; }
    if (strcmp(args, "verify") == 0) { cmd_verify(); return; }
    if (strncmp(args, "test", 4) == 0) {
        const char *p = skip_ws(args + 4);
        g_t_hold = (g_t1 != NULL);
        if (strcmp(p, "checker") == 0) draw(pat_checker);
        else if (strcmp(p, "grid") == 0) draw(pat_grid);
        else if (strcmp(p, "page") == 0) draw_text_page();   /* 47.5's; `test text` is the terminal's */
        else if (strcmp(p, "white") == 0) { fill(0xff); g_last_pat = NULL; }
        else if (strcmp(p, "black") == 0) { fill(0x00); g_last_pat = NULL; }
        else if (strcmp(p, "ramp") == 0) {
            for (unsigned y = 0; y < DSI_LCD_V_RES; y++)
                for (unsigned x = 0; x < DSI_LCD_H_RES; x++) px(x, y, (uint8_t)(x * 256u / DSI_LCD_H_RES));
            g_last_pat = NULL;
        } else { cprintf("usage: lcd test checker|grid|page|ramp|white|black|text\n"); return; }
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        if (g_double_buffered) dsi_lcd_flip(true);
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
        g_t_hold = (g_t1 != NULL);
        fill(0xff);
        dsi_lcd_fb_flush(0, DSI_LCD_V_RES);
        if (g_double_buffered) dsi_lcd_flip(true);
        task_sleep_ms(5000);
        uint8_t *front = fb_front();
        for (unsigned y = 150; y < 150 + 300 && y < DSI_LCD_V_RES; y++) {
            for (unsigned x = 312; x < 312 + 400 && x < DSI_LCD_H_RES; x++) {
                if (g_fb_fmt == FB_RGB565) ((uint16_t *)front)[y * DSI_LCD_H_RES + x] = 0;
                else front[y * DSI_LCD_H_RES + x] = 0;
            }
        }
        cprintf("lcd wbtest: black 400x300 box drawn, NOT written back -- look now (%u s)\n", secs);
        task_sleep_ms(secs * 1000u);
        fb_flush_buffer(front, 0, DSI_LCD_V_RES);
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
    if (strncmp(args, "orient", 6) == 0) {
        const char *p = skip_ws(args + 6);
        if (*p == '\0') {
            const char *mode_str = "0 deg (Portrait)";
            if (g_lcd_rot == 270) mode_str = "270 deg (Landscape)";
            else if (g_lcd_rot == 90) mode_str = "90 deg (Landscape Inverted)";
            else if (g_lcd_rot == 180) mode_str = "180 deg (Portrait Inverted)";
            cprintf("lcd orient: %s\n", mode_str);
            return;
        }
        int rot = -1;
        if (strcmp(p, "portrait") == 0 || strcmp(p, "0") == 0) rot = 0;
        else if (strcmp(p, "landscape") == 0 || strcmp(p, "270") == 0) rot = 270;
        else if (strcmp(p, "90") == 0) rot = 90;
        else if (strcmp(p, "180") == 0) rot = 180;
        else {
            cprintf("usage: lcd orient [portrait|landscape|0|90|180|270]\n");
            return;
        }
        orient_manual();
        if (dsi_lcd_set_orientation(rot) != 0) {
            cprintf("lcd orient: failed to set orientation %d\n", rot);
            return;
        }
        cprintf("lcd orient: set to %d deg (%s)\n", rot, (rot == 90 || rot == 270) ? "Landscape" : "Portrait");
        return;
    }
    cprintf("usage: lcd [colours FG BG | repaint | termbench | test text | scroll N | tee uart|off |\n"
            "            pattern bars|hbars|ber|off | fb [gray|rgb565] | db [on|off] |\n"
            "            orient [portrait|landscape|0|90|180|270] |\n"
            "            test checker|grid|page|ramp|white|black | fps | bench | verify |\n"
            "            wbtest [s] | bl on|off | cmd CC [PP..] | id | rd CC [N]]\n");
}

#endif
