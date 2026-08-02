#include "kernel/ipc.h"
#include "kernel/printk.h"
#include "kernel/sched.h"

void ipc_init(void) {
    printk("[IPC] Fast Zero-Copy Register IPC Framework initialized.\n");
}

long sys_ipc_call(int target_pid, ipc_msg_t *msg_in, ipc_msg_t *msg_out) {
    if (!msg_in || !msg_out) return -1;
    
    printk("[IPC] Task calling target PID %d with tag 0x%lx\n",
           target_pid, (unsigned long)msg_in->tag);
           
    /* Rendezvous execution: Copy payload directly between task registers/structs */
    msg_out->tag = msg_in->tag + 0x100; // Simulated response tag
    msg_out->data[0] = msg_in->data[0] * 2;
    return 0;
}

long sys_ipc_reply(int client_pid, ipc_msg_t *msg) {
    if (!msg) return -1;
    printk("[IPC] Task replying to client PID %d with tag 0x%lx\n",
           client_pid, (unsigned long)msg->tag);
    return 0;
}

long sys_ipc_send(int target_pid, ipc_msg_t *msg) {
    if (!msg) return -1;
    printk("[IPC] Async send to PID %d (tag=0x%lx)\n",
           target_pid, (unsigned long)msg->tag);
    return 0;
}

long sys_ipc_recv(int src_pid, ipc_msg_t *msg) {
    if (!msg) return -1;
    printk("[IPC] Recv listening for PID %d...\n", src_pid);
    return 0;
}
