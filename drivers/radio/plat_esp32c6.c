/* The chip side of the Wi-Fi radio: power, clocks, reset, PHY (45.6,
 * plan/phase45_esp32c6.md). Behind drivers/radio/plat.h; runs in the radio's
 * U-mode domain, with the modem/PMU/PCR register windows granted.
 *
 * 45.6.1: stubs that say they were called. The real bodies are 45.6.2, written
 * against what the blob actually asks for rather than against a reading of
 * IDF's phy_init.c. */

#include "radio_redirect.h"
#include "osi_impl.h"
#include "plat.h"

#include "soc/soc.h"
#include "modem/reg_base.h"
#include "modem/modem_syscon_struct.h"
#include "modem/modem_lpcon_struct.h"
#include "hal/modem_syscon_ll.h"
#include "hal/modem_lpcon_ll.h"
#include "soc/pmu_struct.h"
#include "soc/pmu_reg.h"
#include "hal/pmu_ll.h"
#include "hal/clk_tree_ll.h"
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "phy_init_data.h"

#define CRUMB(n) (*(volatile uint32_t *)0x600B1000u = (n))   /* LP_AON store 0: how far a silent hang got (kernel/main.c) */
#define PLAT_TRACE(name) radio_osi_log_write(3, "plat", name)

/* The modem's clock gates (IDF: esp_hw_support/modem/modem_clock.c and
 * port/esp32c6/modem_clock_impl.c, minus the reference counts and the sleep
 * machinery -- this radio never sleeps). The register accessors are IDF's own
 * `*_ll.h` headers; what is ported is *which* of them, in which order. Done in
 * the radio's domain, which is granted the 64 KB at 0x600A0000 these live in. */
#define SYSCON ((modem_syscon_dev_t *)DR_REG_MODEM_SYSCON_BASE)
#define LPCON  ((modem_lpcon_dev_t *)DR_REG_MODEM_LPCON_BASE)

/* The modem clock domains sit behind clock-gating cells (ICG) whose enable maps the PMU
 * drives by power mode. IDF programs them twice: rtc_clk_init() before anything else
 * ("the PLL calibration needs the I2C master") and modem_clock_module_icg_map_init_all()
 * on each module enable. This chip was started by the ROM, which did neither -- and a
 * register behind a gated domain does not answer: the bus stalls and the core with it
 * (the first register_chipv7_phy call, 45.6). So: no gating in the active and modem
 * states, for every domain. */
#define ICG_ACTIVE (1u << PMU_HP_ICG_MODEM_CODE_ACTIVE)
#define ICG_MODEM  (1u << PMU_HP_ICG_MODEM_CODE_MODEM)
#define PMU_DEV    ((pmu_dev_t *)DR_REG_PMU_BASE)

static void modem_icg_init(void) {
    /* The modem's APB bus: clock on, reset released (PCR_MODEM_APB_CONF). Until then the
     * SYSCON/LPCON blocks read 0 and ignore writes -- the first thing the hardware says. */
    pmu_ll_hp_set_icg_modem(PMU_DEV, PMU_MODE_HP_ACTIVE, PMU_HP_ICG_MODEM_CODE_ACTIVE);
    modem_syscon_ll_set_modem_apb_icg_bitmap(SYSCON, ICG_ACTIVE);
    modem_lpcon_ll_set_i2c_master_icg_bitmap(LPCON, ICG_ACTIVE);
    modem_lpcon_ll_set_lp_apb_icg_bitmap(LPCON, ICG_ACTIVE);
    pmu_ll_imm_update_dig_icg_modem_code(PMU_DEV, true);
    pmu_ll_imm_update_dig_icg_switch(PMU_DEV, true);

    modem_syscon_ll_set_modem_apb_icg_bitmap(SYSCON, modem_syscon_ll_get_modem_apb_icg_bitmap(SYSCON) | ICG_ACTIVE | ICG_MODEM);
    modem_syscon_ll_set_modem_periph_icg_bitmap(SYSCON, modem_syscon_ll_get_modem_periph_icg_bitmap(SYSCON) | ICG_ACTIVE);
    modem_syscon_ll_set_wifi_icg_bitmap(SYSCON, modem_syscon_ll_get_wifi_icg_bitmap(SYSCON) | ICG_ACTIVE | ICG_MODEM);
    modem_syscon_ll_set_bt_icg_bitmap(SYSCON, modem_syscon_ll_get_bt_icg_bitmap(SYSCON) | ICG_ACTIVE);
    modem_syscon_ll_set_fe_icg_bitmap(SYSCON, modem_syscon_ll_get_fe_icg_bitmap(SYSCON) | ICG_ACTIVE | ICG_MODEM);
    modem_syscon_ll_set_ieee802154_icg_bitmap(SYSCON, modem_syscon_ll_get_ieee802154_icg_bitmap(SYSCON) | ICG_ACTIVE);
    modem_lpcon_ll_set_lp_apb_icg_bitmap(LPCON, modem_lpcon_ll_get_lp_apb_icg_bitmap(LPCON) | ICG_ACTIVE | ICG_MODEM);
    modem_lpcon_ll_set_i2c_master_icg_bitmap(LPCON, modem_lpcon_ll_get_i2c_master_icg_bitmap(LPCON) | ICG_ACTIVE | ICG_MODEM);
    modem_lpcon_ll_set_coex_icg_bitmap(LPCON, modem_lpcon_ll_get_coex_icg_bitmap(LPCON) | ICG_ACTIVE | ICG_MODEM);
    modem_lpcon_ll_set_wifipwr_icg_bitmap(LPCON, modem_lpcon_ll_get_wifipwr_icg_bitmap(LPCON) | ICG_ACTIVE | ICG_MODEM);
}

/* WIFI_CLOCK_DEPS = WIFI_MAC, WIFI_BB, COEXIST */
static void modem_wifi_clocks(bool en) {
    if (en) modem_icg_init();
    modem_syscon_ll_enable_wifi_apb_clock(SYSCON, en);
    modem_syscon_ll_enable_wifi_mac_clock(SYSCON, en);
    modem_syscon_ll_clk_wifibb_configure(SYSCON, en);
    modem_lpcon_ll_enable_coex_clock(LPCON, en);
}

/* esp_perip_clk_init(): the Wi-Fi power/timer block (TSF, beacon and sleep timing, and the
 * interrupts that come from it) runs from the modem's low-power clock, which follows the
 * system's slow clock -- the RC_SLOW oscillator -- unless told otherwise. The ROM leaves
 * every source deselected and the power clock off. */
static void modem_wifi_lpclk(void) {
    modem_lpcon_ll_enable_wifi_lpclk_slow_osc(LPCON, false);
    modem_lpcon_ll_enable_wifi_lpclk_fast_osc(LPCON, false);
    modem_lpcon_ll_enable_wifi_lpclk_32k_xtal(LPCON, false);
    modem_lpcon_ll_enable_wifi_lpclk_main_xtal(LPCON, false);
    modem_lpcon_ll_enable_wifi_lpclk_slow_osc(LPCON, true);
    modem_lpcon_ll_set_wifi_lpclk_divisor_value(LPCON, 0);
    modem_lpcon_ll_enable_wifipwr_clock(LPCON, true);
}

/* What IDF's boot has done before any Wi-Fi code runs: the modem clock domains ungated, the analog I2C
 * master clocked (the PHY libraries -- and `esp_wifi_set_mode` -- read and write analog registers
 * through it, and its read routine spins on a busy flag that never clears if it is not), the Wi-Fi
 * low-power clock chosen. A chip started by a RAM load happens to have most of it left over from
 * whatever ran before; a cold boot has none (found when the first cold `radio join` hung in the blob's
 * ram_chip_i2c_readReg_org, 45.7). Called once, before esp_wifi_init. */
/* IDF's rtc_clk_cpu_freq_set_config(160 MHz from the 480 MHz PLL), for a chip the boot ROM left on the
 * crystal: a flash boot runs the CPU at 40 MHz with the PLL off (PCR_SYSCLK_CONF.soc_clk_sel = XTAL; the
 * RAM loads these steps were first tried with had the PLL up because the ROM *downloader* wants it). The
 * Wi-Fi RF synthesizer is derived from the PLL, so with it off the PHY waits forever for lock
 * (ram_set_chan_freq_sw_start polling a PHY status bit). Needs the analog I2C master clocked, which
 * radio_plat_early_init() does first. M-mode, from the kernel's radio start. */
extern void rom_cpu_ticks(uint32_t mhz) __asm__("radio_ets_update_cpu_frequency");
void radio_plat_cpu_to_pll(void) {
    if (clk_ll_cpu_get_src() == SOC_CPU_CLK_SRC_PLL) return;
    clk_ll_bbpll_enable();
    clk_ll_bbpll_set_freq_mhz(480);
    clk_ll_bbpll_calibration_start();
    clk_ll_bbpll_set_config(480, 40);
    for (unsigned i = 0; i < 4000000u && !clk_ll_bbpll_calibration_is_done(); i++) { }
    for (volatile int i = 0; i < 2000; i++) { }
    clk_ll_bbpll_calibration_stop();
    clk_ll_cpu_set_hs_divider(CLK_LL_PLL_480M_FREQ_MHZ / 160);
    clk_ll_cpu_set_src(SOC_CPU_CLK_SRC_PLL);
    rom_cpu_ticks(160);
}

/* rtc_clk_init()'s oscillator tuning (RTC_CLK_CONFIG_DEFAULT): RC_FAST's frequency trim, RC_SLOW's
 * capacitor trim and RC32K's trim. A cold chip has the analog defaults, not these; RC_SLOW is the clock the
 * modem's time counter -- and so every PHY timeout ("pll_cal exceeds 2ms") -- is measured on. */
static void rtc_osc_tuning(void) {
    uint32_t v = *(volatile uint32_t *)0x600B0418u;                          /* LP_CLKRST_FOSC_CNTL */
    *(volatile uint32_t *)0x600B0418u = (v & ~(0x3ffu << 22)) | (100u << 22);
    v = *(volatile uint32_t *)0x600B041Cu;                                   /* LP_CLKRST_RC32K_CNTL */
    *(volatile uint32_t *)0x600B041Cu = (v & ~(0x3ffu << 22)) | (700u << 22);
    _regi2c_impl_write_mask(0x6D, 0, 14, 7, 0, 128);                         /* I2C_DIG_REG_SCK_DCAP */
}

void radio_plat_early_init(void) {
    /* rtc_clk_init(): the slow clock the modem's time counters run from (0x600AD000, which the PHY's
     * wait_i2c_sdm_stable() polls with a 10 ms timeout measured on it -- a counter that does not move
     * is a timeout that never expires): RC_FAST powered and distributed to the HP side, RC_SLOW
     * selected as the system's slow clock. */
    SET_PERI_REG_MASK(PMU_HP_SLEEP_LP_CK_POWER_REG, PMU_HP_SLEEP_XPD_FOSC_CLK);
    for (volatile int i = 0; i < 20000; i++) { }
    SET_PERI_REG_MASK(0x600B0420u, 1u << 31);                    /* LP_CLKRST_CLK_TO_HP: icg_hp_fosc */
    CLEAR_PERI_REG_MASK(0x600B0400u, 3u);                        /* LP_CLKRST_LP_CLK_CONF: slow_clk_sel = RC_SLOW */
    for (volatile int i = 0; i < 20000; i++) { }
    modem_icg_init();
    modem_wifi_lpclk();
    LPCON->i2c_mst_clk_conf.clk_i2c_mst_sel_160m = 1;
    LPCON->clk_conf.clk_i2c_mst_en = 1;
    rtc_osc_tuning();
    LPCON->clk_conf.clk_i2c_mst_en = 1;
    CRUMB(0x40);
}

void radio_plat_wifi_clock_enable(void)  {
    static bool lp_done;
    CRUMB(0x50);
    if (!lp_done) { modem_icg_init(); CRUMB(0x51); modem_wifi_lpclk(); CRUMB(0x52); lp_done = true; }
    modem_wifi_clocks(true);
}
void radio_plat_wifi_clock_disable(void) { modem_wifi_clocks(false); }
void radio_plat_wifi_reset_mac(void)     { modem_syscon_ll_reset_wifimac(SYSCON); }

/* PHY_CLOCK_DEPS (the front end) and PHY_CALIBRATION_CLOCK_DEPS (the Wi-Fi baseband),
 * as IDF's modem_clock_hal_enable_modem_{common,private}_fe_clock does for the C6. */
static void modem_phy_clocks(void) {
    modem_icg_init();
    LPCON->i2c_mst_clk_conf.clk_i2c_mst_sel_160m = 1;   /* as the second-stage bootloader does */
    LPCON->clk_conf.clk_i2c_mst_en = 1;           /* the analog I2C master the PHY programs the RF through */
    modem_syscon_ll_enable_fe_apb_clock(SYSCON, true);
    modem_syscon_ll_enable_fe_80m_clock(SYSCON, true);
    modem_syscon_ll_enable_fe_cal_160m_clock(SYSCON, true);
    modem_syscon_ll_enable_fe_160m_clock(SYSCON, true);
    modem_syscon_ll_clk_wifibb_configure(SYSCON, true);
    /* PHY_CALIBRATION_CLOCK_DEPS also names the BT/802.15.4 common baseband and the BT APB
     * bus: the calibration reads registers behind both, and a gated one does not answer. */
    modem_syscon_ll_enable_bt_clock(SYSCON, true);
    modem_syscon_ll_enable_bt_apb_clock(SYSCON, true);
    modem_syscon_ll_enable_modem_sec_apb_clock(SYSCON, true);
    /* The analog I2C bus to the SAR ADC and temperature sensor: reset, enable, release
     * (regi2c_ctrl_ll_i2c_sar_periph_enable). */
    CLEAR_PERI_REG_MASK(PMU_RF_PWC_REG, PMU_PERIF_I2C_RSTB);
    for (volatile int i = 0; i < 200; i++) { }
    SET_PERI_REG_MASK(PMU_RF_PWC_REG, PMU_XPD_PERIF_I2C);
    SET_PERI_REG_MASK(PMU_RF_PWC_REG, PMU_PERIF_I2C_RSTB);
}

/* IDF's pmu_init() -> esp_ocode_calib_init() (at a power-on reset): the RF block's resistor reference
 * ("O-code"), taken from the chip's eFuse and forced into the ULP analog block over the analog I2C
 * bus. Nothing of IDF's startup has run here, and the chip was power-cycled by a USB plug, not by us:
 * the value is whatever the analog block powered up with. 1 Mbit/s DSSS (beacons, the handshake) works
 * regardless; OFDM/HT data -- the DHCP exchange -- did not. */
extern void _regi2c_impl_write_mask(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t msb, uint8_t lsb, uint8_t data);
static void rf_ocode_from_efuse(void) {
    uint32_t ocode = (*(volatile uint32_t *)0x600B086Cu >> 9) & 0xffu;      /* EFUSE rd_sys_part1_data4.ocode */
    _regi2c_impl_write_mask(0x61, 0, 6, 7, 0, (uint8_t)ocode);              /* I2C_ULP_EXT_CODE */
    _regi2c_impl_write_mask(0x61, 0, 5, 6, 6, 1);                           /* I2C_ULP_IR_FORCE_CODE */
    radio_osi_log_write(3, "plat", "o-code %u from eFuse", (unsigned)ocode);
}

static bool g_phy_calibrated;
static volatile bool g_phy_on;
extern void phy_param_track_tot(bool en_wifi, bool en_ble_154);
extern void phy_init_param_set(uint8_t param);

/* esp_phy_enable() -> esp_phy_load_cal_and_init(), for the first enable, without the
 * calibration-data store (there is no NVS: every boot is a full calibration) and
 * without the sleep/retention variants. */
void radio_plat_phy_enable(void) {
    CRUMB(0x60);
    modem_phy_clocks();
    CRUMB(0x61);
    if (!g_phy_calibrated) {
        rf_ocode_from_efuse();
        CRUMB(0x62);
        radio_osi_log_write(3, "plat", "phy_version %s", get_phy_version_str());
        esp_phy_calibration_data_t *cal = radio_osi_zalloc(sizeof(*cal));
        if (!cal) { PLAT_TRACE("phy_enable: out of memory"); return; }
        radio_osi_read_mac(cal->mac, 0);
        phy_init_param_set(1);                 /* esp_phy_load_cal_and_init(), SOC_PHY_COMBO_MODULE: Wi-Fi's parameter set */
        int r = register_chipv7_phy(&phy_init_data, cal, PHY_RF_CAL_FULL);
        CRUMB(0x63);
        radio_osi_log_write(3, "plat", "register_chipv7_phy -> %d", r);
        radio_osi_free(cal);                   /* the PHY keeps its own copy */
        g_phy_calibrated = true;
    } else {
        phy_wakeup_init();                     /* bring the RF back after phy_close_rf() */
    }
    /* esp_phy_enable() ends with phy_module_disable(): the clocks only the calibration needs -- the BT/802.15.4
     * common baseband and the BT APB bus, which includes the modem security block's -- are released again;
     * only Wi-Fi's own stay on. Left running (as this did until 45.7), the board associated and completed the
     * 4-way handshake but no protected data frame got through in either direction (an IDF build on the same
     * board, register-diffed against this one, has exactly these three off). */
    modem_syscon_ll_enable_bt_clock(SYSCON, false);
    modem_syscon_ll_enable_bt_apb_clock(SYSCON, false);
    modem_syscon_ll_enable_modem_sec_apb_clock(SYSCON, false);
    phy_param_track_tot(true, false);          /* esp_phy_enable() -> phy_track_pll(): the first track is due at once */
    g_phy_on = true;
    phy_wifi_enable_set(1);                    /* esp_adapter.c's phy_enable_wrapper, after esp_phy_enable */
}

/* IDF's phy_track_pll_init() timer (phy_common.c): once a second while the PHY is on, the PHY re-tracks its
 * PLL. The radio's main thread calls this on that period. */
void radio_plat_phy_track(void) {
    if (g_phy_on) phy_param_track_tot(true, false);
}

void radio_plat_phy_disable(void) {
    g_phy_on = false;
    phy_wifi_enable_set(0);
    phy_close_rf();
    phy_xpd_tsens();
}
int  radio_plat_phy_update_country(const char *country) { (void)country; return 0; }


/* ---- what IDF's pmu_init.c / pmu_param.c ask of the rest of IDF ------------------------------------
 * They run in M-mode, from the kernel's radio start (kernel/radio_c6.c), before anything else touches the
 * chip. The reset reason is stated as power-on (this runs once per start, and it makes pmu_init do the
 * O-code step, which from eFuse is a few register writes); the register accessors are the open
 * regi2c_impl.c's. */
soc_reset_reason_t esp_rom_get_reset_reason(int cpu) { (void)cpu; return RESET_REASON_CHIP_POWER_ON; }
void esp_ocode_calib_init(void) { rf_ocode_from_efuse(); }
void regi2c_ctrl_write_reg_mask(uint8_t block, uint8_t host, uint8_t reg, uint8_t msb, uint8_t lsb, uint8_t data) {
    _regi2c_impl_write_mask(block, host, reg, msb, lsb, data);
}
extern uint8_t _regi2c_impl_read_mask(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t msb, uint8_t lsb);
uint8_t regi2c_ctrl_read_reg_mask(uint8_t block, uint8_t host, uint8_t reg, uint8_t msb, uint8_t lsb) {
    return _regi2c_impl_read_mask(block, host, reg, msb, lsb);
}

extern void _regi2c_impl_write(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t data);
void regi2c_ctrl_write_reg(uint8_t block, uint8_t host, uint8_t reg, uint8_t data) { _regi2c_impl_write(block, host, reg, data); }
