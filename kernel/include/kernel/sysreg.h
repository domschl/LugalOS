#ifndef LUGALOS_KERNEL_SYSREG_H
#define LUGALOS_KERNEL_SYSREG_H

#include <stdbool.h>
#include <stdint.h>
#include "lugalos_config.h"

/* Shared system registers: words that hold bits for several drivers -- the
 * ESP32-P4's clock gates and reset lines (HP_SYS_CLKRST, LP_CLKRST) above all.
 * A read-modify-write of one from two tasks (the panel coming up on the first
 * `lcd`, the LLP task bringing up the SD host for the C6, `smpstart`) loses one
 * of the updates: a gate closed again, a reset left asserted. Every
 * read-modify-write of such a word goes through sysreg_update(), which holds
 * one spinlock for it; plain stores of a whole word are not affected.
 *
 * The RP2350 has no such words in use: its peripherals take atomic set/clear
 * aliases (+0x2000/+0x3000), which drivers write directly. The C6's PCR has
 * one word per peripheral. */

/* *addr = (*addr & ~clear) | set, as one step against every other caller.
 * Callable from any context, before the scheduler, and from a handler. */
void sysreg_update(uintptr_t addr, uint32_t clear, uint32_t set);

/* For an update that is several statements (compute, then store): hold the
 * same lock around it. Call nothing that takes another spinlock inside. */
uintptr_t sysreg_lock(void);
void      sysreg_unlock(uintptr_t flags);

static inline void sysreg_set(uintptr_t addr, uint32_t bits)   { sysreg_update(addr, 0, bits); }
static inline void sysreg_clear(uintptr_t addr, uint32_t bits) { sysreg_update(addr, bits, 0); }

/* Whether `addr` is in a block of shared words (constant-folds for the usual
 * constant address): the drivers' field_set() helpers route through
 * sysreg_update() when it is. */
static inline bool sysreg_is_shared(uintptr_t addr) {
#if defined(CONFIG_BOARD_ESP32P4)
    return (addr >= 0x500E6000u && addr < 0x500E7000u) ||   /* HP_SYS_CLKRST */
           (addr >= 0x50111000u && addr < 0x50112000u);     /* LP_CLKRST */
#else
    (void)addr;
    return false;
#endif
}

#endif /* LUGALOS_KERNEL_SYSREG_H */
