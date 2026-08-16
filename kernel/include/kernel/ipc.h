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

/* M5, plan/phase12_microkernel_migration.md: the first two syscalls a
 * long-lived U-mode *driver* task needs that a one-shot U-mode user program
 * never did -- a cooperative yield and a pacing clock, both value-only (no
 * pointer, nothing to validate against a domain). See arch/riscv/common/
 * trap.c's own comments on each for the fuller reasoning. */
#define SYS_YIELD    22
#define SYS_TIME_MS  23

/* M5 Phase 2, plan/phase12_microkernel_migration.md: what a U-mode driver
 * task needs beyond Phase 1's heartbeat -- a microsecond-granular delay for
 * timing-sensitive bit-bang protocols (SYS_TIME_MS is millisecond-granular,
 * useless for a few-microsecond pulse width), and the server half of the
 * channel API (chan_serve_wait()/chan_serve_reply() have no U-mode route
 * yet; only the client half, SYS_CHAN_CALL above, does). See
 * arch/riscv/common/trap.c's own comments on each. */
#define SYS_DELAY_US         24
#define SYS_CHAN_SERVE_WAIT  25
#define SYS_CHAN_SERVE_REPLY 26

void ipc_init(void);

#endif /* LUGALOS_KERNEL_IPC_H */
