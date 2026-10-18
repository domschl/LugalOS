#include "kernel/time.h"
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
#include "drivers/lcdterm.h"
#include "drivers/reader_view.h"
#include "drivers/dsi_esp32p4.h"
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

bool console_lock_held(void) {
    return ylock_owner(&g_console_lock) == sched_context_id();
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

void console_print_divider(char ch) {
    unsigned cols = 80, rows = 24;
    if (!console_size(&cols, &rows) || cols == 0) cols = 80;
    if (cols > 72) cols = 72;
    if (cols > 2) cols -= 1;
    console_lock();
    for (unsigned i = 0; i < cols; i++) console_putc(ch);
    console_putc('\n');
    console_unlock();
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

bool console_pixels_format(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride, unsigned *format) {
    if (!g_screen) return false;
    if (g_screen->pixels_format) return g_screen->pixels_format(fb, w, h, stride, format);
    if (g_screen->pixels) {
        if (!g_screen->pixels(fb, w, h, stride)) return false;
        if (format) *format = SCREEN_FORMAT_1BPP;
        return true;
    }
    return false;
}

static volatile uint8_t g_active_win_type = 1; /* 1 = RIBBON_WIN_TERM */

void console_set_active_win_type(uint8_t type) {
    g_active_win_type = type;
}

uint8_t console_active_win_type(void) {
    return g_active_win_type;
}

static volatile unsigned g_hotkey;

/* --- 47.11: idle blanking ------------------------------------------------
 *
 * Input is the only activity that counts: a program printing to a blanked
 * panel leaves it blanked. Checked from the pump, which every task waiting
 * for input -- or polling for Ctrl-C -- runs, so a system with a shell at a
 * prompt anywhere checks it all the time. */
static volatile uint32_t g_blank_after_s;       /* 0: never */
static volatile uint64_t g_last_input_ms;
static volatile bool     g_blanked;
static uint32_t          g_blankings, g_wakes, g_wakes_swallowed;

static void blank_hook(bool off) {
    if (g_screen && g_screen->blank) g_screen->blank(off);
}

/* An input arrived. True if it only woke the panel and should be dropped. */
static bool note_input(bool local) {
    g_last_input_ms = time_get_ms();
    if (!g_blanked) return false;
    g_blanked = false;
    blank_hook(false);
    g_wakes++;
    if (local) g_wakes_swallowed++;
    return local;
}

static void blank_check(void) {
    uint32_t after = g_blank_after_s;
    if (!after || g_blanked) return;
    if (time_get_ms() - g_last_input_ms < (uint64_t)after * 1000u) return;
    g_blanked = true;
    blank_hook(true);
    g_blankings++;
}

void console_blank_set(uint32_t seconds) {
    g_last_input_ms = time_get_ms();
    g_blank_after_s = seconds;
    if (g_blanked) { g_blanked = false; blank_hook(false); }
}

uint32_t console_blank_after(void) { return g_blank_after_s; }
bool console_blanked(void) { return g_blanked; }

void console_blank_counts(uint32_t *blankings, uint32_t *wakes, uint32_t *swallowed) {
    *blankings = g_blankings;
    *wakes = g_wakes;
    *swallowed = g_wakes_swallowed;
}

static volatile uint32_t g_tap_xy;     /* x << 16 | y */
static volatile int      g_scroll_dy = 0;
static volatile uint32_t g_scroll_ms = 0;

void console_touch_tap(unsigned x, unsigned y) {
    if (note_input(true)) return;
    g_tap_xy = ((uint32_t)x << 16) | (y & 0xFFFFu);
    g_hotkey = CONSOLE_HOTKEY_TAP;
}

void console_touch_scroll(int dy, uint32_t ms) {
    if (note_input(true)) return;
    g_scroll_dy = dy;
    g_scroll_ms = ms;
    g_hotkey = dy < 0 ? CONSOLE_HOTKEY_SCROLL_DOWN : CONSOLE_HOTKEY_SCROLL_UP;
}

void console_hotkey(unsigned code) {
    /* Only the panel's keyboard makes hotkeys; one that wakes it does
     * nothing else. */
    if (note_input(true)) return;
    g_hotkey = code;
}

void console_sync_readers(void) {
    uint8_t req[2] = { 'R', 0 };
    uint8_t reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 1, reply) || reply[0] != 0) return;
    uint8_t count = reply[1];
    for (uint8_t i = 0; i < count; i++) {
        req[0] = 'R';
        req[1] = i;
        if (console_canvas(req, 2, reply) && reply[0] == 0) {
            if (reply[1] == 3 /* RIBBON_WIN_READER */) {
                uint8_t rid = reply[2];
                int rx0 = (int)(reply[4] | (reply[5] << 8));
                int rx1 = (int)(reply[6] | (reply[7] << 8));
                int rw = rx1 - rx0 - 1;
                int rh = (int)(reply[10] | (reply[11] << 8));
                if (rw > 0) {
                    if (reader_set_target(rid, rw, rh)) {
                        reader_update_instance(rid);
                    }
                }
            }
        }
    }
}

/* From an input wait only: a hotkey draws (a layout change) or writes 48 KB
 * to the SD card (a screenshot), neither of which belongs inside a print. */
static void run_hotkey(void) {
    /* Taken, not read then cleared: every task in an input wait calls this,
     * and two of them on two harts both saw one Super+Enter and opened two
     * terminals for it. */
    unsigned hk = __atomic_exchange_n(&g_hotkey, 0u, __ATOMIC_ACQ_REL);
    if (!hk) return;
    if (hk == CONSOLE_HOTKEY_SCREENSHOT) {
        char path[48];
        (void)screenshot_save(NULL, path, sizeof(path));
        return;
    }
    if (hk == CONSOLE_HOTKEY_NEW_TERM) {
        int vid = shell_spawn_terminal(NULL);
        if (vid >= 0) {
            /* 0 columns: the screen's own half (drivers/ribbon.h, D7) --
             * 48 on the RP2350's 800 px, 62 on the LCD-7B's 1024. */
            uint8_t req[3] = { 'N', (uint8_t)vid, 0 }, reply[SCREEN_REPLY_LEN];
            (void)console_canvas(req, 3, reply);
            g_active_win_type = 1;
            vterm_set_active(vid);
            console_sync_readers();
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_FOCUS_LEFT) {
        uint8_t req[1] = { '<' }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 1, reply) && reply[0] == 0) {
            if (reply[10] != 0) g_active_win_type = reply[10];
            if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
            vterm_set_active(reply[1]);
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_FOCUS_RIGHT) {
        uint8_t req[1] = { '>' }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 1, reply) && reply[0] == 0) {
            if (reply[10] != 0) g_active_win_type = reply[10];
            if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
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
            if (closed_type == 3 /* RIBBON_WIN_READER */) {
                reader_close_instance(closed_vid);
                g_active_win_type = 1;
            }
            vterm_set_active(reply[1]);
            console_sync_readers();
        }
        return;
    }
    if (hk >= CONSOLE_HOTKEY_JUMP_BASE && hk <= CONSOLE_HOTKEY_JUMP_BASE + 8) {
        uint8_t target_idx = (uint8_t)(hk - CONSOLE_HOTKEY_JUMP_BASE);
        uint8_t req[2] = { 'J', target_idx }, reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 2, reply) && reply[0] == 0) {
            if (reply[10] != 0) g_active_win_type = reply[10];
            if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
            vterm_set_active(reply[1]);
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_TAP) {
        uint32_t xy = g_tap_xy;
        uint8_t req[5] = { 'P', (uint8_t)(xy >> 16), (uint8_t)(xy >> 24), (uint8_t)xy, (uint8_t)(xy >> 8) };
        uint8_t reply[SCREEN_REPLY_LEN];
        if (console_canvas(req, 5, reply) && reply[0] == 0) {
            if (reply[8] == 0xCC) {
                uint8_t closed_type = reply[10];
                uint8_t closed_vid = reply[11];
                if (closed_type == 1 /* RIBBON_WIN_TERM */ && closed_vid > 0) {
                    vterm_destroy(closed_vid);
                }
                if (closed_type == 3 /* RIBBON_WIN_READER */) {
                    reader_close_instance(closed_vid);
                    g_active_win_type = 1;
                }
                vterm_set_active(reply[1]);
                console_sync_readers();
                return;
            }
            if (reply[10] != 0) g_active_win_type = reply[10];
            if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
            vterm_set_active(reply[1]);
            int cx = (int)(reply[2] | (reply[3] << 8));
            int cy = (int)(reply[4] | (reply[5] << 8));
            if ((cx != 0 || cy != 0) && reader_is_active()) {
                (void)reader_tap(cx, cy);
            }
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_SCROLL_UP || hk == CONSOLE_HOTKEY_SCROLL_DOWN) {
        if (reader_is_active()) {
            int dy = g_scroll_dy;
            uint32_t ms = g_scroll_ms;
            g_scroll_dy = 0;
            g_scroll_ms = 0;
            if (ms > 0 && dy != 0) {
                reader_touch_fling(dy, ms);
            } else {
                reader_scroll(hk == CONSOLE_HOTKEY_SCROLL_UP ? -160 : 160);
            }
        }
        return;
    }
    if (hk == CONSOLE_HOTKEY_TOGGLE_MAX) {
        uint8_t req[1] = { 'M' }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 1, reply);
        console_sync_readers();
        return;
    }
    if (hk == CONSOLE_HOTKEY_ORIENT) {
#if defined(CONFIG_BOARD_TAB5)
        int cur = dsi_lcd_get_orientation();
        int next = (cur == 0 || cur == 180) ? 270 : 0;
        dsi_lcd_set_orientation(next);
        console_sync_readers();
        if (reader_is_active()) {
            reader_update();
        }
#endif
        return;
    }
    uint8_t req[2] = { 'w', (uint8_t)(hk == CONSOLE_HOTKEY_RIGHT ? 1 : -1) }, reply[SCREEN_REPLY_LEN];
    (void)console_canvas(req, 2, reply);
    console_sync_readers();
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

/* The console stream flushes the
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
static int      g_cap_pid = -1;     /* Only its writes are kept */

void console_capture(char *buf, uint32_t cap) {
    g_cap_len = 0;
    g_cap_max = cap;
    g_cap_pid = sched_current_pid();
    g_cap = cap ? buf : NULL;
}

uint32_t console_capture_end(void) {
    uint32_t len = g_cap_len;
    if (g_cap) {
        if (g_cap_len < g_cap_max) {
            g_cap[g_cap_len] = '\0';
        } else if (g_cap_max > 0) {
            g_cap[g_cap_max - 1] = '\0';
        }
    }
    g_cap = NULL;
    return len;
}

/* The terminal the running task writes to and reads from. Its
 * vterm for any terminal but the root console; NULL for the root console
 * (vterm 0: the boot shell and what it started) -- and NULL with *closed set
 * for a task whose terminal was closed under it (task_orphan_vterm()). */
static vterm_t *task_terminal(bool *closed) {
    *closed = false;
    int pid = sched_current_pid();
    int vid = (pid >= 0) ? task_get_vterm(pid) : 0;
    if (vid == 0) return NULL;
    vterm_t *vt = (vid > 0) ? vterm_get(vid) : NULL;
    if (!vt) *closed = true;
    return vt;
}

/* A task of a closed terminal, waiting for input that cannot come. It ends
 * here if it holds no ylock -- this is a wait, so by contract not the
 * console's -- and otherwise is handed a Ctrl-C to unwind on, ending at a
 * later wait. Never from the output side: cprintf() writes with
 * console_lock() held, and a task ended there took the console with it --
 * `vterm close 1` typed in terminal 1 hung every shell (2026-10-04). */
static char closed_terminal_wait(void) {
    if (ylock_held_by(sched_current_pid()) == 0) task_exit();
    sched_yield();
    return 0x03;
}

static inline bool is_pid_captured(int pid, int cap_pid) {
    int depth = 0;
    while (pid >= 0 && depth < MAX_TASKS) {
        if (pid == cap_pid) return true;
        pid = task_get_parent(pid);
        depth++;
    }
    return false;
}

void console_putc(char c) {
    if (g_cap && is_pid_captured(sched_current_pid(), g_cap_pid)) {
        if (g_cap_len < g_cap_max) {
            g_cap[g_cap_len++] = c;
        }
        return;
    }
    bool closed;
    vterm_t *vt = task_terminal(&closed);
    if (closed) return;                         /* nobody to show it to */
    if (vt) vterm_write(vt, &c, 1);
    else vt = vterm_get(0);
    if (vt && !vt->active) {
#if defined(LCDTERM_PRESENT)
        lcdterm_screen_putc_vterm(vt->id, c);
#endif
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
    /* One request, one locked write (phase31 §7 R4), like cprintf(). */
    console_lock();
    for (uint32_t i = 0; i < req_len; i++) console_putc((char)req[i]);
    console_unlock();
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

/* --- Ctrl-C interrupt --- */

static volatile bool g_interrupt_pending = false;

/* Push-back ring. See kernel/console.h for why this exists and what it is
 * not. 128 bytes: enough for a few typed-ahead commands, small enough that
 * it can never be mistaken for a real input queue. How it is shared between
 * its producer and its consumer: see pushback_full() below. */
#define CONSOLE_PUSHBACK_SIZE 128u

static char     g_pushback[CONSOLE_PUSHBACK_SIZE];
static uint32_t g_pb_head;   /* next slot to write */
static uint32_t g_pb_tail;   /* next slot to read */

/* Single producer (console_pump(), under g_input_lock) and single consumer
 * (vterm 0's reader), so the indices are published, not locked: the byte is
 * written before the head that covers it is released, and read only after
 * that head is acquired (phase31 §7 T9). irq_save() alone ordered nothing
 * between two harts -- the reader on one could see the head move before
 * the byte it covers. */
static bool pushback_full(void) {
    uint32_t head = __atomic_load_n(&g_pb_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&g_pb_tail, __ATOMIC_ACQUIRE);
    return ((head + 1u) % CONSOLE_PUSHBACK_SIZE) == tail;
}

static void pushback_put(char c) {
    uint32_t head = __atomic_load_n(&g_pb_head, __ATOMIC_RELAXED);
    uint32_t next = (head + 1u) % CONSOLE_PUSHBACK_SIZE;
    if (next != __atomic_load_n(&g_pb_tail, __ATOMIC_ACQUIRE)) {   /* full: drop, but see console_pump() */
        g_pushback[head] = c;
        __atomic_store_n(&g_pb_head, next, __ATOMIC_RELEASE);
    }
}

/* -1 when empty. */
static int pushback_get(void) {
    uint32_t tail = __atomic_load_n(&g_pb_tail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&g_pb_head, __ATOMIC_ACQUIRE)) return -1;
    int c = (unsigned char)g_pushback[tail];
    __atomic_store_n(&g_pb_tail, (tail + 1u) % CONSOLE_PUSHBACK_SIZE, __ATOMIC_RELEASE);
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
    /* The latch belongs to the terminal being typed into. */
    vterm_t *act = vterm_active();
    if (act && act->id <= 0) act = NULL;
    for (unsigned i = 0; i < g_ninputs; i++) {
        if (g_inputs[i]->peek_interrupt && g_inputs[i]->peek_interrupt()) {
            if (act) act->interrupt_pending = true;
            else g_interrupt_pending = true;
        }
    }

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
    blank_check();
    for (unsigned i = 0; i < g_ninputs; i++) {
        const console_input_t *src = g_inputs[i];
        if (src->local && g_active_win_type == 3 /* RIBBON_WIN_READER */ && reader_is_active()) {
            while (src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (note_input(src->local)) continue;
                reader_feed_key(c);
            }
        } else if (act) {
            /* Room first, as for the ring: a byte taken from the device and
             * then refused by a full queue was lost. */
            while (vterm_has_room(act) && src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (note_input(src->local)) continue;
                if (!vterm_feed_char(act, (char)c)) break;
            }
        } else {
            while (!pushback_full() && src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (note_input(src->local)) continue;
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
    bool closed;
    vterm_t *vt = task_terminal(&closed);
    if (closed) return true;                    /* console_getc() ends it */
    if (vt) return vterm_has_char(vt);
    return __atomic_load_n(&g_pb_head, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_pb_tail, __ATOMIC_RELAXED);
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
    bool closed;
    if (task_terminal(&closed) || closed) {
        for (;;) {
            vterm_t *vt = task_terminal(&closed);
            if (closed) return closed_terminal_wait();
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
    bool closed;
    vterm_t *vt = task_terminal(&closed);
    if (vt) vterm_ungetc(vt, c);
    else if (!closed) {
        ylock_acquire(&g_input_lock);   /* the ring's one producer is the pump */
        pushback_put(c);
        ylock_release(&g_input_lock);
    }
}


bool console_interrupt_requested(void) {
    console_pump();
    bool closed;
    vterm_t *vt = task_terminal(&closed);
    if (closed) return true;                    /* stop: nobody is watching */
    /* Read, not cleared, as for the root console: the latch stays up until
     * console_interrupt_clear(), so two pollers of one command both see it. */
    if (vt) return vt->interrupt_pending;
    return g_interrupt_pending;
}

void console_interrupt_clear(void) {
    bool closed;
    vterm_t *vt = task_terminal(&closed);
    if (vt) (void)vterm_check_interrupt(vt);
    else if (!closed) g_interrupt_pending = false;
}
