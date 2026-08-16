#include "drivers/uart_net.h"
#include "fs/9p.h"
#include "drivers/uart.h"
#include "kernel/printk.h"
#include "kernel/irq.h"
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
/* Defined below, next to the block codec it replaced in the receive paths. */
static int slip_feed(uint8_t c, uint8_t *frame, uint32_t frame_cap,
                     uint32_t *frame_len, bool *escaping);

typedef struct {
    bool escaping;          /* mid-escape across byte boundaries */
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
        if (slip_feed(c, ctx->frame, sizeof(ctx->frame), &ctx->frame_len,
                      &ctx->escaping)) {
            ctx->frame_ready = true;
            return 1;
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
    /* Encoded straight onto the wire rather than into a buffer first. The
     * bytes were going to uart_putc() one at a time regardless, so the 8 KB
     * staging buffer this replaces existed only to hold a copy of them (C5).
     * slip_encode() stays for the loopback self-test below, which genuinely
     * needs the encoded form as a block. */
    if (!buf) return -1;
    uart_putc((char)SLIP_END);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c == SLIP_END) {
            uart_putc((char)SLIP_ESC);
            uart_putc((char)SLIP_ESC_END);
        } else if (c == SLIP_ESC) {
            uart_putc((char)SLIP_ESC);
            uart_putc((char)SLIP_ESC_ESC);
        } else {
            uart_putc((char)c);
        }
    }
    uart_putc((char)SLIP_END);
    /* M4, plan/phase12_microkernel_migration.md: uart_putc() batches now
     * (drivers/uart_16550.c) instead of writing straight through, so a
     * complete frame sitting in that batch is not actually on the wire
     * until something flushes it. Nothing else in this call path ever
     * would -- this function *is* the natural per-message boundary for a
     * SLIP frame, the same role printk_unlock() plays for a printk() call. */
    uart_flush();
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

/* Incremental SLIP decode: one received byte at a time, straight into the
 * frame buffer (C5, plan/phase6_memory_and_processes.md).
 *
 * Both receive paths used to accumulate the *escaped* bytes in a raw[] buffer
 * and hand the whole thing to slip_decode() on SLIP_END. That was the right
 * first move -- it reused an already-tested decoder rather than writing a
 * second one -- but it cost a second buffer per context, sized at twice
 * P9_MAX_MSIZE because escaping can double a frame. Two contexts, 8 KB each,
 * on a board with 512 KB of SRAM.
 *
 * The state a SLIP decoder needs between bytes is one boolean, so the buffer
 * was never buying anything but the shape of the call. Returns 1 when a
 * complete frame is ready in `frame`, 0 otherwise.
 *
 * Behaviour matches the buffered version exactly, including the two cases
 * worth naming: a SLIP_END with nothing accumulated is a leading or duplicate
 * delimiter and is ignored, and an overflowing frame is dropped so the link
 * resynchronises on the next delimiter rather than emitting a truncated
 * message. */
static int slip_feed(uint8_t c, uint8_t *frame, uint32_t frame_cap,
                     uint32_t *frame_len, bool *escaping) {
    if (c == SLIP_END) {
        *escaping = false;
        if (*frame_len == 0) return 0;   /* leading/duplicate delimiter */
        return 1;
    }

    if (*escaping) {
        *escaping = false;
        if (c == SLIP_ESC_END) c = SLIP_END;
        else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
    } else if (c == SLIP_ESC) {
        *escaping = true;
        return 0;
    }

    if (*frame_len >= frame_cap) {
        *frame_len = 0;                  /* overflow: resync on the next END */
        return 0;
    }
    frame[(*frame_len)++] = c;
    return 0;
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

/* --- link_uart_demux (A3b) ---
 *
 * The state machine is a byte-routing variant of link_uart_slip's own
 * accumulator above: instead of assuming *every* byte belongs to a 9P
 * frame, it tracks whether it's currently inside an open frame (started by
 * a SLIP_END not yet matched by a closing one) and routes accordingly.
 * Bytes seen outside an open frame go to a small console ring instead of
 * being discarded, which is the one behavioral difference from headless
 * mode; everything else (accumulate-until-END, hand the whole thing to the
 * existing slip_decode() in one call) is unchanged.
 *
 * Known, accepted tradeoff: this framing has no reserved switch character
 * separate from SLIP's own END byte, so a console byte that happens to be
 * exactly 0xC0 would be misread as the start of a 9P frame, silently
 * swallowing keystrokes until the next 0xC0 (or a raw-buffer overflow)
 * resyncs it. Plain ASCII terminal input (letters, digits, punctuation,
 * control codes, and every VT100/xterm escape sequence kernel/line_editor.c
 * parses) never produces 0xC0, so this doesn't bite in practice -- but it's
 * why this whole path stays off by default (see uart_demux_set_enabled())
 * rather than being silently always-on. */
#define UART_DEMUX_CONSOLE_RING_CAP 256

typedef struct {
    uart_raw_has_char_fn raw_has_char;
    uart_raw_getc_fn raw_getc;
    bool enabled;

    uint8_t console_ring[UART_DEMUX_CONSOLE_RING_CAP];
    uint32_t console_head, console_tail;

    bool in_frame;
    bool escaping;          /* mid-escape across byte boundaries */
    uint8_t frame[P9_MAX_MSIZE];
    uint32_t frame_len;
    bool frame_ready;
} uart_demux_ctx_t;

static uart_demux_ctx_t g_demux;

static void demux_console_push(uint8_t c) {
    uint32_t next = (g_demux.console_head + 1) % UART_DEMUX_CONSOLE_RING_CAP;
    if (next == g_demux.console_tail) return; // ring full; drop (matches usb_cdc.c's EP2 precedent)
    g_demux.console_ring[g_demux.console_head] = c;
    g_demux.console_head = next;
}

static void demux_route_byte(uint8_t c) {
    if (c == SLIP_END) {
        if (g_demux.in_frame && g_demux.frame_len > 0) {
            g_demux.in_frame = false;
            g_demux.frame_ready = true;
        } else {
            g_demux.in_frame = true; // leading/duplicate END: (re)start the frame
            g_demux.frame_len = 0;
        }
        g_demux.escaping = false;
        return;
    }

    if (g_demux.in_frame) {
        uint32_t before = g_demux.frame_len;
        (void)slip_feed(c, g_demux.frame, sizeof(g_demux.frame),
                        &g_demux.frame_len, &g_demux.escaping);
        /* slip_feed() zeroes the length on overflow; the demux additionally
         * has to leave frame mode so subsequent bytes go back to the console
         * rather than being silently eaten until the next delimiter. */
        if (before > 0 && g_demux.frame_len == 0 && !g_demux.escaping) {
            g_demux.in_frame = false;
        }
    } else {
        demux_console_push(c);
    }
}

/* Drains every raw hardware byte currently available. Called from both the
 * console side (uart_demux_console_has_char()) and the p9_link_t's poll()
 * so whichever caller happens to run first keeps the wire moving -- there's
 * only one consumer of the underlying hardware register either way. */
/* B6: every entry point below runs with interrupts masked.
 *
 * Two independent callers reach this state machine -- the console via
 * uart_getc(), and the 9P server task via the link's poll() -- and before
 * preemption they were serialised for free by cooperative scheduling. A timer
 * interrupt can now land between reading a byte and routing it, so two
 * callers can interleave inside the frame accumulator and the console ring.
 * The symptom was not subtle: the p9share shared-wire test failed the moment
 * preemption was switched on, which is exactly the sort of race that would
 * otherwise have shown up as an occasional corrupted frame much later.
 *
 * Masking rather than a lock, for the same reasons as kernel/irq.h: the
 * regions are short and there is one hart. */
static void demux_pump_locked(void) {
    if (!g_demux.enabled || !g_demux.raw_has_char || !g_demux.raw_getc) return;
    while (g_demux.raw_has_char()) {
        demux_route_byte(g_demux.raw_getc());
    }
}


void uart_demux_init(uart_raw_has_char_fn has_char, uart_raw_getc_fn getc) {
    g_demux.raw_has_char = has_char;
    g_demux.raw_getc = getc;
}

void uart_demux_set_enabled(bool enabled) {
    if (enabled && !g_demux.enabled) {
        // Start clean: don't let bytes queued from before demux was armed
        // (or leftover from a previous session) surface as stale input.
        g_demux.console_head = g_demux.console_tail = 0;
        g_demux.in_frame = false;
        g_demux.escaping = false;
        g_demux.frame_ready = false;
        g_demux.frame_len = 0;
    }
    g_demux.enabled = enabled;
}

bool uart_demux_is_enabled(void) {
    return g_demux.enabled;
}

bool uart_demux_console_has_char(void) {
    uintptr_t f = irq_save();
    demux_pump_locked();
    bool r = g_demux.enabled && g_demux.console_head != g_demux.console_tail;
    irq_restore(f);
    return r;
}

char uart_demux_console_getc(void) {
    uintptr_t f = irq_save();
    char c = 0;
    if (g_demux.console_head != g_demux.console_tail) {
        c = (char)g_demux.console_ring[g_demux.console_tail];
        g_demux.console_tail = (g_demux.console_tail + 1) % UART_DEMUX_CONSOLE_RING_CAP;
    }
    irq_restore(f);
    return c;
}

static int uart_demux_poll(p9_link_t *link) {
    (void)link;
    uintptr_t f = irq_save();
    demux_pump_locked();
    int r = g_demux.frame_ready ? 1 : 0;
    irq_restore(f);
    return r;
}

/* Claiming the frame must be atomic with clearing frame_ready, or two pollers
 * can both decide a frame is theirs and one copies a buffer already being
 * overwritten by the next arrival. */
static int uart_demux_recv_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    uintptr_t f = irq_save();
    if (!g_demux.frame_ready) { irq_restore(f); return -1; }
    uint32_t n = (g_demux.frame_len < max_len) ? g_demux.frame_len : max_len;
    memcpy(buf, g_demux.frame, n);
    g_demux.frame_ready = false;
    g_demux.frame_len = 0;
    irq_restore(f);
    return (int)n;
}

static int uart_demux_send_frame(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    return uart_slip_send_frame(link, buf, len); // TX has no demux ambiguity; reuse as-is
}

static p9_link_t g_uart_demux_link = {
    .name = "uart-demux",
    .poll = uart_demux_poll,
    .send_frame = uart_demux_send_frame,
    .recv_frame = uart_demux_recv_frame,
    .ctx = NULL,
};

p9_link_t *uart_demux_get_link(void) {
    return &g_uart_demux_link;
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
