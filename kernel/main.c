#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/console.h"
#include "kernel/device.h"
#include "kernel/palloc.h"
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

extern char _kernel_end[];
extern char _heap_end[];

void kernel_main(void) {
    /* Boot bootstrap: the console UART and the kernel log sink must both
     * exist before anything can printk(), which the device registry itself
     * does -- so these two cannot go through it, and stay explicit here. */
    uart_init(board_uart_base());
#if defined(CONFIG_BOARD_RP2350)
    led_blink_phase(3);  /* 3 blinks = kernel_main reached */
#endif

    /* B0: attach the default kernel log sink before anything can printk().
     * uart_putc() is exactly what printk() used to call directly, so boot
     * output is byte-identical to the pre-B0 path -- the difference is that
     * it is now detachable (`klog detach console`) and retained in the ring
     * for /proc/kmsg either way. Must precede time_init(), which logs. */
    klog_sink_register("console", uart_putc);

    /* B4: the user-facing stream, bound to the same device by default so
     * behaviour is unchanged until something rebinds it. It is a separate
     * stream, not a second sink: detaching the *log* sink above must not
     * silence the shell. */
    console_bind(uart_putc);

    time_init();

    printk("\n==================================================\n");
    printk("       LugalOS Lisp Machine v%s\n", LUGALOS_VERSION_FULL);
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

    /* Hardware: what exists is a per-board table (kernel/board.c), not a
     * sequence of #ifs here. */
    board_register_devices();
    dev_probe_all();

    /* Kernel subsystems -- not devices, so they stay explicit. Note
     * palloc_init() must precede vmm_init(): the MMU backend allocates its
     * root page table through vmm_alloc_page(), which is now backed by the
     * page allocator rather than an unbounded bump pointer. */
    trap_init();
    palloc_init((uintptr_t)_kernel_end, (uintptr_t)_heap_end);
    vmm_init();
    ipc_init();
    vfs_server_init();

    /* Serve inbound 9P on every dedicated link this board has. The registry
     * decides which those are via DEV_F_BACKGROUND_9P; the UART-backed links
     * deliberately lack the flag because they share a wire with the console
     * and stay behind `p9serve` / `p9share` (see kernel/device.h). */
    uint32_t cursor = 0;
    void *link;
    while ((link = dev_next_with_flags(&cursor, DEV_KIND_P9LINK, DEV_F_BACKGROUND_9P)) != NULL) {
        p9_link_register_background((p9_link_t *)link);
    }

    console_server_init();

    /* Re-bind by name now that the registry exists. The bootstrap binding
     * above had to be a direct function pointer -- the console must work
     * before any device can be probed -- but that leaves the *reported*
     * binding as "(none)", which would be a lie the moment anyone asked
     * which device owns the terminal. */
    console_bind_device("uart");

    sched_init();
    /* The 9P/filesystem server, now a scheduled task rather than something
     * pumped from the console's busy-wait (D4). Must follow sched_init(). */
    p9_server_task_start();

    shell_init();
    lisp_init();

    /* Launch Interactive Console Shell (lsh) */
    shell_run();

    /* Hang if system exits */
    while (1) {
        __asm__ __volatile__("wfi");
    }
}

