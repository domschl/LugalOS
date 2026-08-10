#ifndef LUGALOS_ARCH_RP2350_BOOTROM_H
#define LUGALOS_ARCH_RP2350_BOOTROM_H

#include <stdint.h>
#include <stdbool.h>

/* Reboots into BOOTSEL (USB mass-storage) mode via the RP2350 bootrom, so a
 * host can flash a new UF2 without physically holding the BOOTSEL button.
 *
 * Used by drivers/usb_cdc.c to implement the "1200-baud touch" convention.
 * Does not return on success; returns false only if the bootrom function
 * could not be found (or on a non-RP2350 build). */
bool rp2350_reboot_to_bootsel(void);

/* Restarts the board, coming back up running the flashed image (as opposed to
 * the BOOTSEL path above, which comes back as a mass-storage device).
 *
 * Exists so a test run can leave the board in a known state without anyone
 * touching it: tests/hw/'s node-pool test deliberately ends with the Lisp
 * evaluator inert, and everything else in that suite drives the board through
 * a shell that evaluates. Does not return on success; returns false if the
 * bootrom function could not be found, or on a non-RP2350 build. */
bool rp2350_reboot(void);

#endif /* LUGALOS_ARCH_RP2350_BOOTROM_H */
