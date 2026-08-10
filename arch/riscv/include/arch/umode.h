#ifndef LUGALOS_ARCH_UMODE_H
#define LUGALOS_ARCH_UMODE_H

#include <stdint.h>

/* U-mode entry (B3, plan/phase5_distributed_design.md §5.4).
 *
 * Drops to user mode at `entry` with `user_sp` as the stack and `user_ra` in
 * ra, after parking the caller's kernel stack pointer in the trap path's
 * scratch CSR (see arch/riscv/common/entry.S for that invariant). Does not
 * return: the task leaves U-mode only by trapping.
 *
 * `user_ra` is where the program lands if it *returns* rather than asking to
 * exit, which is how an ordinary compiled `main` ends. It must be an address
 * this task's domain grants execute on; the ELF loader points it at an exit
 * stub inside the user's own image. Pass 0 for user code that never returns.
 *
 * The task needs a memory domain installed (task_set_domain) before this is
 * called, or on the M-mode targets U-mode cannot fetch its first instruction
 * -- see arch/riscv/common/umode.c. */
/* `argc`/`argv` are handed to the program in a0/a1, so a user program is
 * entered as if it were `int _start(int argc, char **argv)` -- no crt needed,
 * the calling convention already puts them there.
 *
 * `argv` must be an address inside the task's *own* domain. The loader builds
 * the vector at the top of the program's stack page for exactly that reason
 * (C3, plan/phase6_memory_and_processes.md): passing a kernel address would
 * hand U-mode a pointer it cannot read, and copying the strings somewhere the
 * kernel owns would be worse -- a pointer into kernel memory is the one thing
 * the whole domain mechanism exists to prevent. Callers with no arguments
 * pass 0, 0. */
void arch_enter_user(void (*entry)(void), uintptr_t user_sp, uintptr_t user_ra,
                     uintptr_t argc, uintptr_t argv);

/* Grants U-mode access to the whole address space via one PMP entry.
 * NOT isolation -- the minimum that makes the mode transition observable, so
 * it can be proven before per-task regions exist. No-op on the S-mode build. */
void arch_user_grant_all(void);

/* Deactivates the grant above. Must be called once the U-mode task that
 * needed it is done: the grant is not boot state, and leaving it installed
 * hands every later U-mode task unrestricted access. */
void arch_user_revoke_all(void);

#endif /* LUGALOS_ARCH_UMODE_H */
