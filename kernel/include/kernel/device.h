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

#define DEV_MAX 12

typedef struct {
    const char *name;
    dev_kind_t  kind;
    uint32_t    flags;
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

/* --- Per-board configuration (kernel/board.c) --- */

/* MMIO base of the boot console UART. Needed before the registry exists,
 * because printk() must work before anything else can be probed. */
uintptr_t board_uart_base(void);

/* Populates the registry with this board's devices. */
void board_register_devices(void);

#endif /* LUGALOS_KERNEL_DEVICE_H */
