#ifndef DRIVERS_ENC28J60_H
#define DRIVERS_ENC28J60_H

#include "net/netif.h"

/*
 * Microchip ENC28J60 Ethernet controller on SPI0 -- R4,
 * plan/phase19_ip_stack_and_ethernet.md.
 *
 * MAC + PHY, no TCP anywhere in it: it hands `netif_t` whole Ethernet
 * frames and nothing more, which is exactly the seam net/stack.c already
 * expects (unlike phase 18's cancelled W5500, which terminated TCP in
 * silicon and needed no seam at all).
 *
 * Board: a HanRun V823 HR911105A module (the common ten-pin SPI breakout,
 * magnetics-integrated RJ45) on the gateway persona's SPI0. Pin map in
 * cmake/board-rp2350-gateway.cmake and tests/hw/README.md's wiring section.
 *
 * Half-duplex only, forced -- the part has no auto-negotiation and no
 * auto-MDIX (plan §5): a straight cable to a switch, duplex fixed on both
 * ends. 10 Mbit is not a constraint worth minding at a 2 KB msize over an
 * SPI bus the kernel copies every frame through.
 */

/* Brings up SPI0 and the chip: reset, identity check (EREVID), MAC
 * programmed from the node's own identity, RX filter, half-duplex MAC/PHY
 * config, then registers a netif_t with net/netif.c. Safe to call before
 * the scheduler exists. Returns 0 on success, -1 if no chip answered. */
int enc28j60_init(void);

/* NULL until enc28j60_init() has succeeded. */
netif_t *enc28j60_get_netif(void);

/* Raw register dump (`net regs`), for bring-up: EIE/EIR/ESTAT/ECON1/ECON2,
 * EPKTCNT, ERXFCON, MACON1, and PHSTAT2 -- the chip's own account of
 * whether it thinks a frame arrived, independent of anything netif_t's
 * counters believe. */
void enc28j60_dump_regs(void);

#endif /* DRIVERS_ENC28J60_H */
