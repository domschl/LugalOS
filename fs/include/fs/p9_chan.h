#ifndef FS_P9_CHAN_H
#define FS_P9_CHAN_H

#include "fs/p9_link.h"

/* The local 9P server, reachable as a channel endpoint and exposed as an
 * ordinary p9_link_t (B1, plan/phase5_distributed_design.md §5.1).
 *
 * Registers the "p9" endpoint (visible as /srv/p9) whose handler is
 * p9_server_process(). Everything above sees a p9_link_t indistinguishable
 * from virtio-console's or USB-CDC's -- which is the entire claim B1 makes:
 * a local server and a remote server are the same code path, because the
 * copy-always discipline that an address-space boundary would force is
 * already being obeyed with no boundary present.
 *
 * Call once at boot, after p9_init(). */
int p9_chan_init(void);

/* NULL until p9_chan_init() has succeeded. */
p9_link_t *p9_chan_get_link(void);

#endif /* FS_P9_CHAN_H */
