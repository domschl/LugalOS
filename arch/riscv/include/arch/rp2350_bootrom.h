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

/* The bootrom maps only the booted partition at 0x10000000. This points the
 * QMI's spare address-translation windows at the filesystem and the identity
 * sector (cmake/flash_layout.cmake) so they can be read through
 * LUGALOS_FLASHFS_BASE and LUGALOS_IDENTITY_BASE. Before the first read of
 * either; a no-op where the flash map has no such windows. */
void rp2350_map_data_windows(void);

/* The A/B slot the bootrom booted: 0 or 1, or -1 if this boot was not from
 * the A/B pair (a RAM image, a BOOTSEL-loaded binary) or the ROM did not say. */
int rp2350_boot_slot(void);
/* This boot was entered under try-before-you-buy and rp2350_explicit_buy() has
 * not yet been made (datasheet 5.1.17). */
bool rp2350_boot_buy_pending(void);
/* Restarts into the image at flash storage offset `phys`, a "flash update"
 * boot: preferred whatever the versions, and a try-before-you-buy image only
 * boots this way. Does not return on success. */
bool rp2350_reboot_flash_update(uint32_t phys);

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
