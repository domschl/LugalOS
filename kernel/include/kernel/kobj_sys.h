#ifndef LUGALOS_KERNEL_KOBJ_SYS_H
#define LUGALOS_KERNEL_KOBJ_SYS_H

#include <stdint.h>
#include "kernel/kobj_abi.h"

/* Dispatches one of the SYS_KOBJ_BASE.. syscalls on behalf of the calling
 * task. Called from the trap handler (arch/riscv/common/trap.c); `op` is
 * `nr - SYS_KOBJ_BASE`. Returns the value for a0. */
long kobj_syscall(unsigned op, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                  uintptr_t a4, uintptr_t a5);

/* `kobjutest` (kernel/kobj_utest.c): a U-mode task using every operation. */
int kobj_utest(void);
/* Releases everything a finished memory domain still holds in the kernel-object layer -- its objects,
 * timers and critical-section mutex -- and returns how many objects and timers that was. Only once no
 * task of the domain can run any more. */
uint32_t kobj_sys_release_domain(uintptr_t owner);

#endif
