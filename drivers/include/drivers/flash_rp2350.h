#ifndef LUGALOS_DRIVERS_FLASH_RP2350_H
#define LUGALOS_DRIVERS_FLASH_RP2350_H

#include <stdint.h>

/* Erases one 4 KB sector of the RP2350's internal flash and programs 4 KB
 * into it. `flash_offs` is an offset from the start of flash, NOT an XIP
 * address -- 0x3FF000 for the last sector, not 0x103FF000 -- matching the
 * bootrom's own convention. `data` must point at 4096 bytes of SRAM: not
 * flash, which is switched off while it is read, and not PSRAM, which shares
 * the same interface (H2, plan/phase38_psram.md §2).
 *
 * Returns 0, or -1 if the offset is not sector-aligned, `data` is NULL or
 * outside SRAM, the bootrom's flash functions could not be resolved, core 1
 * did not park, or this is not an RP2350 build.
 *
 * Runs with interrupts disabled and XIP turned off for its duration, so it is
 * not something to call on a hot path: everything else on the core stops,
 * including the scheduler tick. It is written for the identity store's
 * occasional, deliberate writes (I7b).
 *
 * **Leaves the QSPI interface exactly as it found it** (38.1). The bootrom's
 * sequence resets both QMI windows to its own serial 03h read at a slow
 * divider; until 38.1 flash stayed that way until the next reset (~46 %
 * slower code), and PSRAM on window 1 answered garbage and lost every dirty
 * cache line. The XIP cache is now cleaned before, and every window
 * register, XIP_CTRL and the QSPI pads are put back after.
 */
int flash_rp2350_write_sector(uint32_t flash_offs, const void *data);

/* The `xipcycle` instrument (38.1): runs the same RAM-resident sequence as a
 * write -- XIP off, the bootrom's re-entry, the restore -- with the erase and
 * program left out, and reports the QMI registers and a timed uncached flash
 * read before and after. Nothing is written, and interrupts are off for
 * microseconds rather than the ~60 ms an erase takes, so the USB console
 * survives it. */
void flash_rp2350_selftest(void);

/* The same sequence silently: 0, or -1 if it could not run. For tests that
 * need a flash write's effect on the QSPI windows without a write (38.2's
 * `psram test`: do dirty PSRAM lines survive it?). */
int flash_rp2350_xip_cycle(void);

/* One line per QMI window for `clocks`: TIMING/RFMT/RCMD and what they mean
 * (SCK divider, read command, data width). */
void flash_rp2350_qmi_report(void);

#endif /* LUGALOS_DRIVERS_FLASH_RP2350_H */
