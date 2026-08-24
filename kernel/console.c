#include "kernel/console.h"
#include "kernel/chan.h"
#include "kernel/device.h"
#include "kernel/printk.h"
#include "drivers/uart.h"
#include "kernel/irq.h"
#include <string.h>

/* See kernel/include/kernel/console.h. The formatting engine lives in
 * kernel/printk.c and is shared; only the destination differs. */

static console_putc_fn g_console_putc;

void console_bind(console_putc_fn putc) {
    g_console_putc = putc;
}

/* See kernel/include/kernel/console.h for why the CRLF convention lives on
 * this stream rather than in the format engine or the UART driver. */
void console_emit(console_putc_fn out, char c) {
    if (!out) return;
    if (c == '\n') out('\r');
    out(c);
}

void console_putc(char c) {
    console_emit(g_console_putc, c);
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

    /* Claim the wire before taking it (C8). Several device names can be the
     * same physical channel -- the UART is a console, a dedicated 9P link and
     * a demultiplexed one under three names -- and binding the console to a
     * wire something else is already driving used to be silently allowed. */
    if (dev_claim(name) != 0) return -1;

    /* Release the previous binding's wire, so moving the console frees what it
     * was on rather than holding both. */
    if (g_bound_name && strcmp(g_bound_name, name) != 0) {
        dev_release(g_bound_name);
    }

    console_bind(d->putc);
    g_bound_name = name;
    printk("[Console] Output bound to device '%s'\n", name);
    return 0;
}

const char *console_bound_device(void) { return g_bound_name; }

/* --- Ctrl-C interrupt (J2, plan/phase10_chess_completion.md) --- */

static volatile bool g_interrupt_pending = false;

/* Push-back ring. See kernel/console.h for why this exists and what it is
 * not. 128 bytes: enough for a few typed-ahead commands, small enough that
 * it can never be mistaken for a real input queue.
 *
 * Guarded with irq_save()/irq_restore() rather than left bare. The producer
 * (console_interrupt_requested(), from whichever task is running a long
 * command) and the consumer (console_getc(), from whichever task is
 * reading a line) are the same task in every path today, but preemption is
 * on -- and the whole point of this ring is to be written from inside a
 * hot loop that can be interrupted at any instruction. */
#define CONSOLE_PUSHBACK_SIZE 128u

static char     g_pushback[CONSOLE_PUSHBACK_SIZE];
static uint32_t g_pb_head;   /* next slot to write */
static uint32_t g_pb_tail;   /* next slot to read */

static bool pushback_full(void) {
    uintptr_t f = irq_save();
    bool full = (((g_pb_head + 1u) % CONSOLE_PUSHBACK_SIZE) == g_pb_tail);
    irq_restore(f);
    return full;
}

static void pushback_put(char c) {
    uintptr_t f = irq_save();
    uint32_t next = (g_pb_head + 1u) % CONSOLE_PUSHBACK_SIZE;
    if (next != g_pb_tail) {          /* full: drop, but see console_pump() */
        g_pushback[g_pb_head] = c;
        g_pb_head = next;
    }
    irq_restore(f);
}

/* -1 when empty. */
static int pushback_get(void) {
    uintptr_t f = irq_save();
    int c = -1;
    if (g_pb_head != g_pb_tail) {
        c = (unsigned char)g_pushback[g_pb_tail];
        g_pb_tail = (g_pb_tail + 1u) % CONSOLE_PUSHBACK_SIZE;
    }
    irq_restore(f);
    return c;
}

/* The single point where bytes leave the device, so there is exactly one
 * policy about what a Ctrl-C is.
 *
 * Everything that reads console input funnels through here: the interrupt
 * poll, console_has_char(), console_getc(). 0x03 is latched and never
 * delivered onward; every other byte queues. Having one pump is what makes
 * that statement true regardless of *who* reads first -- which matters
 * because both a line reader and an interrupt poll can be live at the same
 * time (see chess_next_event(), user/chess/src/chess_ui.c). With separate
 * drain paths, whichever ran first would decide the byte's fate, and a
 * Ctrl-C consumed by the line editor -- which has no handling for it -- would
 * simply vanish. */
static void console_pump(void) {
    /* The interrupt latch comes FIRST, and deliberately not inside the drain
     * loop below.
     *
     * The loop stops when the ring is full, which is correct for *data* (see
     * the second note below) and was catastrophic for *interrupts*: with the
     * latch inside it, a full ring meant Ctrl-C could never be latched again
     * for the rest of the boot. Found on hardware, 2026-08-24, on the clock
     * appliance -- which is exactly where it hurts, because nothing drains
     * the ring while a long-running program owns the console, so the ring
     * stays full and the program becomes uninterruptible. The 127 bytes that
     * filled it were 9P `Tversion` port-probe frames (tests/hw/rp2350.py
     * writes one to each candidate port to classify it, and its comment
     * calls them "harmless line noise" on the console -- they are harmless
     * as data and were not harmless as occupancy).
     *
     * uart_peek_interrupt() answers without consuming, so this neither eats
     * input nor depends on having room to store it. Where a device cannot be
     * inspected that way it answers false, and the old behaviour stands. */
    if (uart_peek_interrupt()) g_interrupt_pending = true;

    /* Moves device bytes into the ring, latching Ctrl-C on the way past.
     *
     * Two things, and it is worth being precise about why both:
     *
     * **It latches 0x03 but still queues it.** An earlier version treated
     * Ctrl-C as "a signal, never data" and dropped the byte. That broke
     * edit_multiline_box() (kernel/line_editor.c), whose documented exit is
     * Ctrl-X Ctrl-C -- it reads the 0x03 as an ordinary character, so
     * swallowing it made the editor impossible to leave and it redrew
     * forever. Latching at the pump, rather than at whoever happens to read
     * first, is what makes the latch reliable *without* having to steal the
     * byte: a poller sees the interrupt even if a line reader consumes the
     * character, because the two no longer compete for it.
     *
     * **It stops when the ring is full**, leaving the rest in the device.
     * That bound is the whole correctness of the pump, not a refinement: an
     * earlier version drained the device dry on every call and silently
     * destroyed everything past the 128th byte -- reintroducing, one layer
     * down, exactly the input-eating this ring exists to fix. The test
     * runner submits multi-line command blocks well over that size, and
     * their tails vanished. Surplus left in the device is also the right
     * back-pressure: the UART's own buffering holds it until a reader makes
     * room. */
    while (!pushback_full() && uart_has_char()) {
        char c = uart_getc();
        if (c == 0x03) g_interrupt_pending = true;
        pushback_put(c);
    }
}

bool console_has_char(void) {
    console_pump();
    uintptr_t f = irq_save();
    bool queued = (g_pb_head != g_pb_tail);
    irq_restore(f);
    return queued;
}

char console_getc(void) {
    console_pump();
    int c = pushback_get();
    if (c >= 0) return (char)c;

    /* Nothing queued: block on the device, latching a Ctrl-C that arrives
     * this way too, and still handing it on. */
    char raw = uart_getc();
    if (raw == 0x03) g_interrupt_pending = true;
    return raw;
}

void console_ungetc(char c) {
    pushback_put(c);
}


bool console_interrupt_requested(void) {
    console_pump();
    return g_interrupt_pending;
}

void console_interrupt_clear(void) {
    g_interrupt_pending = false;
}
