#ifndef FS_P9_LINK_H
#define FS_P9_LINK_H

#include <stdint.h>
#include <stdbool.h>

/* Transport-agnostic 9P link (A3, plan/phase5_distributed_design.md) -- the
 * 9P server (fs/9p.c, A2) never knows what wire it's running over. A
 * backend (drivers/virtio_console.c, drivers/uart_net.c's SLIP link, ...)
 * fills in a p9_link_t and hands it to p9_link_service()/the background
 * pump; framing (SLIP-escaped vs. plain length-prefixed) is entirely the
 * backend's own business.
 *
 * `poll()` pumps the backend's RX path and returns 1 once a *complete*
 * frame is buffered and ready for recv_frame(), 0 if nothing's ready yet,
 * -1 on a transport error. It must never block -- callers may invoke it
 * from a tight busy-wait loop (see p9_link_background_poll()). */
typedef struct p9_link {
    const char *name;
    int (*poll)(struct p9_link *link);
    int (*send_frame)(struct p9_link *link, const uint8_t *buf, uint32_t len);
    int (*recv_frame)(struct p9_link *link, uint8_t *buf, uint32_t max_len);
    void *ctx;
} p9_link_t;

/* Services exactly one pending request on `link`, if poll() reports one
 * ready: recv_frame() -> p9_server_process() -> send_frame(). Returns 1 if
 * a request was serviced, 0 if nothing was pending, -1 on a transport
 * error (a malformed *frame* still yields 1 with an Rerror reply --
 * p9_server_process() itself never fails to produce a response for a frame
 * it actually received). Never blocks. */
int p9_link_service(p9_link_t *link);

/* Registers `link` to be serviced opportunistically from
 * p9_link_background_poll() -- the hook used by drivers/uart_16550.c's
 * uart_getc() busy-wait loop (the same spot usb_cdc_task() is already
 * pumped from) so a QEMU-only link like virtio-console can carry live 9P
 * traffic *while the interactive shell is blocked on keyboard input*,
 * without any RX demultiplexing of the console's own byte stream (that's
 * the deferred, high-risk A3b work -- see the A3 completion notes). Pass
 * NULL to unregister. At most one background link at a time. */
void p9_link_register_background(p9_link_t *link);
void p9_link_background_poll(void);

/* Link-agnostic synchronous 9P client (A4, plan/phase5_distributed_design.md):
 * attaches at "/", walks to `path` (split on '/'), opens it for read, reads
 * the whole file into `out_buf`, clunks. Works over any p9_link_t, not just
 * loopback -- e.g. a node fetching a file from whatever peer is bridged
 * onto its virtio-console link. See fs/p9_link.c for why this is safe to
 * use even on a link that also has a registered background server.
 * Returns the byte count read, or -1. A non-responding peer blocks forever
 * (matches drivers/virtio_blk.c's own busy-wait-until-done convention). */
int p9_link_cat(p9_link_t *link, const char *path, char *out_buf, uint32_t out_max);

#endif // FS_P9_LINK_H
