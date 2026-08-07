#include "fs/p9_link.h"
#include "fs/9p.h"
#include "kernel/printk.h"
#include <string.h>

static p9_link_t *g_background_link = NULL;

int p9_link_service(p9_link_t *link) {
    if (!link || !link->poll || !link->recv_frame || !link->send_frame) return -1;

    int ready = link->poll(link);
    if (ready <= 0) return ready; // 0 = nothing pending, -1 = transport error

    static uint8_t req_buf[P9_MAX_MSIZE];
    static uint8_t resp_buf[P9_MAX_MSIZE];

    int req_len = link->recv_frame(link, req_buf, sizeof(req_buf));
    if (req_len < 7) return -1;

    int resp_len = p9_server_process(req_buf, (uint32_t)req_len, resp_buf, sizeof(resp_buf));
    if (resp_len < 7) return -1;

    if (link->send_frame(link, resp_buf, (uint32_t)resp_len) < 0) return -1;
    return 1;
}

void p9_link_register_background(p9_link_t *link) {
    g_background_link = link;
    if (link) {
        printk("[9P Link] '%s' registered as background transport.\n", link->name ? link->name : "?");
    }
}

void p9_link_background_poll(void) {
    if (!g_background_link) return;
    p9_link_service(g_background_link);
}

/* --- p9_link_cat: a link-agnostic synchronous 9P client (A4) ---
 *
 * Safe to run on a link that also has a registered background server (see
 * p9_link_register_background() above), including on itself: this kernel
 * has no real task scheduler or interrupts that could preempt a C function
 * call (kernel/sched.c is a bookkeeping shim, not a scheduler -- see the
 * A1/A2 completion notes), so while p9_link_cat() is running, nothing else
 * -- including this link's own background pump -- can run concurrently and
 * misinterpret one of its frames. The remote peer only ever *responds* to
 * what this function sends (it never spontaneously originates traffic), so
 * there is no risk of the two ends' server roles colliding either.
 *
 * This does mean the roles are asymmetric by convention, not by
 * negotiation: whichever node actively calls p9_link_cat() is acting as
 * client for that one exchange, and the peer it's talking to must be
 * purely a server (running only its own background pump) for as long as
 * that holds. Good enough for A4's test topology (see the completion
 * notes); a real bidirectional peer-to-peer protocol would need tag-aware
 * multiplexing this doesn't attempt. */

/* Waits for a reply, exactly like virtio_blk_transfer()'s own
 * busy-wait-until-done poll (drivers/virtio_blk.c) -- no separate timeout
 * mechanism; a non-responsive peer hangs the caller, matching that
 * existing precedent rather than inventing a new one. */
static int p9_link_wait_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    for (;;) {
        int ready = link->poll(link);
        if (ready < 0) return -1;
        if (ready > 0) return link->recv_frame(link, buf, max_len);
    }
}

static int p9_link_roundtrip(p9_link_t *link, const p9_msg_t *req, p9_msg_t *resp,
                              uint8_t *tx, uint8_t *rx, uint32_t cap) {
    int tx_len = p9_serialize(req, tx, cap);
    if (tx_len < 7) return -1;
    if (link->send_frame(link, tx, (uint32_t)tx_len) < 0) return -1;
    int rx_len = p9_link_wait_frame(link, rx, cap);
    if (rx_len < 7) return -1;
    return p9_deserialize(rx, (uint32_t)rx_len, resp);
}

int p9_link_cat(p9_link_t *link, const char *path, char *out_buf, uint32_t out_max) {
    if (!link || !link->poll || !link->send_frame || !link->recv_frame || !path) return -1;
    while (*path == '/') path++;

    static uint8_t tx[P9_MAX_MSIZE];
    static uint8_t rx[P9_MAX_MSIZE];
    p9_msg_t req, resp;

    memset(&req, 0, sizeof(req));
    req.type = P9_TVERSION;
    req.tag = 1;
    req.msize = P9_MAX_MSIZE;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RVERSION) return -1;

    memset(&req, 0, sizeof(req));
    req.type = P9_TATTACH;
    req.tag = 2;
    req.fid = 1;
    strncpy(req.uname, "lugal", sizeof(req.uname) - 1);
    // aname left empty -> namespace root "/"
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RATTACH) return -1;

    memset(&req, 0, sizeof(req));
    req.type = P9_TWALK;
    req.tag = 3;
    req.fid = 1;
    req.newfid = 2;
    char pathcopy[128];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';
    uint16_t n = 0;
    char *tok = pathcopy;
    while (*tok && n < P9_MAX_WALK_ELEM) {
        char *slash = strchr(tok, '/');
        if (slash) *slash = '\0';
        if (*tok) {
            strncpy(req.wname[n], tok, P9_MAX_NAME_LEN - 1);
            n++;
        }
        if (!slash) break;
        tok = slash + 1;
    }
    req.nwname = n;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RWALK || resp.nwqid != n) return -1;

    memset(&req, 0, sizeof(req));
    req.type = P9_TOPEN;
    req.tag = 4;
    req.fid = 2;
    req.mode = P9_OREAD;
    if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_ROPEN) return -1;

    uint32_t total = 0;
    uint64_t offset = 0;
    uint16_t tag = 5;
    bool read_failed = false;
    for (;;) {
        uint32_t room = (out_buf && out_max > 0 && total + 1 < out_max) ? (out_max - 1 - total) : 0;
        uint32_t want = room < 1024 ? room : 1024;
        if (want == 0) break;

        memset(&req, 0, sizeof(req));
        req.type = P9_TREAD;
        req.tag = tag++;
        req.fid = 2;
        req.offset = offset;
        req.count = want;
        if (p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)) < 0 || resp.type != P9_RREAD) { read_failed = true; break; }
        if (resp.count == 0) break;

        if (out_buf) memcpy(out_buf + total, resp.data, resp.count);
        total += resp.count;
        offset += resp.count;
    }
    if (out_buf && out_max > 0) out_buf[(total < out_max) ? total : out_max - 1] = '\0';

    memset(&req, 0, sizeof(req));
    req.type = P9_TCLUNK;
    req.tag = tag;
    req.fid = 2;
    p9_link_roundtrip(link, &req, &resp, tx, rx, sizeof(tx)); // best-effort cleanup

    return read_failed ? -1 : (int)total;
}
