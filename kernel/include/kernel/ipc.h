#ifndef LUGALOS_KERNEL_IPC_H
#define LUGALOS_KERNEL_IPC_H

#include <stdint.h>
#include <stddef.h>

#define SYS_IPC_CALL   1
#define SYS_IPC_REPLY  2
#define SYS_IPC_SEND   3
#define SYS_IPC_RECV   4

#define IPC_ANY -1

/* Zero-copy register IPC message payload (fits in a1..a6) */
typedef struct ipc_msg {
    uintptr_t tag;
    uintptr_t data[5];
} ipc_msg_t;

void ipc_init(void);
long sys_ipc_call(int target_pid, ipc_msg_t *msg_in, ipc_msg_t *msg_out);
long sys_ipc_reply(int client_pid, ipc_msg_t *msg);
long sys_ipc_send(int target_pid, ipc_msg_t *msg);
long sys_ipc_recv(int src_pid, ipc_msg_t *msg);

#endif /* LUGALOS_KERNEL_IPC_H */
