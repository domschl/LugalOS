#ifndef LUGALOS_RADIO_PLAT_H
#define LUGALOS_RADIO_PLAT_H

#include <stdint.h>

/* What the Wi-Fi blob's OS table asks of the *chip*, rather than of an OS: the
 * power, clock and reset state of the radio, and the PHY's calibration (45.6,
 * plan/phase45_esp32c6.md).
 *
 * These are the entries behind which Espressif's open code sits -- esp_phy's
 * esp_phy_enable(), modem_clock_module_*(), the PMU setup -- about 5 000 lines
 * that a chip started from ROM has not had run for it (plan §4.4). They are
 * declared here so the OS table (drivers/radio/esp32c6_osi_table.c) is complete
 * and compiles now; the C6 port provides them, in the radio domain, with the
 * register windows PCR, PMU and the modem block granted. The QEMU test
 * provides stubs. */

void radio_plat_phy_enable(void);                 /* esp_phy_enable(PHY_MODEM_WIFI); phy_wifi_enable_set(1) */
void radio_plat_phy_disable(void);                /* phy_wifi_enable_set(0); esp_phy_disable(PHY_MODEM_WIFI) */
int  radio_plat_phy_update_country(const char *country);   /* esp_phy_update_country_info() */
void radio_plat_wifi_reset_mac(void);             /* modem_clock_module_mac_reset(PERIPH_WIFI_MODULE) */
void radio_plat_wifi_clock_enable(void);          /* wifi_module_enable() */
void radio_plat_wifi_clock_disable(void);         /* wifi_module_disable() */

#endif
