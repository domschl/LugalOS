#ifndef LUGALOS_KERNEL_DEVICE_H
#define LUGALOS_KERNEL_DEVICE_H

#include <stdint.h>
#include <stdbool.h>

/* Device registry (B0, plan/phase5_distributed_design.md §5.4).
 *
 * Before this, kernel_main() was a fixed init sequence with
 * `#if defined(CONFIG_BOARD_RP2350)` blocks inline: which hardware exists,
 * which 9P link serves inbound requests, and in what order any of it starts
 * were all compile-time facts smeared across the boot path. That is the
 * second of the three concrete blockers §5.2 measures, and like the log ring
 * it needs no scheduler to fix.
 *
 * A driver publishes a `dev_driver_t` describing what it is and how to reach
 * it; a per-board table (kernel/board.c) decides which ones exist on this
 * target; kernel_main() just probes the table. Nothing about *policy* --
 * which link serves 9P, which console the log goes to -- is decided by an
 * #if in the boot path any more.
 *
 * This is deliberately a *device* registry only. The `/srv/` service registry
 * (vfs_register_service(), fs/vfs_server.c) already exists and still maps a
 * name to a placeholder PID; it is the stub B1's chan_t replaces, so it is
 * left alone here rather than being half-merged into this table.
 *
 * No locking: still single-call-stack. B2 must revisit dev_probe_all()'s
 * ordering assumptions once probes can yield.
 */

typedef enum {
    DEV_KIND_CONSOLE,   /* character in/out for an interactive terminal */
    DEV_KIND_P9LINK,    /* provides a p9_link_t (get() returns p9_link_t *) */
    DEV_KIND_CLOCK,     /* real-time clock */
    DEV_KIND_EEPROM,    /* small byte-addressable persistent store */
    DEV_KIND_BLOCK,     /* block device (get() returns block_dev_t *) */
} dev_kind_t;

/* DEV_KIND_P9LINK only: serve inbound 9P on this link from boot. Set for
 * dedicated channels that carry no console traffic and so risk nothing --
 * virtio-console on QEMU, ACM1/EP4 on RP2350. Deliberately NOT set for the
 * UART-backed links, which share a wire with the console and stay behind the
 * explicit `p9serve` / `p9share` commands (see the A3b completion notes). */
#define DEV_F_BACKGROUND_9P (1u << 0)

/* This device coexists with others on its wire rather than owning it (C8).
 * Set for `uartdemux`, whose entire purpose is to carry console traffic and
 * 9P frames on the same UART -- it is the resolution of that conflict, not an
 * instance of it, so it neither claims the wire nor collides with the holder. */
#define DEV_F_SHARES_WIRE (1u << 1)

#define DEV_MAX 14

/* Which physical resource a device drives (C8,
 * plan/phase6_memory_and_processes.md).
 *
 * Several *names* can be the same *wire*. The PL011 appears three times --
 * `uart` as a console, `uartslip` as dedicated SLIP-framed 9P, `uartdemux` as
 * both demultiplexed -- because "one wire, several possible protocols" is the
 * model. What was missing is that nothing recorded they were the same piece of
 * hardware, so binding the console to `uart` while `p9serve` drove `uartslip`
 * gave two owners the same registers. It was masked only because p9serve never
 * returns; p9share exists precisely because that conflict is real.
 *
 * DEV_WIRE_NONE is for devices that are not a shared channel at all -- a clock,
 * an EEPROM, a block device -- which are never exclusive and never claimed. */
typedef enum {
    DEV_WIRE_NONE = 0,
    DEV_WIRE_UART0,     /* the board's primary UART */
    DEV_WIRE_ACM0,      /* USB CDC interface 0 */
    DEV_WIRE_ACM1,      /* USB CDC interface 1 */
    DEV_WIRE_VIRTIO,    /* QEMU virtio-console */
} dev_wire_t;

typedef struct {
    const char *name;
    dev_kind_t  kind;
    uint32_t    flags;
    /* The physical resource this name drives. Devices sharing a wire are
     * mutually exclusive; DEV_WIRE_NONE means not exclusive at all. */
    dev_wire_t  wire;
    /* Returns 0 if the device is present and usable, <0 otherwise. NULL
     * means "present, nothing to initialize". */
    int       (*probe)(void);
    /* Returns the kind-specific object, or NULL if there isn't one. Called
     * only after a successful probe. */
    void     *(*get)(void);
} dev_driver_t;

/* Adds `drv` to the registry. The pointer is stored, not copied, so it must
 * have static lifetime. Returns 0, or -1 if the table is full or the name is
 * already taken. Does not probe -- dev_probe_all() does that. */
int dev_register(const dev_driver_t *drv);

/* Probes every registered device, in registration order, logging what was
 * found. Safe to call once; later calls re-probe nothing. */
void dev_probe_all(void);

/* Looks up a present device's object by name, checking `kind` matches.
 * Returns NULL if absent, of the wrong kind, or has no object. */
void *dev_get(const char *name, dev_kind_t kind);

/* Enumeration for /proc/devices. `index` is 0-based over *registered*
 * devices (present or not); returns false once exhausted. */
bool dev_info(uint32_t index, const char **name_out, const char **kind_out,
              bool *present_out);

/* Iterates the objects of present devices matching `kind` and carrying every
 * bit in `flags`. *cursor must start at 0; returns NULL when exhausted. Lets
 * kernel_main() act on DEV_F_BACKGROUND_9P without this layer needing to
 * know what 9P is. */
void *dev_next_with_flags(uint32_t *cursor, dev_kind_t kind, uint32_t flags);

/* --- Exclusive use of a wire (C8) ---
 *
 * Claims `name`'s wire for it. Returns 0 on success, or -1 if another device
 * already holds that wire -- in which case the caller has been told, by name,
 * what it is competing with. A device on DEV_WIRE_NONE always succeeds and
 * holds nothing.
 *
 * Idempotent: claiming a wire a device already holds is success, so a boot
 * script re-running a binding is not an error. */
int dev_claim(const char *name);

/* Releases `name`'s claim, if it holds one. */
void dev_release(const char *name);

/* The device currently holding `name`'s wire, or NULL if it is free. Returns
 * `name` itself when `name` is the holder. */
const char *dev_wire_owner(const char *name);

/* Enumeration for /proc/ports: every device that drives a wire, with what it
 * is and whether it currently owns that wire. Returns false once exhausted. */
bool dev_binding_info(uint32_t index, const char **name_out, const char **kind_out,
                      const char **wire_out, bool *present_out, bool *bound_out);

/* --- Per-board configuration (kernel/board.c) --- */

/* MMIO base of the boot console UART. Needed before the registry exists,
 * because printk() must work before anything else can be probed. */
uintptr_t board_uart_base(void);

/* Power-of-two, self-aligned executable region for U-mode tasks (B3). */
void board_text_region(uintptr_t *base, uintptr_t *size);

/* M5 Phase 4, plan/phase12_microkernel_migration.md: st7735's own
 * dedicated U-mode-executable page (linker: .st7735text), separate from
 * board_text_region()'s shared .utext -- which ran out of room once
 * st7735's font table and draw functions needed to fit alongside
 * heartbeat/tm1638/i2c's own U-mode code. RP2350-only, like the driver
 * that uses it. */
void board_st7735_text_region(uintptr_t *base, uintptr_t *size);

/* M5 Phase 5, plan/phase12_microkernel_migration.md: blk's own dedicated
 * U-mode-executable page (linker: .blktext), same reasoning as
 * board_st7735_text_region() above -- the shared .utext page overflowed
 * once blk's SD/SPI primitives joined it. RP2350-only, like the driver
 * that uses it. */
void board_blk_text_region(uintptr_t *base, uintptr_t *size);

/* M5 Phase 7, plan/phase12_microkernel_migration.md: usb_cdc's own
 * dedicated U-mode-executable page (linker: .usbtext), same reasoning as
 * board_st7735_text_region()/board_blk_text_region() above -- the shared
 * .utext page overflowed once usb_cdc's dispatch (EP0 enumeration,
 * EP2/EP4 pump/drain, the full SETUP_REQ dispatch) joined it. RP2350-only,
 * like the driver that uses it. */
void board_usb_text_region(uintptr_t *base, uintptr_t *size);

/* Phase 17b, plan/phase17b_clock_task_split.md: the clock server's own
 * dedicated U-mode-executable page (linker: .clocktext), same reasoning as
 * the three above -- it carries the row scan, the frame-buffer writers, the
 * button state machine and the whole proportional font, which on its own is
 * more than .utext had left. RP2350-only, like the driver that uses it. */
void board_clock_text_region(uintptr_t *base, uintptr_t *size);

/* Populates the registry with this board's devices. */
void board_register_devices(void);

/* Restarts the machine. Does not return where it is supported; returns false
 * where it is not, so a caller can say so rather than appearing to hang.
 *
 * Here rather than behind an #if at the call site, following this file's own
 * rule: which hardware a board has, and what it can be asked to do, is a table
 * in one place. Only RP2350 can currently do it -- via the bootrom's reboot
 * entry point, the same one the BOOTSEL touch uses with a different type flag. */
bool board_reset(void);

#endif /* LUGALOS_KERNEL_DEVICE_H */
