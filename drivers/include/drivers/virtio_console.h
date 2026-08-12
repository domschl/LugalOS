#ifndef LUGALOS_DRIVERS_VIRTIO_CONSOLE_H
#define LUGALOS_DRIVERS_VIRTIO_CONSOLE_H

#include <stdint.h>
#include "fs/p9_link.h"

/* QEMU-only virtio-serial (device id 3, non-multiport: RX = queue 0,
 * TX = queue 1) MMIO driver, dedicated entirely to carrying 9P traffic
 * (A3, plan/phase5_distributed_design.md) -- a clean channel separate from
 * the UART console, so it needs no RX demultiplexing (that's link_uart_slip's
 * problem, deferred as A3b). Probes the same 0x10001000..0x10008000 MMIO
 * slot range as drivers/virtio_blk.c.
 *
 * Returns 0 on success (a virtconsole device was found and brought up), -1
 * if none is present -- e.g. a QEMU invocation that didn't pass
 * `-device virtconsole,...`. Harmless to call unconditionally on QEMU
 * targets; never linked in on RP2350 builds. */
int virtio_console_init(void);

/* NULL if virtio_console_init() hasn't succeeded. */
p9_link_t *virtio_console_get_link(void);

/* J5 (plan/phase10_chess_completion.md): raw byte read/write for
 * chess_uci_run()'s QEMU test path. See virtio_console.c's own comment on
 * these for why a caller must suspend background 9P on this link first. */
int virtio_console_write_raw(const uint8_t *data, uint32_t len);
uint32_t virtio_console_read_raw(uint8_t *buf, uint32_t max);

#endif /* LUGALOS_DRIVERS_VIRTIO_CONSOLE_H */
