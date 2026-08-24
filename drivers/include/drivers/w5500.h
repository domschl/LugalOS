#ifndef DRIVERS_W5500_H
#define DRIVERS_W5500_H

#include <stdint.h>
#include <stdbool.h>

/*
 * WIZnet W5500 Ethernet controller on SPI0 — N4,
 * plan/phase18_networking_and_auth.md.
 *
 * The part that makes this phase small: the W5500 contains the TCP/IP stack
 * in silicon. It terminates TCP, answers ARP and ICMP, retransmits, and hands
 * this side a byte stream over SPI. LugalOS therefore has no IP stack, and
 * the 9P server sees exactly what it sees over USB-CDC or a UART — a
 * `p9_link_t` carrying length-prefixed frames.
 *
 * Board: a USR-ES1 module (W5500 behind a HanRun HR961160C magjack) on the
 * gateway persona's SPI0, pins in cmake/board-rp2350-gateway.cmake and the
 * module's own header map in the plan's §2.
 */

/* Brings up SPI0 and the chip: hardware reset, identity check, MAC and
 * address, then socket 0 listening on the 9P port. Safe to call before the
 * scheduler exists. Returns 0 on success.
 *
 * Fails loudly rather than silently on the two things that are worth
 * distinguishing: an unreadable VERSIONR (the bus is wrong -- wiring, power,
 * or ACCESSCTRL) and an unconfigured address (nobody told this board who it
 * is; see w5500_set_address()). */
int w5500_init(void);

/* True once VERSIONR has read back the W5500's own 0x04 -- i.e. the SPI bus
 * demonstrably works. Everything else in this header is meaningless until
 * this is true, which is why it is separate from "is the cable plugged in". */
bool w5500_present(void);

/* PHY link state, straight from PHYCFGR. False means no cable, no switch, or
 * a switch port that is down -- this is the question `net` exists to answer
 * without a logic analyser. */
bool w5500_link_up(void);

/* The address this board answers to. Set from (net-config ...) on the SD
 * card, or from the board file's fallback if it has one; see the plan's §3
 * for why an unconfigured gateway stays off the network and says so rather
 * than picking a default and causing an address conflict on a network it
 * knows nothing about.
 *
 * Each argument is four bytes, network order. `gw` may be all zeros on an
 * isolated segment with no router. Applying an address re-runs the socket
 * setup, so it is safe to call on a running gateway. */
int  w5500_set_address(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4]);
bool w5500_have_address(void);

/* The MAC. Locally administered (first octet 0x02); see w5500_rp2350.c on why
 * it is a configured constant rather than derived from silicon, and what that
 * costs if two of these ever share a LAN. */
void w5500_set_mac(const uint8_t mac[6]);

/* One line per fact, for `net` and /proc/net: presence, link, MAC, address,
 * socket state and the byte counters. */
void w5500_report(void);

/* The 9P transport over socket 0, for kernel/board.c's device table -- NULL
 * until the chip is present AND an address has been configured, because a
 * link that cannot carry anything should not be registered as one. It sets
 * `auth_required`: this is the only wire in this tree that is a network. */
struct p9_link;
struct p9_link *w5500_get_link(void);

/* Bring-up tools -- see the definitions for what question each answers.
 * `mode` is auto|100f|100h|10f|10h|down; NULL means auto. */
int  w5500_phy_mode(const char *mode);
void w5500_watch(unsigned secs);

/* Log every socket state transition as it happens (bring-up aid). */
void w5500_debug(bool on);

/* Does the chip still receive? Puts socket 0 into MACRAW for `secs` and counts
 * every frame on the wire, ARP included -- the only measurement that separates
 * a dead transmitter from a dead receiver. Drops any attached peer and
 * restores the 9P socket afterwards; see the definition. */
void w5500_rxtest(unsigned secs);

/* Does the chip still transmit? Sends `count` gratuitous ARPs from MACRAW --
 * broadcast, so nothing has to be learned or cached at the far end -- and
 * reports what the chip's own SEND_OK says. The mirror of w5500_rxtest();
 * between them a silent board is either transmitting or it is not. */
void w5500_txtest(unsigned count);

/* Is the SPI bus delivering what it is told? Reads a constant register and
 * round-trips a pattern through an unused one, at four clock rates, and
 * counts the mismatches. Decides whether a wedged chip is a wiring problem
 * or a logic one -- see the definition. */
void w5500_bustest(unsigned iterations);

#endif /* DRIVERS_W5500_H */
