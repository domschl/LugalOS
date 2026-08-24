#include "kernel/printk.h"
#include "kernel/klog.h"
#include "kernel/console.h"
#include "kernel/device.h"
#include "kernel/palloc.h"
#include "kernel/balloc.h"
#include "kernel/path.h"
#include "kernel/ticker.h"
#include "kernel/irq.h"
#include "kernel/sched.h"
#include "kernel/ipc.h"
#include "kernel/shell.h"
#include "kernel/time.h"
#include "kernel/version.h"
#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "drivers/usb_cdc.h"
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
#include "drivers/st7735.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
#include "drivers/tm1638.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
#include "drivers/pico_clock_green.h"
#endif
#ifndef CLOCK_BOOT_MARK
/* Every other target: the boot beacon is a Pico-Clock-Green thing, because it
 * is the only board with no console, no LED and no card to report with. */
#define CLOCK_BOOT_MARK(n) ((void)0)
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
#include "drivers/dcf77.h"
#endif
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_W5500_SCK_GPIO)
#include "drivers/w5500.h"
#endif
#include "arch/csr.h"
#include "arch/trap.h"
#include "arch/vmm.h"
#include "drivers/uart.h"
#include "fs/vfs.h"
#include "fs/p9_link.h"
#include "lisp.h"

#if !defined(CONFIG_BOARD_RP2350)
#include "drivers/virtio_console.h"
#include "drivers/virtio_blk.h"
#else
#include "drivers/spisd.h"
#endif

#if defined(CONFIG_BOARD_RP2350)
#include "arch/riscv/rp2350/binary_info.h"

extern int  heartbeat_task_start(void);
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

/* The kernel log's default sink: the physical console device, with the
 * terminal newline convention applied on the way out (C0, see
 * kernel/console.h). Deliberately not console_putc() -- that is the *console
 * stream*, which init.lisp may rebind to another device, and the log sink and
 * the shell are meant to be independently routable. This sink names its
 * device the way the pre-C0 code did; only the CRLF conversion is new, and it
 * is new here because printk() stopped doing it. */
static void klog_terminal_sink(char c) {
    console_emit(uart_putc, c);
}

void kernel_main(void) {
    /* Boot bootstrap: the console UART and the kernel log sink must both
     * exist before anything can printk(), which the device registry itself
     * does -- so these two cannot go through it, and stay explicit here. */
    uart_init(board_uart_base());
    /* The 2-and-3-blink boot signal that used to be here and in
     * uart_rp2350.c is gone (user, 2026-08-23). It dates from bring-up, when
     * "did we reach kernel_main at all" was a live question; it answered that
     * with a GPIO the clock persona does not even have, and charged roughly a
     * second of dead busy-wait to every boot on every board to do it.
     *
     * Its replacement is CLOCK_BOOT_MARK() (drivers/pico_clock_green.h), off
     * by default and with no call sites at rest: set CONFIG_CLOCK_BOOT_BEACON
     * and scatter marks over whatever is suspect. It clicks the buzzer once
     * per mark and latches a count on the LED row, so a hang leaves its last
     * mark lit rather than merely stopping -- which is how the charger-boot
     * fault was found. */

    /* B0: attach the default kernel log sink before anything can printk().
     * uart_putc() is exactly what printk() used to call directly, so boot
     * output is byte-identical to the pre-B0 path -- the difference is that
     * it is now detachable (`klog detach console`) and retained in the ring
     * for /proc/kmsg either way. Must precede time_init(), which logs. */
    klog_sink_register("console", klog_terminal_sink);

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
    /* ST7735 canvas + TM1638 keypad/7-segment (H1/H2,
     * plan/phase9_chess_computer.md): dedicated, always-wired hardware for
     * this board persona, so both are initialized eagerly here rather than
     * lazily on first Lisp call, the same shape as the LED init above rather
     * than spisd's lazy pattern (which exists because a MicroSD card is
     * optional/hot-pluggable; neither of these is). Independently gated
     * (H3) -- a board persona might want the keypad/LEDs without the
     * display, or vice versa. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    st7735_init();
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    tm1638_init();
#endif
    /* SM16106/SM5166P LED matrix + LDR (L2, plan/phase11_pico_clock_green.md):
     * dedicated, always-wired hardware for the Pico-Clock-Green board
     * persona specifically -- same eager-init rationale as ST7735/TM1638
     * above, not this driver's own invention. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    pico_clock_green_init();
#endif
    /* DCF-77 receiver pins (D2, plan/phase17_clock_ui_and_dcf77.md). Init
     * only: this leaves the module powered *down* (PON released), so an
     * unpopulated GP27/GP28 pair costs nothing and a populated one draws
     * nothing until something asks for a sync or a probe. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_DCF77
    dcf77_init();
#endif
    /* W5500 Ethernet (N4, plan/phase18_networking_and_auth.md): the gateway
     * persona's whole reason for existing. Eager, like every other soldered
     * peripheral above -- and early, because "does the SPI bus reach a
     * W5500" is the first question a bring-up asks and its answer belongs in
     * the boot log rather than behind a command. Without an address it stops
     * short of joining the network and says so; see the plan's §3. */
#if defined(CONFIG_BOARD_RP2350) && defined(CONFIG_W5500_SCK_GPIO)
    w5500_init();
#endif

    /* Kernel subsystems -- not devices, so they stay explicit. Note
     * palloc_init() must precede vmm_init(): the MMU backend allocates its
     * root page table through vmm_alloc_page(), which is now backed by the
     * page allocator rather than an unbounded bump pointer. */
    trap_init();
    palloc_init((uintptr_t)_kernel_end, (uintptr_t)_heap_end);
    /* M1, plan/phase12_microkernel_migration.md: reserves its arena out of
     * what palloc_init() just brought up, so it must follow it and nothing
     * needs to follow *it* -- no other subsystem below depends on balloc
     * yet. */
    balloc_init();
    /* Before vfs_server_init(): the boot scripts it loads are themselves
     * found through the path (C1). */
    path_init();
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
    /* Record that the background links hold their wires (C8), so a later
     * binding onto the same channel is refused rather than quietly making two
     * owners. Their wires are dedicated, so this never fails at boot -- it is
     * the bookkeeping that makes a *later* conflict detectable. */
    dev_claim("usbnet");
    dev_claim("vconsole");

    console_server_init();

    /* Re-bind by name now that the registry exists. The bootstrap binding
     * above had to be a direct function pointer -- the console must work
     * before any device can be probed -- but that leaves the *reported*
     * binding as "(none)", which would be a lie the moment anyone asked
     * which device owns the terminal. */
    console_bind_device("uart");

    sched_init();
    /* Preemption (B6). Started after sched_init() so there is a task table to
     * switch within, and after the server task below would be pointless --
     * the tick is what lets a busy node keep answering. 100 Hz: frequent
     * enough that a spinning task cannot monopolise the console, rare enough
     * that the switch cost is irrelevant. */
    if (ticker_init(100)) {
        irq_restore(IRQ_ENABLE_BIT); /* from here on, anything can be preempted */
    }

#if defined(CONFIG_BOARD_RP2350)
    /* M4.5, plan/phase12_microkernel_migration.md, Part B: usb_cdc.c's own
     * background servicing task, started before uart_task_start() below --
     * RP2350's console mirrors every write to USB CDC, so the task that
     * drains it should already exist by the time that starts happening,
     * even though nothing strictly deadlocks if the order were reversed
     * (both are independent, already-running tasks by the time any real
     * interactive use occurs). Must follow sched_init(). */
    usb_cdc_task_start();
#endif

    /* M4, plan/phase12_microkernel_migration.md: the uart driver as a task,
     * the first one converted from "library called synchronously by
     * whoever needs them" to "long-lived task reachable via chan_call()".
     * Must follow sched_init() (task_create() needs a table) and precede
     * anything that does real console output, so as little boot text as
     * possible falls back to the pre-M4 direct-access path for no reason
     * other than the task not existing yet. */
    uart_task_start();

#if defined(CONFIG_BOARD_RP2350)
    /* M4.5, plan/phase12_microkernel_migration.md, Part B: the GP16
     * heartbeat LED as its own task, not a side effect of console polling
     * -- a live, on-the-board visual check that the scheduler is actually
     * giving every READY task a turn, independent of a serial connection.
     * Must follow sched_init() for the same reason uart_task_start() does. */
    heartbeat_task_start();
#endif

    /* M4.5, plan/phase12_microkernel_migration.md, Part B: the SD/block
     * storage driver as a task -- the lowest-risk conversion in the
     * remaining driver list, since a read_blocks()/write_blocks() call was
     * already exactly one message's worth of work, no batching redesign
     * needed. Must follow sched_init(); every read before this point
     * (vfs_server_init() above mounts the filesystem before a task table
     * even exists) already used, and keeps using, direct hardware access --
     * this only changes the path for whatever reads happen after it. Split
     * by board the same way the driver source list itself is: spisd_rp2350.c
     * (real SD hardware) only builds for RP2350, virtio_blk.c (QEMU) only
     * for everything else. */
#if defined(CONFIG_BOARD_RP2350)
    spisd_task_start();
#else
    virtio_blk_task_start();
#endif

    /* M4.5, plan/phase12_microkernel_migration.md, Part B: RTC + EEPROM as
     * one shared "i2c" task -- both devices sit on the same physical I2C
     * bus (see drivers/i2c_rtc.h), so one task, not two. Must follow
     * sched_init(); dev_probe_all() above already detected both devices via
     * direct hardware access, and every RTC/EEPROM call keeps using that
     * path if this fails or hasn't run yet. */
    i2c_task_start();

    /* M4.5, plan/phase12_microkernel_migration.md, Part B: display/keypad as
     * tasks. Independently gated the same way their init calls above are
     * (H3) -- a board persona might have the keypad without the display, or
     * vice versa; the Pico-Clock-Green persona has neither of these and its
     * own "clock" task instead. Must follow sched_init(); every function
     * these drivers expose keeps using direct hardware access if this fails
     * or hasn't run yet. */
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_ST7735
    st7735_task_start();
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_TM1638
    tm1638_task_start();
#endif
#if defined(CONFIG_BOARD_RP2350) && CONFIG_ENABLE_PICO_CLOCK_GREEN
    pico_clock_green_task_start();
#endif

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

