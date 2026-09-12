#ifndef DRIVERS_EMAC_ESP32P4_H
#define DRIVERS_EMAC_ESP32P4_H

#include <stdint.h>
#include <stdbool.h>

/* The ESP32-P4's Ethernet MAC, and the IP101GRI it drives over RMII.
 * Z1, plan/phase28_esp32p4_ethernet.md.
 *
 * At Z1 this is a bring-up surface and nothing more: it brings the clocks,
 * pads, PHY reset and station-management interface up far enough to talk to
 * the PHY over MDIO, and offers a scan so the PHY's address can be *measured*
 * rather than assumed. There is no netif_t here yet -- the descriptor rings
 * are Z2, the link is Z3, and frames are Z4.
 *
 * Every register address and bit position behind this header came from the
 * TRM's own bit diagrams cross-checked against ESP-IDF's generated headers.
 * The pin numbers are board facts and live in cmake/board-esp32p4-nano.cmake,
 * which explains at length why they are not interchangeable.
 */

/* Brings the MAC up to the point where MDIO works: enables the EMAC's clocks,
 * selects RMII, routes the pads, releases the PHY from reset, and performs
 * the MAC's software reset.
 *
 * Returns 0 on success, or a negative code identifying *which* step failed,
 * because "Ethernet does not work" is not a diagnosis (net/include/net/ip.h
 * makes the same argument about drop counters):
 *
 *   -1  the MAC's software reset never completed. Very likely the PHY is not
 *       supplying the 50 MHz RMII reference -- the reset cannot finish
 *       without it, by the DWC_EMAC's own specification. See the long comment
 *       in the implementation.
 *
 * Idempotent: calling it twice re-runs the sequence, which is what a bring-up
 * command wants.
 */
int emac_probe(void);

/* Clause-22 station management. `phy_addr` is 0..31, `reg` is 0..31.
 *
 * Reads return the 16-bit value, or -1 if the MAC stayed busy. A PHY that is
 * absent does NOT make these fail -- MDIO has no acknowledgement, so a read
 * from nothing returns 0xFFFF (the bus idles high) and the MAC reports
 * success. That is why identifying a PHY means looking at what came back, not
 * at whether the call succeeded. */
int emac_mdio_read(uint8_t phy_addr, uint8_t reg);
int emac_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val);

/* Reads registers 2 and 3 (PHYIDR1/PHYIDR2) at every address 0..31 and prints
 * a table of the ones that answered with something that is neither 0x0000 nor
 * 0xFFFF. Z1's done-condition: exactly one address should answer.
 *
 * Prints with cprintf() -- this is a table a human asked for, like
 * i2c_scan_bus(), not kernel log output. */
void emac_phy_scan(void);

#endif /* DRIVERS_EMAC_ESP32P4_H */
