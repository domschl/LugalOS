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

#define PLAT_TRACE(name) radio_osi_log_write(3, "plat", name " (stub)")

void radio_plat_phy_enable(void)        { PLAT_TRACE("phy_enable"); }
void radio_plat_phy_disable(void)       { PLAT_TRACE("phy_disable"); }
int  radio_plat_phy_update_country(const char *country) { (void)country; PLAT_TRACE("phy_update_country"); return 0; }
void radio_plat_wifi_reset_mac(void)    { PLAT_TRACE("wifi_reset_mac"); }
void radio_plat_wifi_clock_enable(void) { PLAT_TRACE("wifi_clock_enable"); }
void radio_plat_wifi_clock_disable(void){ PLAT_TRACE("wifi_clock_disable"); }
