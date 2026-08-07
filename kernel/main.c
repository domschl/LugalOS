#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/ipc.h"
#include "kernel/shell.h"
#include "kernel/time.h"
#include "kernel/version.h"
#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "drivers/usb_cdc.h"
#include "arch/csr.h"
#include "arch/trap.h"
#include "arch/vmm.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "lisp.h"

#if !defined(CONFIG_BOARD_RP2350)
#include "drivers/virtio_console.h"
#endif

#if defined(CONFIG_BOARD_RP2350)
#include "arch/riscv/rp2350/binary_info.h"

extern void led_blink_phase(int count);
extern char __binary_info_start;
extern char __binary_info_end;
extern char __flash_binary_end;

const uint32_t g_address_mapping_table[] = {
    0x10000000, 0x10000000, 0x10400000, /* Flash identity mapping */
    0, 0, 0                             /* Null terminator */
};

static const char g_prog_name[] = "lugalos_microkernel";

static const struct bi_id_and_string_t g_bi_name = {
    .type = BINARY_INFO_TYPE_ID_AND_STRING,
    .tag  = BINARY_INFO_TAG_RP,
    .id   = BINARY_INFO_ID_RP_PROGRAM_NAME,
    .value = (uint32_t)g_prog_name,
};

static const struct bi_id_and_int_t g_bi_binary_end = {
    .type = BINARY_INFO_TYPE_ID_AND_INT,
    .tag  = BINARY_INFO_TAG_RP,
    .id   = BINARY_INFO_ID_RP_BINARY_END,
    .value = (uint32_t)&__flash_binary_end,
};

const void * const __attribute__((section(".binary_info"))) g_p_bi_name = &g_bi_name;
const void * const __attribute__((section(".binary_info"))) g_p_bi_binary_end = &g_bi_binary_end;
#endif

void kernel_main(void) {
#if defined(CONFIG_BOARD_RP2350)
    uart_init(0x40070000);
    led_blink_phase(3);  /* 3 blinks = kernel_main reached */
#else
    uart_init(0x10000000);
#endif

    time_init();

    printk("\n==================================================\n");
    printk("       LugalOS Lisp Machine v%s\n", LUGALOS_VERSION);
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
    i2c_rtc_init();
    at24c32_init();
    usb_cdc_init();
    trap_init();
    vmm_init();
    ipc_init();
    vfs_server_init();
#if !defined(CONFIG_BOARD_RP2350)
    if (virtio_console_init() == 0) {
        p9_link_register_background(virtio_console_get_link());
    }
#else
    // A3b link_usb_cdc: ACM1/EP4 is a dedicated channel (its own USB
    // endpoint pair), not the shared UART -- same zero-console-risk
    // reasoning as virtio_console_init() above on QEMU, so it's registered
    // unconditionally at boot rather than needing an explicit opt-in like
    // `p9share` (the UART demux, which *does* share a wire with the
    // console). Harmless if nothing is ever plugged into ACM1: its poll()
    // just finds an empty ring.
    p9_link_register_background(usb_cdc_get_net_link());
#endif
    sched_init();
    shell_init();
    lisp_init();

    /* Launch Interactive Console Shell (lsh) */
    shell_run();

    /* Hang if system exits */
    while (1) {
        __asm__ __volatile__("wfi");
    }
}

