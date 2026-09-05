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

/* Arms a reboot `ms` from now and returns; rp2350_reboot_cancel() withdraws
 * it. A deadman for anything that can hang the machine -- a bootrom reboot
 * preserves SRAM, so evidence written to a NOLOAD section survives it. */
bool rp2350_reboot_after_ms(uint32_t ms);
void rp2350_reboot_cancel(void);

/* Looks up a bootrom function by its two-character ROM table code (e.g.
 * ROM_TABLE_CODE('R','E') for flash_range_erase). Returns NULL if this build
 * has no bootrom or the code is not present on this silicon.
 *
 * Exported so drivers/flash_rp2350.c can reuse the RISC-V lookup rather than
 * copy it: the sequence is subtle in a way that is easy to get almost right
 * (the table entry *is* executable code on RISC-V, so the entry offset and
 * the RT_FLAG_FUNC_RISCV flag both differ from the Arm path), and it has
 * already been debugged once on real silicon. See bootrom.c for the details.
 *
 * Callers that will disable XIP must resolve every function they need
 * *before* doing so -- the lookup itself is safe (the ROM is not flash), but
 * resolving inside the critical section means more code that has to be
 * RAM-resident for no benefit. */
void *rp2350_rom_func_lookup(uint32_t code);

#endif /* LUGALOS_ARCH_RP2350_BOOTROM_H */
