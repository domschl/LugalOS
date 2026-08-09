#ifndef LUGALOS_ARCH_UMODE_H
#define LUGALOS_ARCH_UMODE_H

#include <stdint.h>

/* U-mode entry (B3, plan/phase5_distributed_design.md §5.4).
 *
 * Drops to user mode at `entry` with `user_sp` as the stack, after parking
 * the caller's kernel stack pointer in the trap path's scratch CSR (see
 * arch/riscv/common/entry.S for that invariant). Does not return: the task
 * leaves U-mode only by trapping.
 *
 * Call arch_user_grant_all() first on the M-mode targets, or U-mode cannot
 * fetch its first instruction -- see arch/riscv/common/umode.c. */
void arch_enter_user(void (*entry)(void), uintptr_t user_sp);

/* Grants U-mode access to the whole address space via one PMP entry.
 * NOT isolation -- the minimum that makes the mode transition observable, so
 * it can be proven before per-task regions exist. No-op on the S-mode build. */
void arch_user_grant_all(void);

/* Deactivates the grant above. Must be called once the U-mode task that
 * needed it is done: the grant is not boot state, and leaving it installed
 * hands every later U-mode task unrestricted access. */
void arch_user_revoke_all(void);

#endif /* LUGALOS_ARCH_UMODE_H */
