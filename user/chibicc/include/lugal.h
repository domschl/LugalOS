/*
 * <lugal.h> - LugalOS Microkernel C System Call & User API Header
 * Copyright (c) 2026 LugalOS Developers
 * License: MIT License
 */

#ifndef _LUGAL_H
#define _LUGAL_H

#include <stdint.h>
#include <stddef.h>

/* System Call Numbers */
#define SYS_IPC_CALL   1
#define SYS_IPC_REPLY  2
#define SYS_IPC_SEND   3
#define SYS_IPC_RECV   4

#define IPC_ANY -1

/* Zero-copy register IPC message payload */
typedef struct ipc_msg {
    uintptr_t tag;
    uintptr_t d0;
    uintptr_t d1;
    uintptr_t d2;
    uintptr_t d3;
    uintptr_t d4;
} ipc_msg_t;

/* Direct ecall inline assembly for RISC-V LugalOS Syscalls */
static inline long lugal_syscall(long sys_nr, long a1, long a2, long a3) {
    register long a0 __asm__("a0") = sys_nr;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a3 __asm__("a3") = a3;

    __asm__ __volatile__(
        "ecall"
        : "+r"(a0)
        : "r"(r_a1), "r"(r_a2), "r"(r_a3)
        : "memory"
    );
    return a0;
}

static inline long ipc_call(int target_pid, ipc_msg_t *msg_in, ipc_msg_t *msg_out) {
    return lugal_syscall(SYS_IPC_CALL, (long)target_pid, (long)msg_in, (long)msg_out);
}

static inline long ipc_send(int target_pid, ipc_msg_t *msg_in) {
    return lugal_syscall(SYS_IPC_SEND, (long)target_pid, (long)msg_in, 0);
}

static inline long ipc_recv(int src_pid, ipc_msg_t *msg_out) {
    return lugal_syscall(SYS_IPC_RECV, (long)src_pid, (long)msg_out, 0);
}

#endif /* _LUGAL_H */
