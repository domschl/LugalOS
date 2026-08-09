#include "fs/p9_chan.h"
#include "fs/9p.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include <string.h>

/* The local 9P server as a channel endpoint (B1,
 * plan/phase5_distributed_design.md §5.1).
 *
 * This is the concrete payoff of Rule 2 ("9P *is* the service IPC"): the
 * local filesystem server is reached through exactly the same p9_link_t
 * interface as a server on the far end of a USB cable, differing only in
 * which channel the frames cross. Nothing above this file can tell the
 * difference -- vfs_mount_local() is literally vfs_mount_remote() handed
 * this link instead of a wire's.
 *
 * The frames really are serialized and really are copied (twice, via
 * chan_call). That is not ceremony: it is what makes the same server source
 * correct when B3 puts a hardware boundary between client and server, and it
 * is why Track A's remote case needed no separate protocol design.
 */

static uint8_t g_req_buf[P9_MAX_MSIZE];
static uint8_t g_resp_buf[P9_MAX_MSIZE];

/* The pending reply from the most recent send_frame(). p9_link_t's contract
 * is send -> poll -> recv, but a local channel call is synchronous: the
 * response already exists by the time send_frame() returns, so it is parked
 * here for the recv_frame() that the interface says must follow. */
static uint8_t  g_reply[P9_MAX_MSIZE];
static uint32_t g_reply_len;

static chan_endpoint_t *g_ep;

/* The endpoint handler: a request message in, a response message out, both
 * in endpoint-owned memory. p9_server_process() already has exactly this
 * shape, which is not a coincidence -- it was written to be driven from a
 * wire (A3's p9_link_service() calls it identically). */
static int p9_chan_handler(void *ctx, const uint8_t *req, uint32_t req_len,
                           uint8_t *resp, uint32_t resp_max) {
    (void)ctx;
    return p9_server_process(req, req_len, resp, resp_max);
}

static int p9_chan_send(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    if (!g_ep) return -1;
    int n = chan_call(g_ep, buf, len, g_reply, sizeof(g_reply));
    if (n < 0) {
        g_reply_len = 0;
        return -1;
    }
    g_reply_len = (uint32_t)n;
    return 0;
}

static int p9_chan_poll(p9_link_t *link) {
    (void)link;
    return g_reply_len > 0 ? 1 : 0;
}

static int p9_chan_recv(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    if (g_reply_len == 0) return 0;
    if (g_reply_len > max_len) {
        /* Caller can't hold the reply. Drop it rather than hand back a
         * truncated 9P frame, which the parser would reject anyway with a
         * far more confusing symptom. */
        g_reply_len = 0;
        return -1;
    }
    uint32_t n = g_reply_len;
    memcpy(buf, g_reply, n);
    g_reply_len = 0;
    return (int)n;
}

static p9_link_t g_link = {
    .name       = "chan-local",
    .poll       = p9_chan_poll,
    .send_frame = p9_chan_send,
    .recv_frame = p9_chan_recv,
    .ctx        = NULL,
};

int p9_chan_init(void) {
    if (g_ep) return 0;
    if (chan_register("p9", p9_chan_handler, NULL,
                      g_req_buf, sizeof(g_req_buf),
                      g_resp_buf, sizeof(g_resp_buf)) != 0) {
        printk("[9P Chan] Failed to register local 9P endpoint\n");
        return -1;
    }
    g_ep = chan_lookup("p9");
    printk("[9P Chan] Local 9P server endpoint '/srv/p9' online (copy-always IPC).\n");
    return 0;
}

p9_link_t *p9_chan_get_link(void) {
    return g_ep ? &g_link : NULL;
}
