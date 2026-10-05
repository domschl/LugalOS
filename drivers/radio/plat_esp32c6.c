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

#define PLAT_TRACE(name) radio_osi_log_write(3, "plat", name)

/* The modem's clock gates (IDF: esp_hw_support/modem/modem_clock.c and
 * port/esp32c6/modem_clock_impl.c, minus the reference counts and the sleep
 * machinery -- this radio never sleeps). The register accessors are IDF's own
 * `*_ll.h` headers; what is ported is *which* of them, in which order. Done in
 * the radio's domain, which is granted the 64 KB at 0x600A0000 these live in. */
#define SYSCON ((modem_syscon_dev_t *)DR_REG_MODEM_SYSCON_BASE)
#define LPCON  ((modem_lpcon_dev_t *)DR_REG_MODEM_LPCON_BASE)

/* WIFI_CLOCK_DEPS = WIFI_MAC, WIFI_BB, COEXIST */
static void modem_wifi_clocks(bool en) {
    modem_syscon_ll_enable_wifi_apb_clock(SYSCON, en);
    modem_syscon_ll_enable_wifi_mac_clock(SYSCON, en);
    modem_syscon_ll_clk_wifibb_configure(SYSCON, en);
    modem_lpcon_ll_enable_coex_clock(LPCON, en);
}

void radio_plat_wifi_clock_enable(void)  { modem_wifi_clocks(true); }
void radio_plat_wifi_clock_disable(void) { modem_wifi_clocks(false); }
void radio_plat_wifi_reset_mac(void)     { modem_syscon_ll_reset_wifimac(SYSCON); }

/* 45.6.2, next: the PHY (calibration, register_chipv7_phy) and the modem power domain. */
void radio_plat_phy_enable(void)        { PLAT_TRACE("phy_enable (not yet)"); }
void radio_plat_phy_disable(void)       { PLAT_TRACE("phy_disable (not yet)"); }
int  radio_plat_phy_update_country(const char *country) { (void)country; return 0; }
