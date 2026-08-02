#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/ipc.h"
#include "kernel/shell.h"
#include "arch/csr.h"
#include "arch/trap.h"
#include "arch/vmm.h"
#include "drivers/uart.h"
#include "lisp.h"

void kernel_main(void) {
    /* Initialize UART 16550 serial console */
    uart_init(0x10000000);

    printk("\n==================================================\n");
    printk("       LugalOS Microkernel Engine v0.2.0\n");
    printk("==================================================\n");

#if defined(CONFIG_TARGET_RV32)
    printk("[Arch] Target: RISC-V 32-bit (RV32IMAC)\n");
#elif defined(CONFIG_TARGET_RV64)
    printk("[Arch] Target: RISC-V 64-bit (RV64GC)\n");
#endif

#if defined(CONFIG_NOMMU)
    printk("[Mode] Memory: NOMMU Physical Direct Execution\n");
#elif defined(CONFIG_MMU)
    printk("[Mode] Memory: Sv39 MMU Virtual Memory Paging Enabled\n");
#endif

#if defined(CONFIG_MODE_S)
    printk("[Priv] Execution Mode: Supervisor Mode (S-mode)\n");
#else
    printk("[Priv] Execution Mode: Machine Mode (M-mode)\n");
#endif

    /* Initialize Subsystems */
    trap_init();
    vmm_init();
    ipc_init();
    sched_init();
    lisp_init();
    shell_init();

    printk("\n[Kernel] Demonstrating IPC Syscall via ecall...\n");
    ipc_msg_t msg_in = { .tag = 0x42, .data = {10, 20, 30, 40, 50} };
    ipc_msg_t msg_out = {0};

    /* Invoke IPC call syscall via ecall */
    register uintptr_t a0 __asm__("a0") = SYS_IPC_CALL;
    register uintptr_t a1 __asm__("a1") = 1;
    register uintptr_t a2 __asm__("a2") = (uintptr_t)&msg_in;
    register uintptr_t a3 __asm__("a3") = (uintptr_t)&msg_out;

    __asm__ __volatile__ (
        "ecall"
        : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3)
        :
        : "memory"
    );

    printk("[Kernel] IPC Response Tag: 0x%lx, Data[0]=%ld (Status=%ld)\n",
           (unsigned long)msg_out.tag, (long)msg_out.data[0], (long)a0);

    printk("[Kernel] Handing control over to interactive Lugal Shell...\n");
    shell_run();

    /* Hang if shell exits */
    while (1) {
        __asm__ __volatile__("wfi");
    }
}
