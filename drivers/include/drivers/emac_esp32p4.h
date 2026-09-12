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

/* Z2's done-condition: sends frames of several sizes through the MAC's
 * internal loopback and checks each comes back byte-identical. No PHY and no
 * cable are involved, deliberately -- a failure here can only be the
 * descriptor rings, the cache maintenance, or the buffer ownership protocol.
 * Reports the .bss the rings cost and the cache line size they are built
 * around. Leaves loopback disabled. */
void emac_loopback_test(void);

/* --- Z3: link state ----------------------------------------------------- */

typedef struct {
    bool     up;            /* carrier present */
    bool     an_complete;   /* auto-negotiation finished; false means parallel detect */
    uint16_t speed_mbit;    /* 10 or 100; meaningless when !up */
    bool     full_duplex;
} emac_link_t;

/* Advertises 10/100 half/full and restarts auto-negotiation. Returns 0, or -1
 * if the PHY did not answer. */
int emac_phy_autoneg_start(void);

/* Reads link state and, when it has changed, applies the negotiated speed and
 * duplex to *both* the MAC and the RMII clock divisors -- they live in
 * different peripherals and setting one without the other corrupts rather
 * than fails. Returns whether the link is up; fills `out` when non-NULL.
 *
 * Never blocks in the scheduler's sense (no task_block(), no lock), which is
 * what netif_t's link_up() requires. Rate-limited internally to one MDIO
 * exchange per 200 ms, so it is cheap to call from a polling loop. */
bool emac_link_poll(emac_link_t *out);

/* Z3's done-condition: brings the MAC up, negotiates, and reports what was
 * agreed, or that there is no carrier. */
void emac_link_report(void);

/* Z3's other half, and the one a hardcoded `return true` would fail: drops
 * the PHY with BMCR's POWERDOWN bit (indistinguishable from a pulled cable as
 * far as the MAC can see), checks the link goes down, brings it back, and
 * reports how long each direction took. Needs no human to unplug anything, so
 * it can live in the hardware suite. */
void emac_link_updown_test(void);

#endif /* DRIVERS_EMAC_ESP32P4_H */
