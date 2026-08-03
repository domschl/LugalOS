#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/ipc.h"
#include "kernel/shell.h"
#include "arch/csr.h"
#include "arch/trap.h"
#include "arch/vmm.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "lisp.h"

void kernel_main(void) {
    /* Initialize UART 16550 serial console */
    uart_init(0x10000000);

    printk("\n==================================================\n");
    printk("       LugalOS Microkernel Lisp Machine v0.4.0\n");
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
    vfs_server_init();
    sched_init();
    shell_init();
    lisp_init();

    /* Hang if system exits */
    while (1) {
        __asm__ __volatile__("wfi");
    }
}

