#ifndef LUGALOS_ARCH_ATOMIC_H
#define LUGALOS_ARCH_ATOMIC_H

#include <stdint.h>
#include <stdbool.h>

/* The one hart-count-sensitive operation in the locking layer (S1/§6.3,
 * plan/phase22_smp_locking_foundation.md).
 *
 * ## Why this is a seam and not just an inline `amoswap`
 *
 * `kernel/lock.c` needs exactly one thing the C language cannot give it: a
 * read-modify-write that no other hart can interleave with. On RISC-V that is
 * the A extension, which every target in this tree already compiles with
 * (`-march=rv32imac_zicsr_zbs` / `rv64gc`, CMakeLists.txt) -- so the obvious
 * move is to write `amoswap.w` at the one call site and be done.
 *
 * The plan's §6.3 asks for this seam instead, and the reason is worth
 * keeping: "RISC-V atomics are defined identically whether or not a second
 * hart is listening" is a statement about the *ISA*, and this project's
 * standing rule is that Hazard3 is not assumed to match the ISA until it has
 * been measured against it -- six divergences found that way so far. RP2350
 * additionally ships 32 SIO hardware spinlocks, which exist precisely because
 * cross-core exclusion is something that silicon offers directly.
 *
 * So the shape here is "try to take this lock word / release it" rather than
 * "swap this integer": a SIO-spinlock implementation can map a word to a
 * hardware lock index, which a bare `amoswap` hook could not express. If
 * phase 23's X3 finds that Hazard3's AMOs do not do what the ISA says across
 * two live cores, the fix is this file and nothing else -- not one call site
 * in kernel/, fs/ or drivers/.
 *
 * Nothing here is conditional today: every target gets the AMO version, and
 * on a single hart it behaves exactly as the plain load/store it replaces.
 * That is the point of landing it before a second core exists.
 */

/* Attempts to take `word`, which is 0 when free and 1 when held.
 *
 * Returns true only for the caller that moved it from 0 to 1 -- so at most
 * one caller, on any hart, can see true for a given transition.
 *
 * `.aq` (acquire) on the swap: no load or store *after* this in program order
 * may be observed by another hart before the swap is. Without it the compiler
 * and the hardware are both free to hoist the critical section's first read
 * above the acquire, which is the whole thing the lock exists to prevent. */
static inline bool arch_lock_try_acquire(volatile uint32_t *word) {
    uint32_t prev;
    __asm__ __volatile__("amoswap.w.aq %0, %2, (%1)"
                         : "=&r"(prev)
                         : "r"(word), "r"((uint32_t)1)
                         : "memory");
    return prev == 0;
}

/* Releases `word`. Only the holder may call this.
 *
 * `.rl` (release) pairs with the `.aq` above: everything the critical section
 * wrote is visible to whichever hart acquires next, before it can observe the
 * lock as free. A plain `*word = 0` would compile to a store the hardware may
 * reorder ahead of the section's own writes, handing the next holder a lock
 * over memory that has not landed yet. */
static inline void arch_lock_release(volatile uint32_t *word) {
    __asm__ __volatile__("amoswap.w.rl zero, zero, (%0)"
                         :: "r"(word) : "memory");
}

#endif /* LUGALOS_ARCH_ATOMIC_H */
