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
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "phy_init_data.h"

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

void radio_plat_wifi_clock_enable(void)  {
    static int once;
    if (!once++) {
        radio_osi_log_write(3, "plat", "before: syscon04=%08x 0c=%08x pcr108=%08x", *(volatile uint32_t *)0x600A9804,
                            *(volatile uint32_t *)0x600A980C, *(volatile uint32_t *)0x60096108);
        modem_wifi_clocks(true);
        radio_osi_log_write(3, "plat", "readback: mac=%d apb=%d bb=%d coex=%d icg_wifi=%x", modem_syscon_ll_wifi_mac_clock_is_enabled(SYSCON),
                            modem_syscon_ll_wifi_apb_clock_is_enabled(SYSCON), modem_syscon_ll_wifibb_clock_is_enabled(SYSCON),
                            modem_lpcon_ll_coex_clock_is_enabled(LPCON), modem_syscon_ll_get_wifi_icg_bitmap(SYSCON));
        radio_osi_log_write(3, "plat", "after: pmu0c=%08x syscon00=%08x lpcon10=%08x", *(volatile uint32_t *)0x600B000C,
                            *(volatile uint32_t *)0x600A9800, *(volatile uint32_t *)0x600AF010);
        return;
    }
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

static bool g_phy_calibrated;

/* esp_phy_enable() -> esp_phy_load_cal_and_init(), for the first enable, without the
 * calibration-data store (there is no NVS: every boot is a full calibration) and
 * without the sleep/retention variants. */
void radio_plat_phy_enable(void) {
    if (g_phy_calibrated) { PLAT_TRACE("phy_enable (already calibrated)"); return; }
    PLAT_TRACE("phy_enable: clocks");
    modem_phy_clocks();
    PLAT_TRACE("phy_enable: clocks on");
    radio_osi_log_write(3, "plat", "phy_version %s", get_phy_version_str());
    esp_phy_calibration_data_t *cal = radio_osi_zalloc(sizeof(*cal));
    if (!cal) { PLAT_TRACE("phy_enable: out of memory"); return; }
    radio_osi_read_mac(cal->mac, 0);
    int r = register_chipv7_phy(&phy_init_data, cal, PHY_RF_CAL_FULL);
    radio_osi_log_write(3, "plat", "register_chipv7_phy -> %d", r);
    radio_osi_free(cal);                       /* the PHY keeps its own copy */
    g_phy_calibrated = true;
}
void radio_plat_phy_disable(void)       { PLAT_TRACE("phy_disable (not yet)"); }
int  radio_plat_phy_update_country(const char *country) { (void)country; return 0; }
