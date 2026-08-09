#ifndef LUGALOS_KERNEL_IRQ_H
#define LUGALOS_KERNEL_IRQ_H

#include <stdint.h>
#include "arch/csr.h"

/* Interrupt-masking critical sections (B6, plan/phase5_distributed_design.md §5.4).
 *
 * Every "no locking needed" note in this kernel -- kernel/palloc.h,
 * kernel/sched.h, kernel/chan.h, kernel/klog.h -- rests on the same premise:
 * scheduling is cooperative, so nothing can run between two instructions
 * unless the code itself yields. Timer preemption removes that premise
 * wholesale. A task can then be suspended midway through updating the
 * allocator bitmap or the task table, and the next task can observe the
 * half-written state.
 *
 * These are deliberately landed *before* preemption exists, where they are
 * behaviour-preserving no-ops: disabling an interrupt that never fires
 * changes nothing, so the whole existing suite is a regression check on the
 * placement of the critical sections before anything can actually interrupt.
 * Getting that ordering backwards would mean debugging the timer and the
 * locking at once.
 *
 * Masking interrupts is the right primitive here rather than a mutex: the
 * regions are a handful of instructions, there is one hart, and a task that
 * blocks while holding a scheduler lock could not be scheduled out of it.
 * Multi-hart would need something else, and nothing in this kernel is
 * multi-hart (entry.S parks every secondary hart at boot).
 *
 * Rule 0 (§5.1): identical structure on both builds; only the CSR and its
 * enable bit differ.
 */

#if defined(CONFIG_MODE_S)
#define IRQ_ENABLE_BIT (1UL << 1)   /* sstatus.SIE */
#else
#define IRQ_ENABLE_BIT (1UL << 3)   /* mstatus.MIE */
#endif

/* Disables interrupts and returns the previous enable state. Nests correctly:
 * an inner save/restore pair cannot re-enable interrupts that an outer one
 * had disabled, because it restores what it actually found rather than
 * unconditionally enabling. */
static inline uintptr_t irq_save(void) {
    uintptr_t prev;
#if defined(CONFIG_MODE_S)
    __asm__ __volatile__("csrrc %0, sstatus, %1"
                         : "=r"(prev) : "r"(IRQ_ENABLE_BIT) : "memory");
#else
    __asm__ __volatile__("csrrc %0, mstatus, %1"
                         : "=r"(prev) : "r"(IRQ_ENABLE_BIT) : "memory");
#endif
    return prev & IRQ_ENABLE_BIT;
}

static inline void irq_restore(uintptr_t flags) {
    if (!flags) return; /* they were already off; leave them off */
#if defined(CONFIG_MODE_S)
    __asm__ __volatile__("csrs sstatus, %0" :: "r"(IRQ_ENABLE_BIT) : "memory");
#else
    __asm__ __volatile__("csrs mstatus, %0" :: "r"(IRQ_ENABLE_BIT) : "memory");
#endif
}

#endif /* LUGALOS_KERNEL_IRQ_H */
