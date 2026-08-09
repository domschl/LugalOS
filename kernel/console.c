#include "kernel/console.h"
#include "kernel/chan.h"
#include "kernel/device.h"
#include "kernel/printk.h"
#include <string.h>

/* See kernel/include/kernel/console.h. The formatting engine lives in
 * kernel/printk.c and is shared; only the destination differs. */

static console_putc_fn g_console_putc;

void console_bind(console_putc_fn putc) {
    g_console_putc = putc;
}

void console_putc(char c) {
    if (g_console_putc) g_console_putc(c);
}

void console_puts(const char *s) {
    if (!s) return;
    while (*s) console_putc(*s++);
}

/* --- The console as a channel endpoint --- */

static const char *g_bound_name = "(none)";

/* Sized to one line of output rather than a whole screen: a console write is
 * a message, and a caller with more to say sends more messages. Keeping this
 * small matters on RP2350, where the heap is 18 pages. */
static uint8_t g_console_req[256];
static uint8_t g_console_resp[8];

static int console_chan_handler(void *ctx, const uint8_t *req, uint32_t req_len,
                                uint8_t *resp, uint32_t resp_max) {
    (void)ctx;
    /* `req` is endpoint-owned memory that chan_call() copied into -- never
     * the caller's buffer. Rule 1 (§5.1), and the reason a remote node can
     * drive this endpoint over 9P without the console knowing the difference. */
    for (uint32_t i = 0; i < req_len; i++) console_putc((char)req[i]);
    if (resp_max >= 1) { resp[0] = 0; return 1; }
    return 0;
}

int console_server_init(void) {
    int rc = chan_register("console", console_chan_handler, NULL,
                           g_console_req, sizeof(g_console_req),
                           g_console_resp, sizeof(g_console_resp));
    if (rc == 0) {
        printk("[Console] Server endpoint '/srv/console' online.\n");
    }
    return rc;
}

int console_bind_device(const char *name) {
    console_dev_t *d = (console_dev_t *)dev_get(name, DEV_KIND_CONSOLE);
    if (!d || !d->putc) return -1;
    console_bind(d->putc);
    g_bound_name = name;
    printk("[Console] Output bound to device '%s'\n", name);
    return 0;
}

const char *console_bound_device(void) { return g_bound_name; }
