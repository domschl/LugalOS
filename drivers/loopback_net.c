#include "drivers/loopback_net.h"
#include "kernel/identity.h"
#include "fs/9p.h"
#include "fs/p9_chan.h"
#include "fs/p9_link.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include <string.h>

void loopback_net_init(void) {
    p9_init();
    p9_chan_init();
    printk("[Loopback 9P] In-Memory Transport Gateway Online.\n");
}

/* B1: goes through the "p9" channel endpoint rather than calling
 * p9_server_process() directly with the caller's buffers. The two copies
 * chan_call() performs are redundant in this single-address-space build and
 * are done anyway -- see kernel/chan.h on why the NOMMU path deliberately
 * obeys the MMU path's constraints. Net effect: every existing loopback test
 * now exercises the copy-always channel. */
int loopback_send_9p(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max) {
    if (!req_buf || req_len < 7 || !resp_buf) return -1;
    chan_endpoint_t *ep = chan_lookup("p9");
    if (!ep) return -1;
    return chan_call(ep, req_buf, req_len, resp_buf, resp_max);
}

/* Drives a full 9P session against a real file on /ram0 -- attach, walk to
 * (creating if needed) a scratch file, write and/or read it, clunk. The
 * server (fs/9p.c, A2) now has a real fid table wired to the VFS handle
 * API instead of a single global echo buffer, so a Twrite/Tread against an
 * unopened directory fid correctly fails; this is the minimum real 9P
 * message sequence that gets to an actual open file. */
int loopback_9p_rpc(const char *write_payload, char *read_out_buf, uint32_t read_max) {
    static const char *SCRATCH_NAME = "loopback9p.tmp";
    static uint8_t tx_buf[1024];
    static uint8_t rx_buf[1024];

    // 1. Tversion -- also resets the server's fid table for a clean session.
    p9_msg_t tv = { .type = P9_TVERSION, .tag = 1, .msize = 1024 };
    int tx_len = p9_serialize(&tv, tx_buf, sizeof(tx_buf));
    int rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    // 2. Tattach fid=1 at /ram0 (always mounted -- see vfs_mount_ramdisk()).
    // fid=1 is kept unopened for the whole call, reused below as the
    // directory anchor for both the write and read walks.
    p9_msg_t ta = { .type = P9_TATTACH, .tag = 2, .fid = 1, .aname = "ram0" };
    strncpy(ta.uname, node_name(), sizeof(ta.uname) - 1);
    tx_len = p9_serialize(&ta, tx_buf, sizeof(tx_buf));
    rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    if (write_payload && strlen(write_payload) > 0) {
        uint32_t plen = (uint32_t)strlen(write_payload);

        // 3a. Twalk fid=1 -> newfid=2, cloned (still /ram0, unopened).
        p9_msg_t tw0 = { .type = P9_TWALK, .tag = 3, .fid = 1, .newfid = 2, .nwname = 0 };
        tx_len = p9_serialize(&tw0, tx_buf, sizeof(tx_buf));
        rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;

        // 3b. Tcreate fid=2: creates (or truncates) the scratch file; fid=2
        // now refers to it, already open for write.
        p9_msg_t tc = { .type = P9_TCREATE, .tag = 4, .fid = 2, .perm = 0644, .mode = P9_OWRITE | P9_OTRUNC };
        strncpy(tc.name, SCRATCH_NAME, sizeof(tc.name) - 1);
        tx_len = p9_serialize(&tc, tx_buf, sizeof(tx_buf));
        rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;

        // 3c. Twrite fid=2.
        p9_msg_t twr = { .type = P9_TWRITE, .tag = 5, .fid = 2, .offset = 0, .count = plen, .data = (const uint8_t *)write_payload };
        tx_len = p9_serialize(&twr, tx_buf, sizeof(tx_buf));
        rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;

        // 3d. Tclunk fid=2.
        p9_msg_t tcl = { .type = P9_TCLUNK, .tag = 6, .fid = 2 };
        tx_len = p9_serialize(&tcl, tx_buf, sizeof(tx_buf));
        rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;
    }

    // 4a. Twalk fid=1 -> newfid=3, into the scratch file.
    p9_msg_t tw1 = { .type = P9_TWALK, .tag = 7, .fid = 1, .newfid = 3, .nwname = 1 };
    strncpy(tw1.wname[0], SCRATCH_NAME, sizeof(tw1.wname[0]) - 1);
    tx_len = p9_serialize(&tw1, tx_buf, sizeof(tx_buf));
    rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;
    p9_msg_t walk_resp;
    if (p9_deserialize(rx_buf, (uint32_t)rx_len, &walk_resp) < 0 || walk_resp.type != P9_RWALK) return -1;

    // 4b. Topen fid=3 for read.
    p9_msg_t to = { .type = P9_TOPEN, .tag = 8, .fid = 3, .mode = P9_OREAD };
    tx_len = p9_serialize(&to, tx_buf, sizeof(tx_buf));
    rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    // 4c. Tread fid=3.
    p9_msg_t tr = { .type = P9_TREAD, .tag = 9, .fid = 3, .offset = 0, .count = (read_max > 0) ? (read_max - 1) : 0 };
    tx_len = p9_serialize(&tr, tx_buf, sizeof(tx_buf));
    rx_len = loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    p9_msg_t rr;
    if (p9_deserialize(rx_buf, (uint32_t)rx_len, &rr) < 0 || rr.type != P9_RREAD) return -1;

    int result = -1;
    if (read_out_buf && read_max > 0) {
        uint32_t copy_cnt = rr.count;
        if (copy_cnt >= read_max) copy_cnt = read_max - 1;
        if (rr.data && copy_cnt > 0) memcpy(read_out_buf, rr.data, copy_cnt);
        read_out_buf[copy_cnt] = '\0';
        result = (int)copy_cnt;
    }

    // 4d. Tclunk fid=3 -- best-effort cleanup, done after the read data has
    // already been copied out of rx_buf (the next exchange overwrites it).
    p9_msg_t tcl2 = { .type = P9_TCLUNK, .tag = 10, .fid = 3 };
    tx_len = p9_serialize(&tcl2, tx_buf, sizeof(tx_buf));
    loopback_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));

    return result;
}

/* B1: was a ~60-line hand-rolled Tversion/Tattach/Twalk/Topen/Tread/Tclunk
 * sequence duplicating fs/p9_link.c's p9_link_cat() (A4), which does exactly
 * the same thing over any p9_link_t. Now that the local 9P server is reachable
 * as an ordinary link (fs/p9_chan.c), the duplicate is gone: the *same client
 * code* drives the local server and a peer across a USB cable. That
 * equivalence is what B1 exists to demonstrate. */
int loopback_9p_cat(const char *path, char *out_buf, uint32_t out_max) {
    return p9_link_cat(p9_chan_get_link(), path, out_buf, out_max);
}
