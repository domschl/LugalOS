#include "kernel/console.h"
#include "kernel/screenshot.h"
#include "drivers/screen.h"
#include "kernel/klog.h"
#include "kernel/chan.h"
#include "kernel/device.h"
#include "kernel/printk.h"
#include "drivers/uart.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
#include "kernel/vterm.h"
#include "drivers/lcd7.h"
#include <string.h>

/* See kernel/include/kernel/console.h. The formatting engine lives in
 * kernel/printk.c and is shared; only the destination differs. */

static console_putc_fn g_console_putc;

/* See kernel/console.h. A ylock, because it is held across the block that a
 * console write ends in. */
static ylock_t g_console_lock;

void console_lock(void) {
    /* The rule G2 added, now pointing at what it actually guards (§5.6): a
     * driver serve callback must not write the console, because this path
     * ends in chan_call() to the uart task and a caller blocked on that
     * callback's own endpoint closes the cycle. printk() is safe there and no
     * longer comes through here. */
    (void)lock_check_may_console();
    ylock_acquire(&g_console_lock);
}

void console_unlock(void) {
    ylock_release(&g_console_lock);
}

static const console_screen_t *g_screen;

void console_set_screen(const console_screen_t *screen) {
    g_screen = screen;
}

static void screen_flush(void) {
    if (g_screen && g_screen->flush) g_screen->flush();
}

void console_flush(void) {
    uart_flush();
    screen_flush();
}

bool console_size(unsigned *cols, unsigned *rows) {
    return g_screen && g_screen->size && g_screen->size(cols, rows);
}

void console_set_title(const char *title) {
    if (g_screen && g_screen->set_title) g_screen->set_title(title);
}

bool console_canvas(const uint8_t *req, uint32_t n, uint8_t *reply) {
    return g_screen && g_screen->canvas && g_screen->canvas(req, n, reply);
}

bool console_pixels(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride) {
    return g_screen && g_screen->pixels && g_screen->pixels(fb, w, h, stride);
}

static volatile unsigned g_hotkey;

void console_hotkey(unsigned code) {
    g_hotkey = code;
}

/* From an input wait only: a hotkey draws (a layout change) or writes 48 KB
 * to the SD card (a screenshot), neither of which belongs inside a print. */
static void run_hotkey(void) {
    unsigned hk = g_hotkey;
    if (!hk) return;
    g_hotkey = 0;
    if (hk == CONSOLE_HOTKEY_SCREENSHOT) {
        char path[48];
        (void)screenshot_save(NULL, path, sizeof(path));
        return;
    }
    if (hk == CONSOLE_HOTKEY_NEW_TERM) {
        int vid = shell_spawn_terminal(NULL);
        if (vid >= 0) {
            uint8_t req[3] = { 'N', (uint8_t)vid, 48 }, reply[SCREEN_REPLY_LEN];
            (void)console_canvas(req, 3, reply);
            vterm_set_active(vid);
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_FOCUS_LEFT) {
        uint8_t req[1] = { '<' }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 1, reply) && reply[0] == 0) {
            vterm_set_active(reply[1]);
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_FOCUS_RIGHT) {
        uint8_t req[1] = { '>' }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 1, reply) && reply[0] == 0) {
            vterm_set_active(reply[1]);
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_MOVE_LEFT) {
        uint8_t req[1] = { '[' }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 1, reply);
        return;
    }
    if (hk == CONSOLE_HOTKEY_MOVE_RIGHT) {
        uint8_t req[1] = { ']' }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 1, reply);
        return;
    }
    if (hk == CONSOLE_HOTKEY_CLOSE) {
        uint8_t req[1] = { 'C' }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 1, reply) && reply[0] == 0) {
            uint8_t closed_type = reply[10];
            uint8_t closed_vid = reply[11];
            if (closed_type == 1 /* RIBBON_WIN_TERM */ && closed_vid > 0) {
                vterm_destroy(closed_vid);
            }
            vterm_set_active(reply[1]);
        }
        return;
    }
    if (hk >= CONSOLE_HOTKEY_JUMP_BASE && hk <= CONSOLE_HOTKEY_JUMP_BASE + 8) {
        uint8_t target_idx = (uint8_t)(hk - CONSOLE_HOTKEY_JUMP_BASE);
        uint8_t req[2] = { 'J', target_idx }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 2, reply) && reply[0] == 0) {
            vterm_set_active(reply[1]);
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_SWAP) {
        uint8_t req[1] = { 'X' }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 1, reply);
        return;
    }
    uint8_t req[2] = { 'w', (uint8_t)(hk == CONSOLE_HOTKEY_RIGHT ? 1 : -1) }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);
}

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

/* Y5c, plan/phase31_concurrency_hierarchy.md: the console stream flushes the
 * kernel log before writing -- but only at the start of a whole write, never
 * between two characters of one.
 *
 * printk() is asynchronous now, so the log and the console are two streams
 * with different latencies and they need a sync point. Putting it in
 * console_putc() looked more thorough and was wrong twice over. It inserted
 * the backlog between the 'O' and the 'K' of a U-mode program's "UMODE_OK";
 * gating that on a line boundary fixed the splice and left the worse half,
 * which is that klog_drain() can *block*. A task that blocks mid-string may
 * resume on the other hart, and the TX batch is per hart -- so half the
 * string sat in hart 1's batch and half in hart 0's, and the flush at the end
 * emptied only one. That is why this is here and not one level down.
 *
 * klog_drain() is a cheap no-op when the log is already drained, and refuses
 * outright in any context that must not block -- see kernel/klog.c. It does
 * not recurse: the drain writes to klog's *sinks* (console_emit(uart_putc,
 * ...)), not back through here. */
/* 37.5b: console_capture() -- the last `cap` bytes written, kept instead
 * of shown. */
static char    *g_cap;
static uint32_t g_cap_max, g_cap_len;

void console_capture(char *buf, uint32_t cap) {
    g_cap_len = 0;
    g_cap_max = cap;
    g_cap = cap ? buf : NULL;
}

uint32_t console_capture_end(void) {
    g_cap = NULL;
    return g_cap_len;
}

void console_putc(char c) {
    if (g_cap) {
        if (g_cap_len == g_cap_max) {           /* full: keep the newer half */
            uint32_t h = g_cap_max / 2u;
            memmove(g_cap, g_cap + h, g_cap_max - h);
            g_cap_len = g_cap_max - h;
        }
        g_cap[g_cap_len++] = c;
        return;
    }
    vterm_t *vt = vterm_current();
    if (vt) {
        if (vt->id > 0) vterm_write(vt, &c, 1);
        if (vt->active) {
            console_emit(g_console_putc, c);
        } else {
#if defined(CONFIG_LCD_PCLK_GPIO)
            lcd7_screen_putc_vterm(vt->id, c);
#endif
        }
        return;
    }
    console_emit(g_console_putc, c);
}

/* The sync point between the log stream and the console stream, for callers
 * that are at a message boundary and know it.
 *
 * This used to live inside console_puts(), one level down, and that was the
 * second wrong home for it (Y5c moved it out of console_putc(), where it was
 * splicing "UMODE_OK" between the O and the K). console_puts() is not a
 * message boundary -- it is a string-emitting primitive, and its callers use
 * it *mid-message*:
 *
 *   - kernel/line_editor.c calls it from thirty sites, a dozen of them in one
 *     prompt redraw ("\033[?25l", "\r", the prompt, "\033[K", cursor moves,
 *     "\033[?25h"). A drain between two escape sequences of one redraw can
 *     emit a whole klog record into the middle of an echoed line -- and can
 *     block there, which is worse.
 *   - cprintf()'s format engine calls it for exactly one thing, the timestamp
 *     prefix, emitted as "[", the width padding, then "] ". A record flushed
 *     between those lands between the bracket and its own closing bracket.
 *
 * So the drain moves up to the callers that really are whole writes. The
 * property Y5c wanted -- the backlog appears before a new message, never
 * inside one -- is what this name now means, and it is stated once here
 * instead of being implied by where a call happened to sit. */
void console_sync(void) {
    klog_drain();
}

void console_puts(const char *s) {
    if (!s) return;
    /* Neither drains nor locks.
     *
     * Not locking is Y5d: taking console_lock() here looked symmetric with
     * cprintf() and broke the two-hart boot outright -- deterministically,
     * both runs, the SMP target never reaching a shell. This function is
     * reached from inside cprintf()'s format engine, which already holds the
     * lock, and from SYS_PRINT; the first needs no second acquire and the
     * second is a whole syscall.
     *
     * Not draining is the correction above: see console_sync(). */
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
    /* Release the previous binding's wire first, so moving the console frees
     * what it was on rather than holding both -- and so a move between two
     * devices on the *same* wire is not refused as a conflict with itself.
     * 36.6's `lcd` console tees to UART0 and so claims that wire, exactly as
     * `uart` does; claiming before releasing made uart -> lcd impossible. If
     * the new claim fails, the old one is put back and nothing changes. */
    const char *prev = g_bound_name;
    bool moving = prev && strcmp(prev, name) != 0;
    if (moving) dev_release(prev);
    if (dev_claim(name) != 0) {
        if (moving) (void)dev_claim(prev);
        return -1;
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
/* Serialises every read of the input device (2026-09-30).
 *
 * The pump's `uart_has_char()` then `uart_getc()` is a check-then-act, and two
 * tasks run it: whoever is reading a line, and anything polling for Ctrl-C --
 * which includes background tasks (mqttd's connect loop calls
 * console_interrupt_requested(), net/mqtt.c). Without this lock the second
 * reader could see a byte, lose it to the first between the two calls, and
 * then sit in a *blocking* uart_getc() until someone typed again. Found by
 * the test harness once its completion sentinel started arriving just as
 * mqttd connected: mqttd hung silently -- no connect, no error -- about two
 * runs in three, and on hardware typing at the shell while mqttd connects
 * would do the same. A ylock, because uart_getc() may block (a chan_call to
 * the uart task on RP2350). Zero-initialised, like g_console_lock. */
static ylock_t g_input_lock;

/* 36.9: the registered input sources (kernel/include/kernel/console.h). */
static const console_input_t *g_inputs[CONSOLE_INPUT_MAX];
static unsigned               g_ninputs;

int console_input_register(const console_input_t *src) {
    if (!src || !src->has_char || !src->getc || g_ninputs >= CONSOLE_INPUT_MAX) return -1;
    g_inputs[g_ninputs] = src;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_ninputs++;
    return 0;
}

uint32_t console_indicators(char *buf, uint32_t cap) {
    uint32_t n = 0;
    if (cap == 0) return 0;
    for (unsigned i = 0; i < g_ninputs; i++) {
        const char *s = g_inputs[i]->indicator ? g_inputs[i]->indicator() : NULL;
        if (!s || !*s) continue;
        if (n > 0) for (int k = 0; k < 2 && n + 1 < cap; k++) buf[n++] = ' ';
        while (*s && n + 1 < cap) buf[n++] = *s++;
    }
    buf[n] = '\0';
    return n;
}

static void console_pump(void) {
    ylock_acquire(&g_input_lock);
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
    for (unsigned i = 0; i < g_ninputs; i++)
        if (g_inputs[i]->peek_interrupt && g_inputs[i]->peek_interrupt()) g_interrupt_pending = true;

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
     * room.
     *
     * 36.9: sources in registration order, each drained as far as the ring
     * allows. There is no fairness to arrange: a person types on one of
     * them at a time, and a script sending a block on another is exactly
     * what the ring bound above already handles. */
    vterm_t *act = vterm_active();
    for (unsigned i = 0; i < g_ninputs; i++) {
        const console_input_t *src = g_inputs[i];
        if (act && act->id > 0) {
            while (src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (!vterm_feed_char(act, (char)c)) break;
            }
        } else {
            while (!pushback_full() && src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (c == 0x03) g_interrupt_pending = true;
                pushback_put((char)c);
            }
        }
    }
    ylock_release(&g_input_lock);
}

bool console_has_char(void) {
    /* A reader is about to look for input: whatever was echoed must be on
     * the screen first (uart_getc() flushes its own batch the same way). */
    screen_flush();
    console_pump();
    run_hotkey();
    vterm_t *vt = vterm_current();
    if (vt && vt->id > 0) {
        return vterm_has_char(vt);
    }
    uintptr_t f = irq_save();
    bool queued = (g_pb_head != g_pb_tail);
    irq_restore(f);
    return queued;
}

char console_getc(void) {
    /* Every device read goes through the pump, under g_input_lock. This used
     * to fall through to a bare uart_getc() when nothing was queued -- a
     * second, unlocked reader, and the half of the race described at
     * g_input_lock that took the byte out from under the pump. Polling with
     * a yield is what uart_getc() does internally on every target anyway.
     *
     * The screen is flushed on every turn, not only on the way in (see
     * console_has_char()): an empty flush is one comparison, and the turns
     * of a wait are when the status bar's clock gets to move (37.1). */
    vterm_t *vt = vterm_current();
    if (vt && vt->id > 0) {
        for (;;) {
            screen_flush();
            console_pump();
            run_hotkey();
            int c = vterm_getc_nonblock(vt);
            if (c >= 0) return (char)c;
            sched_yield();
        }
    }
    for (;;) {
        screen_flush();
        console_pump();
        run_hotkey();
        int c = pushback_get();
        if (c >= 0) return (char)c;
        sched_yield();
    }
}

void console_ungetc(char c) {
    vterm_t *vt = vterm_current();
    if (vt && vt->id > 0) {
        vterm_ungetc(vt, c);
        return;
    }
    pushback_put(c);
}


bool console_interrupt_requested(void) {
    console_pump();
    vterm_t *vt = vterm_current();
    if (vt && vt->id > 0) {
        return vterm_check_interrupt(vt);
    }
    return g_interrupt_pending;
}

void console_interrupt_clear(void) {
    vterm_t *vt = vterm_current();
    if (vt && vt->id > 0) {
        (void)vterm_check_interrupt(vt);
    }
    g_interrupt_pending = false;
}
