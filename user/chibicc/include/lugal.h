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

/* Intrinsic LugalOS RISC-V System Call (emits hardware ecall instruction) */
long lugal_syscall(long sys_nr, long a1, long a2, long a3);

#endif /* _LUGAL_H */
