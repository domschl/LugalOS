#include "kernel/device.h"
#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "drivers/usb_cdc.h"
#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "drivers/block.h"
#include "kernel/console.h"

#if !defined(CONFIG_BOARD_RP2350)
#include "drivers/virtio_console.h"
#include "drivers/virtio_blk.h"
#endif

/* Per-board device tables (B0, plan/phase5_distributed_design.md §5.4).
 *
 * This file exists so that "which hardware does this board have" is a table
 * in one place rather than `#if defined(CONFIG_BOARD_RP2350)` blocks spliced
 * through kernel_main()'s boot sequence. The #ifs did not disappear -- they
 * were never going to, since CMake compiles a different driver set per board
 * -- but they are now confined to the declarations below instead of
 * interleaving with initialization order and 9P link policy.
 */

/* The executable region a U-mode task needs RX access to (B3).
 *
 * Must be a power-of-two-sized, self-aligned block for NAPOT, so these are
 * whole memory-map regions rather than tight bounds around .text. Coarse but
 * honest: the grant is execute+read only, so a task can run kernel code (it
 * still cannot execute a single privileged instruction) and read constants,
 * but cannot modify anything. Tight per-task code regions need user programs
 * linked separately, which is B6's ELF work. */
extern char _utext_start[];

void board_text_region(uintptr_t *base, uintptr_t *size) {
    /* The dedicated U-mode-executable page (linker: .utext), not the kernel's
     * text. Granting U-mode execute on kernel .text would stop the kernel
     * executing it under Sv39, since S-mode may not fetch instructions from a
     * page marked U. Invisible under PMP, fatal under paging -- so one page
     * of code that U-mode owns, on both. */
    *base = (uintptr_t)_utext_start;
    *size = 4096;
}

uintptr_t board_uart_base(void) {
#if defined(CONFIG_BOARD_RP2350)
    return 0x40070000;
#else
    return 0x10000000;
#endif
}

/* --- probe/get adapters ---
 * Several drivers predate the registry and return void from their init, or
 * expose their object under a differently-shaped accessor. These thin shims
 * adapt them rather than churning every driver's public signature. */

static int probe_i2c_rtc(void)  { i2c_rtc_init();  return 0; }
static int probe_at24c32(void)  { at24c32_init();  return 0; }
static int probe_usb_cdc(void)  { usb_cdc_init();  return 0; }

/* Console devices, exposed so the console stream can be bound to one by name
 * at runtime (B4). `uart` is the boot console: already initialised during
 * bootstrap, so it has no probe. */
static console_dev_t g_uart_console = { .putc = uart_putc };
static void *get_uart_console(void) { return &g_uart_console; }

static console_dev_t g_usb_console = { .putc = usb_cdc_putc };
static void *get_usb_console(void)  { return &g_usb_console; }

static void *get_uart_slip(void)  { return uart_slip_get_link(); }
static void *get_uart_demux(void) { return uart_demux_get_link(); }

#if defined(CONFIG_BOARD_RP2350)
/* usb_cdc_get_net_link() already returns NULL off-RP2350, but ACM1/EP4 only
 * has a data path on this board, so it is only registered here. */
static void *get_usb_net(void) { return usb_cdc_get_net_link(); }
#else
static int   probe_virtio_console(void) { return virtio_console_init(); }
static void *get_virtio_console(void)   { return virtio_console_get_link(); }
static int   probe_virtio_blk(void)     { return virtio_blk_init(); }
static void *get_virtio_blk(void)       { return virtio_blk_get_device(); }
#endif

/* --- The tables ---
 * Order matters: dev_probe_all() probes in registration order, and this
 * order reproduces kernel_main()'s pre-B0 sequence (rtc -> eeprom -> usb)
 * so the change is a refactor, not a reordering.
 *
 * The two virtio probes match on REG_DEVICE_ID (block=2, console=3), so they
 * cannot claim each other's MMIO slot regardless of which runs first --
 * checked before moving virtio_console's probe earlier than it used to run
 * (it was previously initialized after vfs_server_init()). */

static const dev_driver_t dev_rtc = {
    .name = "rtc", .kind = DEV_KIND_CLOCK, .probe = probe_i2c_rtc,
};
static const dev_driver_t dev_eeprom = {
    .name = "eeprom", .kind = DEV_KIND_EEPROM, .probe = probe_at24c32,
};
static const dev_driver_t dev_usb = {
    .name = "usb", .kind = DEV_KIND_CONSOLE, .probe = probe_usb_cdc,
    .get = get_usb_console,
};
static const dev_driver_t dev_uart = {
    .name = "uart", .kind = DEV_KIND_CONSOLE, .get = get_uart_console,
};

/* UART-backed 9P links: present unconditionally (the UART itself is brought
 * up during early boot bootstrap, before the registry exists), but WITHOUT
 * DEV_F_BACKGROUND_9P -- they share a wire with the console, so they stay
 * behind the explicit `p9serve` / `p9share` commands. */
static const dev_driver_t dev_uartslip = {
    .name = "uartslip", .kind = DEV_KIND_P9LINK, .get = get_uart_slip,
};
static const dev_driver_t dev_uartdemux = {
    .name = "uartdemux", .kind = DEV_KIND_P9LINK, .get = get_uart_demux,
};

#if defined(CONFIG_BOARD_RP2350)
static const dev_driver_t dev_usbnet = {
    .name = "usbnet", .kind = DEV_KIND_P9LINK, .flags = DEV_F_BACKGROUND_9P,
    .get = get_usb_net,
};
#else
static const dev_driver_t dev_vconsole = {
    .name = "vconsole", .kind = DEV_KIND_P9LINK, .flags = DEV_F_BACKGROUND_9P,
    .probe = probe_virtio_console, .get = get_virtio_console,
};
static const dev_driver_t dev_vblk = {
    .name = "vblk", .kind = DEV_KIND_BLOCK,
    .probe = probe_virtio_blk, .get = get_virtio_blk,
};
#endif

void board_register_devices(void) {
    dev_register(&dev_rtc);
    dev_register(&dev_eeprom);
    dev_register(&dev_usb);
    dev_register(&dev_uart);
    dev_register(&dev_uartslip);
    dev_register(&dev_uartdemux);
#if defined(CONFIG_BOARD_RP2350)
    dev_register(&dev_usbnet);
#else
    dev_register(&dev_vconsole);
    dev_register(&dev_vblk);
#endif
}
