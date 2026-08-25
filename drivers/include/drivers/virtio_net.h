#ifndef DRIVERS_VIRTIO_NET_H
#define DRIVERS_VIRTIO_NET_H

#include "net/netif.h"

/* virtio-net over virtio-mmio (R1, plan/phase19_ip_stack_and_ethernet.md).
 *
 * The first netif_t, and the one the IP stack is developed against: it
 * delivers whole Ethernet frames, which is exactly what every remaining
 * candidate part does (the ENC28J60, the CYW43439, the ESP32-P4's EMAC) and
 * exactly what the cancelled W5500 did not. Phase 18 §0 declined to build
 * against virtio-net for a reason that stopped being true when the part that
 * terminated TCP in silicon went away -- see the phase 19 plan's §0.
 *
 * Probes the same QEMU MMIO slot range as drivers/virtio_blk.c and
 * drivers/virtio_console.c, matching on device id 1. Registers itself with
 * net/netif.c on success. Returns 0 if a device was found and brought up. */
int virtio_net_init(void);

/* NULL until virtio_net_init() has succeeded. */
netif_t *virtio_net_get_netif(void);

/* Frames the device delivered that were shorter than their own header or
 * longer than the buffer posted for them. A driver-level count, kept apart
 * from netif_t's own counters, which describe what the layer above saw. */
uint32_t virtio_net_malformed(void);

#endif // DRIVERS_VIRTIO_NET_H
