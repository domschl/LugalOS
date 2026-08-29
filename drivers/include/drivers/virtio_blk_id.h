#ifndef LUGALOS_DRIVERS_VIRTIO_BLK_ID_H
#define LUGALOS_DRIVERS_VIRTIO_BLK_ID_H

#include "drivers/block.h"

/* I2, plan/phase21_identity_and_authentication.md: the second virtio-blk
 * device, dedicated to the identity store (kernel/idstore.h). QEMU has no
 * silicon to bind an identity to (§3.1), so a second, small virtio-blk
 * device is the honest analogue -- everything above this seam (parsing,
 * validating, provisioning) is then the same code I7 later points at the
 * RP2350's reserved flash sector.
 *
 * Deliberately not the "blk" driver task's sibling: identity access is
 * boot-time and occasional (a read at startup, a write only when
 * `identity provision`/`identity key`/`peers`/`wlan` run), so the extra
 * machinery a filesystem's read/write traffic justifies would be unused
 * weight here. Direct, polled MMIO access is what the primary driver itself
 * falls back to whenever its task is not running, so this is not a lesser
 * path -- it is the one path, chosen because nothing here needs the other. */

int virtio_blk_id_init(void);
block_dev_t *virtio_blk_id_get_device(void);

#endif /* LUGALOS_DRIVERS_VIRTIO_BLK_ID_H */
