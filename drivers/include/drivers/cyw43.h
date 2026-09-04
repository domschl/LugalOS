#ifndef DRIVERS_CYW43_H
#define DRIVERS_CYW43_H

#include <stdbool.h>
#include <stdint.h>

#include "net/netif.h"

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

/* The Pico 2 W's user LED is on the *wireless chip's* GPIO 0, not an
 * RP2350 pin, so this only works once cyw43_gspi_probe() has brought the
 * firmware up -- which is what makes it a genuine end-to-end check of the
 * whole stack rather than a GPIO poke. Returns false if the ioctl did not
 * complete. */
bool cyw43_led_set(bool on);

/* Associate with a WPA2-PSK network, using the *derived* 32-byte PSK
 * rather than a passphrase -- which is what this node stores (I6,
 * plan/phase21_identity_and_authentication.md). The firmware would accept
 * a passphrase and hash it itself; handing it the PMK is the same join
 * with one less secret at rest. Blocks until associated or the attempt
 * times out. */
bool cyw43_join_wpa2(const char *ssid, const uint8_t psk[32]);

/* True once the firmware is uploaded and answering ioctls -- i.e. once
 * cyw43_gspi_probe() has succeeded. Everything else here (join, the LED,
 * the netif) is meaningless before that, so callers can check rather than
 * discover it as a timeout. */
bool cyw43_is_ready(void);

/* NULL until cyw43_gspi_probe() has succeeded. */
netif_t *cyw43_get_netif(void);

/* Frames the driver had to drop because the previous one had not yet been
 * taken by the stack -- this driver keeps a single frame in flight. Its
 * own number, deliberately not netif_t's rx_dropped, which net/netif.c
 * owns and defines more narrowly. */
uint32_t cyw43_rx_overruns(void);

/* Deepest the receive ring has ever been. A high-water mark well below
 * its size says the ring is doing its job with room to spare; one that
 * sits at the size says it is the next thing to grow. */
uint32_t cyw43_rx_high_water(void);

/* Brings the radio up and joins, in the background, if -- and only if -- the
 * identity record carries WLAN credentials. Started once from kernel/main.c
 * on a board whose pin map declares the module; a no-op task that exits
 * immediately on a board with nothing stored.
 *
 * Runs as a task because the firmware upload takes tens of seconds and must
 * not hold the console; retries the join with a backoff and does not give
 * up, because the case that matters is a power cut where the router comes
 * back later than this board does. Returns the pid, or -1. */
/* Why the link is, or was last, not up -- and how strong it is when it is.
 *
 * Exists because the decoded drop reason used to live only in /proc/kmsg,
 * which is RAM: fetching it meant carrying the board to a cable, which unplugs
 * it, which clears the very thing being fetched. Three drops at one location
 * were written off as "unknown" for exactly that reason. These fields survive
 * the reconnection, so the incident report outlives the incident and can be
 * read over the network once the link is back.
 *
 * `rssi_dbm` is the last reading taken while associated and is deliberately
 * *not* cleared on a drop: what the signal was just before the link went away
 * is the most useful number here. -30 is next to the AP; past about -75 a
 * 2.4 GHz link starts losing frames. */
typedef struct {
    bool     link_up;
    int32_t  rssi_dbm;
    bool     rssi_valid;
    uint32_t drops;              /* link-down transitions since boot */
    uint32_t joins;              /* successful joins since boot */
    uint32_t last_drop_ev;       /* firmware event type that dropped it */
    uint32_t last_drop_status;
    uint32_t last_drop_reason;
    uint32_t s_since_drop;
    uint32_t s_since_up;
} cyw43_link_status_t;

void cyw43_link_status(cyw43_link_status_t *out);

int cyw43_autostart_task_start(void);

#endif /* DRIVERS_CYW43_H */
