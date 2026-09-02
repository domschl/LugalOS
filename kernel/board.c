#include "kernel/device.h"
#include "lugalos_config.h"
#include "drivers/uart.h"
#include "drivers/uart_net.h"
#include "drivers/usb_cdc.h"
#include "drivers/i2c_rtc.h"
#include "drivers/at24c32.h"
#include "drivers/block.h"
#include "kernel/console.h"
#if defined(CONFIG_BOARD_RP2350)
#include "arch/rp2350_bootrom.h"
#if CONFIG_ENABLE_SPISD
#include "drivers/spisd.h"
#endif
#include "drivers/enc28j60.h"
#endif

#if !defined(CONFIG_BOARD_RP2350)
#include "drivers/virtio_console.h"
#include "drivers/virtio_blk.h"
#include "drivers/virtio_net.h"
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

#if defined(CONFIG_BOARD_RP2350)
extern char _st7735text_start[];

void board_st7735_text_region(uintptr_t *base, uintptr_t *size) {
    *base = (uintptr_t)_st7735text_start;
    *size = 4096;
}

extern char _blktext_start[];

void board_blk_text_region(uintptr_t *base, uintptr_t *size) {
    *base = (uintptr_t)_blktext_start;
    *size = 4096;
}

extern char _usbtext_start[];

void board_usb_text_region(uintptr_t *base, uintptr_t *size) {
    *base = (uintptr_t)_usbtext_start;
    *size = 4096;
}

extern char _clocktext_start[];

void board_clock_text_region(uintptr_t *base, uintptr_t *size) {
    *base = (uintptr_t)_clocktext_start;
    *size = 4096;
}
#endif

/* K2, plan/phase7_kernel_config.md: this used to be its own independent
 * #ifdef ladder, hand-typing the same 0x40070000 that drivers/uart_rp2350.c
 * separately hand-typed as its own UART0_BASE -- two copies of one fact,
 * with nothing keeping them in sync. Both now read CONFIG_UART0_BASE from
 * the generated per-board header (cmake/board-rp2350.cmake and friends). */
uintptr_t board_uart_base(void) {
    return CONFIG_UART0_BASE;
}

/* --- probe/get adapters ---
 * Several drivers predate the registry and return void from their init, or
 * expose their object under a differently-shaped accessor. These thin shims
 * adapt them rather than churning every driver's public signature. */

/* Reports whether a chip actually answered, not merely that the probe ran.
 *
 * This returned 0 unconditionally, so /proc/devices listed the RTC as
 * "present" on a board where nothing was on the bus -- which is exactly the
 * board where someone reads that line to find out. It cost a real
 * investigation: the registry said present, the kernel log said nothing had
 * been detected, and both were telling the truth about different things. */
static int probe_i2c_rtc(void)  { i2c_rtc_init();  return i2c_rtc_is_detected() ? 0 : -1; }
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
#if CONFIG_ENABLE_SPISD
/* The SD card as a registry entry, so `/proc/devices` and `ls /dev` agree
 * with what actually mounted. QEMU's virtio block device has been listed
 * since B0; the RP2350 card never was, which nobody noticed while the only
 * personas with a card were also the ones with a display to look at. On the
 * gateway persona -- a file server whose entire job is that card -- a disk
 * missing from the device list is a question waiting to be asked. Already
 * probed by the time this runs (kernel_main() initialises it before
 * dev_probe_all()), so there is no probe hook: just the accessor. */
static void *get_spisd(void) { return spisd_get_device(); }
#endif
#if defined(CONFIG_ETH_CS_GPIO)
/* R4, plan/phase19_ip_stack_and_ethernet.md: the ENC28J60 on the gateway
 * persona's SPI0 -- the wire an Ethernet 9P link used to sit on directly
 * (N4's `w5500net`, removed with the part in R0). This is a netif now, not
 * a p9_link_t: everything above it (TCP, the 9P server) is net/stack.c's
 * job, reached through DEV_KIND_NETIF the same way dev_vnet feeds it on
 * QEMU. */
static int   probe_enc28j60(void) { return enc28j60_init(); }
static void *get_enc28j60(void)   { return enc28j60_get_netif(); }
#endif
#else
static int   probe_virtio_console(void) { return virtio_console_init(); }
static void *get_virtio_console(void)   { return virtio_console_get_link(); }
static int   probe_virtio_blk(void)     { return virtio_blk_init(); }
static void *get_virtio_blk(void)       { return virtio_blk_get_device(); }
static int   probe_virtio_net(void)     { return virtio_net_init(); }
static void *get_virtio_net(void)       { return virtio_net_get_netif(); }
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
    .name = "usb", .kind = DEV_KIND_CONSOLE, .wire = DEV_WIRE_ACM0, .probe = probe_usb_cdc,
    .get = get_usb_console,
};
static const dev_driver_t dev_uart = {
    .name = "uart", .kind = DEV_KIND_CONSOLE, .wire = DEV_WIRE_UART0, .get = get_uart_console,
};

/* UART-backed 9P links: present unconditionally (the UART itself is brought
 * up during early boot bootstrap, before the registry exists), but WITHOUT
 * DEV_F_BACKGROUND_9P -- they share a wire with the console, so they stay
 * behind the explicit `p9serve` / `p9share` commands. */
static const dev_driver_t dev_uartslip = {
    .name = "uartslip", .kind = DEV_KIND_P9LINK, .wire = DEV_WIRE_UART0, .get = get_uart_slip,
};
static const dev_driver_t dev_uartdemux = {
    .name = "uartdemux", .kind = DEV_KIND_P9LINK, .flags = DEV_F_SHARES_WIRE,
    .wire = DEV_WIRE_UART0, .get = get_uart_demux,
};

#if defined(CONFIG_BOARD_RP2350)
/* ACM1 as a *console*, in addition to `usbnet` which is the same wire as a 9P
 * link (C8). Both paths already existed in drivers/usb_cdc.c -- EP4 has a
 * write for 9P frames and this wraps it a byte at a time -- so the interface
 * that was only ever a network port can now be handed the terminal instead.
 * The two are mutually exclusive by construction: same wire, one holder.
 *
 * ACM0 is deliberately not bindable this way and stays the console. It is the
 * port a host connects to by convention and the one flash.py touches, so
 * making it stealable would mean a boot script could lock the operator out of
 * the machine it is booting. */
static void usbnet_console_putc(char c) {
    uint8_t b = (uint8_t)c;
    usb_cdc_write_net(&b, 1);
}
static console_dev_t g_usbnet_console = { .putc = usbnet_console_putc };
static void *get_usbnet_console(void) { return &g_usbnet_console; }

static const dev_driver_t dev_usbnetcon = {
    .name = "usbcon", .kind = DEV_KIND_CONSOLE, .wire = DEV_WIRE_ACM1,
    .get = get_usbnet_console,
};

#if CONFIG_ENABLE_SPISD
static const dev_driver_t dev_spisd = {
    .name = "sdblk", .kind = DEV_KIND_BLOCK, .get = get_spisd,
};
#endif
#if defined(CONFIG_UART1_BASE)
/* N5: the downlink. Deliberately WITHOUT DEV_F_BACKGROUND_9P -- on this wire
 * the gateway is the client and the board at the other end is the server, so
 * nothing here needs to answer inbound requests. It also keeps a background
 * slot free: P9_LINK_MAX_BACKGROUND is 2, and usbnet plus whatever Ethernet
 * link this persona carries are what fill it. */
static void *get_uart1(void) { return uart1_get_link(); }
static const dev_driver_t dev_uart1 = {
    .name = "uart1", .kind = DEV_KIND_P9LINK, .get = get_uart1,
};
#endif
/* An Ethernet 9P link was registered here directly (N4's `w5500net`) and
 * went with the part in phase 19's R0 -- the W5500 terminated TCP itself,
 * so its device WAS a p9_link_t. The ENC28J60 (dev_enc28j60, below) hands
 * over Ethernet frames instead, registers as DEV_KIND_NETIF, and TCP is
 * net/stack.c's job from there; an accepted connection becomes a
 * `p9_link_t` (`tcpnet`) once the stack itself registers one, same as it
 * already does on the QEMU targets. */
static const dev_driver_t dev_usbnet = {
    .name = "usbnet", .kind = DEV_KIND_P9LINK, .flags = DEV_F_BACKGROUND_9P, .wire = DEV_WIRE_ACM1,
    .get = get_usb_net,
};
#if defined(CONFIG_ETH_CS_GPIO)
static const dev_driver_t dev_enc28j60 = {
    .name = "enc0", .kind = DEV_KIND_NETIF,
    .probe = probe_enc28j60, .get = get_enc28j60,
};
#endif
#else
static const dev_driver_t dev_vconsole = {
    .name = "vconsole", .kind = DEV_KIND_P9LINK, .flags = DEV_F_BACKGROUND_9P, .wire = DEV_WIRE_VIRTIO,
    .probe = probe_virtio_console, .get = get_virtio_console,
};
static const dev_driver_t dev_vblk = {
    .name = "vblk", .kind = DEV_KIND_BLOCK,
    .probe = probe_virtio_blk, .get = get_virtio_blk,
};
/* R1, plan/phase19_ip_stack_and_ethernet.md: the first netif. Its probe
 * matches on virtio device id 1, so it cannot claim the block or console
 * slot regardless of probe order -- the same argument the comment above
 * makes for those two. Absent `-netdev`/`-device virtio-net-device` the
 * probe finds nothing and says nothing, which is the ordinary case for
 * every existing test in tests/runner.py. */
static const dev_driver_t dev_vnet = {
    .name = "vnet", .kind = DEV_KIND_NETIF,
    .probe = probe_virtio_net, .get = get_virtio_net,
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
    dev_register(&dev_usbnetcon);
#if CONFIG_ENABLE_SPISD
    dev_register(&dev_spisd);
#endif
#if defined(CONFIG_UART1_BASE)
    dev_register(&dev_uart1);
#endif
#if defined(CONFIG_ETH_CS_GPIO)
    dev_register(&dev_enc28j60);
#endif
#else
    dev_register(&dev_vconsole);
    dev_register(&dev_vblk);
    dev_register(&dev_vnet);
#endif
}

/* See kernel/device.h. Only RP2350 has something to ask; the QEMU targets have
 * no reset path this kernel can reach, and saying so is better than pretending
 * to reboot and continuing. */
bool board_reset(void) {
#if defined(CONFIG_BOARD_RP2350)
    return rp2350_reboot();
#else
    return false;
#endif
}
