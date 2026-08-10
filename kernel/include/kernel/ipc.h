#ifndef LUGALOS_KERNEL_IPC_H
#define LUGALOS_KERNEL_IPC_H

#include <stdint.h>
#include <stddef.h>

/* Syscall numbers 1-4 were the register-IPC entry points -- sys_ipc_call,
 * _reply, _send, _recv. They were fixed stubs from the day they were written
 * and were superseded by copy-always channels (kernel/chan.h), which is what
 * the microkernel actually uses. Deleted in C3.
 *
 * The numbers stay burned rather than being reused. A binary built against the
 * old ABI would otherwise silently invoke whatever took its number; leaving
 * the range empty makes it a clean "unknown syscall" instead. */
#define SYS_IPC_RESERVED_LOW   1
#define SYS_IPC_RESERVED_HIGH  4

/* Reach a named service over a copy-always channel (C3). This is the entry
 * point 1-4 were meant to be and never were: a U-mode task naming a service,
 * with every buffer validated against the caller's own domain and copied.
 * See the handler in arch/riscv/common/trap.c. */
#define SYS_CHAN_CALL 5

/* B3: a U-mode task's only way back out. Handled in trap.c by ending the
 * calling task -- a user task cannot call task_exit() itself, since that is
 * kernel code manipulating kernel state. */
#define SYS_UEXIT 20

void ipc_init(void);

#endif /* LUGALOS_KERNEL_IPC_H */
