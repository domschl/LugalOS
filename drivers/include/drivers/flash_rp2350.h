#ifndef LUGALOS_DRIVERS_FLASH_RP2350_H
#define LUGALOS_DRIVERS_FLASH_RP2350_H

#include <stdint.h>

/* Erases one 4 KB sector of the RP2350's internal flash and programs 4 KB
 * into it. `flash_offs` is an offset from the start of flash, NOT an XIP
 * address -- 0x3FF000 for the last sector, not 0x103FF000 -- matching the
 * bootrom's own convention. `data` must point at 4096 readable bytes.
 *
 * Returns 0, or -1 if the offset is not sector-aligned, `data` is NULL, the
 * bootrom's flash functions could not be resolved, or this is not an RP2350
 * build.
 *
 * Runs with interrupts disabled and XIP turned off for its duration, so it is
 * not something to call on a hot path: everything else on the core stops,
 * including the scheduler tick. It is written for the identity store's
 * occasional, deliberate writes (I7b).
 *
 * **Leaves XIP in the bootrom's generic 03h read mode**, which is slower than
 * the mode the boot sequence set up, until the next reset. That is the
 * SDK's own non-RP2040 path (`flash_enable_xip_via_boot2()` resolves to
 * ROM_FUNC_FLASH_ENTER_CMD_XIP there) and it is a fair trade for a rare
 * operation, but it is why callers tell the user a reboot is worth it.
 */
int flash_rp2350_write_sector(uint32_t flash_offs, const void *data);

#endif /* LUGALOS_DRIVERS_FLASH_RP2350_H */
