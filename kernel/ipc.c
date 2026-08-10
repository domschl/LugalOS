#include "kernel/ipc.h"
#include "kernel/printk.h"

/* What used to be here: sys_ipc_call/_reply/_send/_recv, a "zero-copy register
 * IPC" that never worked. Each was a fixed stub -- it printed what it would
 * have done and returned a constant -- and nothing in the kernel ever called
 * one. They were superseded rather than finished: services are reached by
 * message passing over copy-always channels (kernel/chan.h), which is what
 * makes a local service and a service on another machine the same code path,
 * and what a U-mode task now reaches through SYS_CHAN_CALL.
 *
 * Deleted in C3 (plan/phase6_memory_and_processes.md) together with the
 * ipc_msg_t register-message struct, in the same change that gave U-mode the
 * channel syscall. Removing them alone would have left user programs exactly
 * as unable to reach a service as they were before, which is the opposite of
 * what the phase is for.
 *
 * This file stays for ipc_init(), which the boot sequence calls and which is
 * the natural place for a real IPC subsystem to reappear. */

void ipc_init(void) {
    printk("[IPC] Message passing: copy-always channels (kernel/chan.h)\n");
}
