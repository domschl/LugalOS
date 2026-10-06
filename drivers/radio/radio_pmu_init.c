/* IDF's pmu_init.c, unmodified (45.7).
 *
 * pmu_init() programs the power management unit's three modes -- active, modem, sleep -- including the
 * clock-power flags (is the PLL's I2C bus, the PLL and the crystal powered in each mode) that the Wi-Fi RF
 * needs: with the first form of this file those setters were disabled for fear of switching the system
 * clock under a kernel that executes from flash, and the PHY's PLL calibration then timed out ("pll_cal
 * exceeds 2ms"). The fear was misplaced -- the illegal instruction it was guarding against was a missing
 * .iram1 section in the flash image -- and by the time this runs radio_plat_cpu_to_pll() has put the CPU on
 * the PLL, the state IDF's own parameters describe. A wrapper rather than the file itself so that it is
 * placed and compiled with the radio's other open code.
 */

#include "pmu_init.c"      /* IDF's, found through the include path (cmake/radio_esp32c6.cmake) */
