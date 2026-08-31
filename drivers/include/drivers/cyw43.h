#ifndef DRIVERS_CYW43_H
#define DRIVERS_CYW43_H

#include <stdbool.h>

/*
 * Infineon/Cypress CYW43439 wireless chip on the Pico 2 W's internal gSPI
 * bus -- R5, plan/phase19_ip_stack_and_ethernet.md.
 *
 * Unlike R4's ENC28J60, this part terminates nothing itself: it is a bus
 * (gSPI, bit-banged over RP2350's PIO0 because the wire protocol needs a
 * single bidirectional data line with a mid-transfer direction flip, which
 * no hardware SPI controller does) carrying a register/backplane windowing
 * protocol, on top of which sits a ~230 KB firmware blob the chip itself
 * runs, an ioctl layer, and only then a netif_t worth of Ethernet frames.
 * Milestone 1 (this header, cyw43_gspi_probe()) is the bus layer only: can
 * anything be read back over PIO0 at all. Firmware download, join, and the
 * netif_t seam are later milestones, not yet built.
 *
 * Board: Pico 2 W's own module, GP23/24/25/29 (WL_ON/WL_D/WL_CS/WL_CLK),
 * internal to the module on every board that carries one -- pin map in
 * cmake/board-rp2350-wifi.cmake.
 */

/* Brings up PIO0 SM0 as the gSPI bus and confirms the chip answers: resets
 * the module via WL_ON, then reads the well-known SPI_READ_TEST_REGISTER
 * pattern (0xFEEDBEAD) using the chip's byte-swapped-32-bit bring-up
 * command form. Safe to call before the scheduler exists. Returns true if
 * the test pattern came back correctly, false otherwise -- does not touch
 * firmware, NVRAM, or anything past the raw bus. */
bool cyw43_gspi_probe(void);

#endif /* DRIVERS_CYW43_H */
