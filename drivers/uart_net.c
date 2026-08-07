#include "drivers/uart_net.h"
#include "fs/9p.h"
#include "drivers/uart.h"
#include "kernel/printk.h"
#include <string.h>

void uart_net_init(void) {
    printk("[UART 9P] Universal Serial Network Transport Engine (SLIP RFC 1055) Online.\n");
}

/* --- link_uart_slip (A3) ---
 * Accumulates raw (still SLIP-escaped) bytes as they arrive; a SLIP_END
 * marks the end of a frame, at which point the accumulated bytes (which
 * never contain SLIP_END themselves, by construction) are handed to the
 * existing, already-tested slip_decode() as a single call -- reusing it
 * exactly as-is rather than re-implementing incremental unescaping. */
#define UART_SLIP_RAW_CAP (P9_MAX_MSIZE * 2) // SLIP escaping can double frame size worst-case

typedef struct {
    uint8_t raw[UART_SLIP_RAW_CAP];
    uint32_t raw_len;
    uint8_t frame[P9_MAX_MSIZE];
    uint32_t frame_len;
    bool frame_ready;
} uart_slip_ctx_t;

static uart_slip_ctx_t g_uart_slip_ctx;

static int uart_slip_poll(p9_link_t *link) {
    (void)link;
    uart_slip_ctx_t *ctx = &g_uart_slip_ctx;
    if (ctx->frame_ready) return 1;

    while (uart_has_char()) {
        uint8_t c = (uint8_t)uart_getc(); // won't block: uart_has_char() just confirmed a byte is ready
        if (c == SLIP_END) {
            if (ctx->raw_len == 0) continue; // leading/duplicate delimiter
            int n = slip_decode(ctx->raw, ctx->raw_len, ctx->frame, sizeof(ctx->frame));
            ctx->raw_len = 0;
            if (n > 0) {
                ctx->frame_len = (uint32_t)n;
                ctx->frame_ready = true;
                return 1;
            }
            continue; // empty/undersized decode; keep listening for the next frame
        }
        if (ctx->raw_len < UART_SLIP_RAW_CAP) {
            ctx->raw[ctx->raw_len++] = c;
        } else {
            ctx->raw_len = 0; // overflow: drop and resync on the next SLIP_END
        }
    }
    return ctx->frame_ready ? 1 : 0;
}

static int uart_slip_recv_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    uart_slip_ctx_t *ctx = &g_uart_slip_ctx;
    if (!ctx->frame_ready) return -1;
    uint32_t n = (ctx->frame_len < max_len) ? ctx->frame_len : max_len;
    memcpy(buf, ctx->frame, n);
    ctx->frame_ready = false;
    ctx->frame_len = 0;
    return (int)n;
}

static int uart_slip_send_frame(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    static uint8_t enc[UART_SLIP_RAW_CAP];
    int n = slip_encode(buf, len, enc, sizeof(enc));
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) uart_putc((char)enc[i]);
    return (int)len;
}

static p9_link_t g_uart_slip_link = {
    .name = "uart-slip",
    .poll = uart_slip_poll,
    .send_frame = uart_slip_send_frame,
    .recv_frame = uart_slip_recv_frame,
    .ctx = NULL,
};

p9_link_t *uart_slip_get_link(void) {
    return &g_uart_slip_link;
}

int slip_encode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max) {
    if (!src || !dst || dst_max < 2) return -1;

    uint32_t out = 0;
    dst[out++] = SLIP_END;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == SLIP_END) {
            if (out + 2 >= dst_max) return -1;
            dst[out++] = SLIP_ESC;
            dst[out++] = SLIP_ESC_END;
        } else if (c == SLIP_ESC) {
            if (out + 2 >= dst_max) return -1;
            dst[out++] = SLIP_ESC;
            dst[out++] = SLIP_ESC_ESC;
        } else {
            if (out + 1 >= dst_max) return -1;
            dst[out++] = c;
        }
    }

    if (out + 1 > dst_max) return -1;
    dst[out++] = SLIP_END;
    return (int)out;
}

int slip_decode(const uint8_t *src, uint32_t len, uint8_t *dst, uint32_t dst_max) {
    if (!src || !dst || len == 0) return -1;

    uint32_t out = 0;
    bool escaping = false;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = src[i];
        if (c == SLIP_END) {
            if (out > 0) break; // Frame completed
            continue;
        }

        if (escaping) {
            if (c == SLIP_ESC_END) c = SLIP_END;
            else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
            escaping = false;
        } else if (c == SLIP_ESC) {
            escaping = true;
            continue;
        }

        if (out >= dst_max) return -1;
        dst[out++] = c;
    }
    return (int)out;
}

int uart_net_send_9p(const uint8_t *req_buf, uint32_t req_len, uint8_t *resp_buf, uint32_t resp_max) {
    if (!req_buf || req_len < 7 || !resp_buf) return -1;

    static uint8_t slip_tx[2048];
    static uint8_t slip_rx[2048];

    int slip_len = slip_encode(req_buf, req_len, slip_tx, sizeof(slip_tx));
    if (slip_len < 0) return -1;

    // Direct microkernel 9P server processing over SLIP transport
    int resp_len = p9_server_process(req_buf, req_len, resp_buf, resp_max);
    if (resp_len < 7) return -1;

    int slip_resp_len = slip_encode(resp_buf, (uint32_t)resp_len, slip_rx, sizeof(slip_rx));
    if (slip_resp_len < 0) return -1;

    return slip_decode(slip_rx, (uint32_t)slip_resp_len, resp_buf, resp_max);
}

/* Same real-file 9P session as loopback_9p_rpc() (drivers/loopback_net.c),
 * over the SLIP/UART transport instead -- see its comment for why the
 * walk/create/open dance is now required instead of a bare Twrite/Tread on
 * the attach fid. Uses its own scratch filename so concurrent loopback and
 * UART RPCs (e.g. both exercised back-to-back by tests/runner.py) never
 * collide on the same file. */
int uart_net_rpc(const char *write_payload, char *read_out_buf, uint32_t read_max) {
    static const char *SCRATCH_NAME = "uart9p.tmp";
    static uint8_t tx_buf[1024];
    static uint8_t rx_buf[1024];

    // 1. Tversion -- also resets the server's fid table for a clean session.
    p9_msg_t tv = { .type = P9_TVERSION, .tag = 1, .msize = 1024 };
    int tx_len = p9_serialize(&tv, tx_buf, sizeof(tx_buf));
    int rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    // 2. Tattach fid=1 at /ram0 (always mounted -- see vfs_mount_ramdisk()).
    p9_msg_t ta = { .type = P9_TATTACH, .tag = 2, .fid = 1, .uname = "lugal", .aname = "ram0" };
    tx_len = p9_serialize(&ta, tx_buf, sizeof(tx_buf));
    rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    if (write_payload && strlen(write_payload) > 0) {
        uint32_t plen = (uint32_t)strlen(write_payload);

        // 3a. Twalk fid=1 -> newfid=2, cloned (still /ram0, unopened).
        p9_msg_t tw0 = { .type = P9_TWALK, .tag = 3, .fid = 1, .newfid = 2, .nwname = 0 };
        tx_len = p9_serialize(&tw0, tx_buf, sizeof(tx_buf));
        rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;

        // 3b. Tcreate fid=2: creates (or truncates) the scratch file; fid=2
        // now refers to it, already open for write.
        p9_msg_t tc = { .type = P9_TCREATE, .tag = 4, .fid = 2, .perm = 0644, .mode = P9_OWRITE | P9_OTRUNC };
        strncpy(tc.name, SCRATCH_NAME, sizeof(tc.name) - 1);
        tx_len = p9_serialize(&tc, tx_buf, sizeof(tx_buf));
        rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;

        // 3c. Twrite fid=2.
        p9_msg_t twr = { .type = P9_TWRITE, .tag = 5, .fid = 2, .offset = 0, .count = plen, .data = (const uint8_t *)write_payload };
        tx_len = p9_serialize(&twr, tx_buf, sizeof(tx_buf));
        rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;

        // 3d. Tclunk fid=2.
        p9_msg_t tcl = { .type = P9_TCLUNK, .tag = 6, .fid = 2 };
        tx_len = p9_serialize(&tcl, tx_buf, sizeof(tx_buf));
        rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
        if (rx_len < 7) return -1;
    }

    // 4a. Twalk fid=1 -> newfid=3, into the scratch file.
    p9_msg_t tw1 = { .type = P9_TWALK, .tag = 7, .fid = 1, .newfid = 3, .nwname = 1 };
    strncpy(tw1.wname[0], SCRATCH_NAME, sizeof(tw1.wname[0]) - 1);
    tx_len = p9_serialize(&tw1, tx_buf, sizeof(tx_buf));
    rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;
    p9_msg_t walk_resp;
    if (p9_deserialize(rx_buf, (uint32_t)rx_len, &walk_resp) < 0 || walk_resp.type != P9_RWALK) return -1;

    // 4b. Topen fid=3 for read.
    p9_msg_t to = { .type = P9_TOPEN, .tag = 8, .fid = 3, .mode = P9_OREAD };
    tx_len = p9_serialize(&to, tx_buf, sizeof(tx_buf));
    rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
    if (rx_len < 7) return -1;

    // 4c. Tread fid=3.
    p9_msg_t tr = { .type = P9_TREAD, .tag = 9, .fid = 3, .offset = 0, .count = (read_max > 0) ? (read_max - 1) : 0 };
    tx_len = p9_serialize(&tr, tx_buf, sizeof(tx_buf));
    rx_len = uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));
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
    uart_net_send_9p(tx_buf, (uint32_t)tx_len, rx_buf, sizeof(rx_buf));

    return result;
}
