#ifndef DRIVERS_VIRTIO_RNG_H
#define DRIVERS_VIRTIO_RNG_H

#include <stdbool.h>
#include <stdint.h>

/* virtio-rng (entropy device, virtio id 4) over virtio-mmio: QEMU's
 * `-device virtio-rng-device`, fed from the host's /dev/urandom. With it a QEMU
 * guest has a hardware entropy source as far as kernel/random.c is concerned,
 * so key generation, signing and TLS can be tested there; without it nothing
 * changes (random_is_hardware() stays false).
 *
 * Probes the same MMIO slot range as drivers/virtio_net.c. Returns 0 when a
 * device was found and its queue is up. Call once, before the first random
 * byte (kernel/random.c's random_init()). */
int virtio_rng_init(void);

bool virtio_rng_present(void);

/* Fills `buf` with `len` bytes from the device. False if there is no device or
 * it did not answer within its timeout (then `buf` holds nothing usable).
 * Serialised internally; callable from any task. */
bool virtio_rng_read(void *buf, uint32_t len);

#endif /* DRIVERS_VIRTIO_RNG_H */
