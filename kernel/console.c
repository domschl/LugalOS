#include "kernel/orient.h"
#include "kernel/time.h"
#include "kernel/console.h"
#include "drivers/view.h"
#include "kernel/keyseq.h"
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
#include "drivers/launcher_view.h"
#include "drivers/osk.h"
#include "drivers/touch.h"
#include "drivers/usbkbd_task.h"
#include "drivers/dsi_esp32p4.h"
#include "kernel/event.h"
#include "kernel/shell.h"
#include "drivers/winclose.h"
#include <string.h>

/* See kernel/include/kernel/console.h. The formatting engine lives in
 * kernel/printk.c and is shared; only the destination differs. */

/* accepted: one pointer store each, by `console bind`. */
static console_putc_fn g_console_putc;

/* See kernel/console.h. A ylock, because it is held across the block that a
 * console write ends in. */
static ylock_t g_console_lock;

/* guarded by: g_console_lock -- written by its holder, from the outermost
 * acquire to the final release. How long the lock is held, in power-of-two
 * millisecond buckets (`lockcheck`): every terminal waits behind a long hold. */
static uint64_t    g_con_t0;
static const char *g_con_site;
static uint32_t    g_con_hist[CONSOLE_HOLD_BUCKETS];
static uint32_t    g_con_max_us;
static int         g_con_max_pid = -1;
static const char *g_con_max_site;
#define CON_LONG_RING 4u
static struct { const char *site; uint32_t ms; int pid; } g_con_long[CON_LONG_RING];
static uint32_t    g_con_long_n;   /* guarded by: g_console_lock, as the above */

void console_lock_at(const char *site) {
    /* The rule G2 added, now pointing at what it actually guards (§5.6): a
     * driver serve callback must not write the console, because this path
     * ends in chan_call() to the uart task and a caller blocked on that
     * callback's own endpoint closes the cycle. printk() is safe there and no
     * longer comes through here. */
    (void)lock_check_may_console();
    ylock_acquire(&g_console_lock);
    if (ylock_depth(&g_console_lock) == 1) { g_con_t0 = time_get_us(); g_con_site = site; }
}

void console_unlock(void) {
    if (ylock_depth(&g_console_lock) == 1 && ylock_owner(&g_console_lock) == sched_context_id()) {
        uint32_t us = (uint32_t)(time_get_us() - g_con_t0);
        unsigned b = 0;
        for (uint32_t ms = us / 1000u; ms && b + 1u < CONSOLE_HOLD_BUCKETS; ms >>= 1) b++;
        g_con_hist[b]++;
        int pid = sched_current_pid();
        if (us > g_con_max_us) { g_con_max_us = us; g_con_max_pid = pid; g_con_max_site = g_con_site; }
        if (us >= CONSOLE_LONG_HOLD_MS * 1000u) {
            uint32_t k = g_con_long_n++ % CON_LONG_RING;
            g_con_long[k].site = g_con_site; g_con_long[k].ms = us / 1000u; g_con_long[k].pid = pid;
        }
    }
    ylock_release(&g_console_lock);
}

void console_lock_hold_stats(uint32_t hist[CONSOLE_HOLD_BUCKETS], uint32_t *max_us, int *max_pid,
                             const char **max_site) {
    for (unsigned i = 0; i < CONSOLE_HOLD_BUCKETS; i++) hist[i] = g_con_hist[i];
    *max_us = g_con_max_us;
    *max_pid = g_con_max_pid;
    *max_site = g_con_max_site;
}

/* single task: set once, by the panel's terminal start. */
static void (*g_frame_lock_fn)(void);
static void (*g_frame_unlock_fn)(void);

void console_set_frame_lock(void (*lock)(void), void (*unlock)(void)) {
    g_frame_lock_fn = lock;
    g_frame_unlock_fn = unlock;
}
void console_frame_lock(void)   { if (g_frame_lock_fn) g_frame_lock_fn(); }
void console_frame_unlock(void) { if (g_frame_unlock_fn) g_frame_unlock_fn(); }

bool console_lock_long_hold(unsigned i, const char **site, uint32_t *ms, int *pid) {
    uint32_t n = g_con_long_n < CON_LONG_RING ? g_con_long_n : CON_LONG_RING;
    if (i >= n) return false;
    uint32_t k = (g_con_long_n - 1u - i) % CON_LONG_RING;
    *site = g_con_long[k].site; *ms = g_con_long[k].ms; *pid = g_con_long[k].pid;
    return true;
}

bool console_lock_held(void) {
    return ylock_owner(&g_console_lock) == sched_context_id();
}

/* single task: the boot, before any reader runs; constant after. */
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

static void ws_deliver(void);

bool console_canvas(const uint8_t *req, uint32_t n, uint8_t *reply) {
    bool ok = g_screen && g_screen->canvas && g_screen->canvas(req, n, reply);
    ws_deliver();                                        /* what the request changed, told */
    return ok;
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

/* accepted: one byte, stored by the ribbon's focus changes (hotkeys, the
 * reader) and read by the input path; the last focus change wins. */
static volatile uint8_t g_active_win_type = 1; /* 1 = RIBBON_WIN_TERM */

void console_set_active_win_type(uint8_t type) {
    g_active_win_type = type;
}

uint8_t console_active_win_type(void) {
    return g_active_win_type;
}

/* accepted: a one-word mailbox. The keyboard, touch and line editor post into
 * it (a tap's or scroll's details first, then the word), the pump takes it
 * with an atomic exchange; a second hotkey before the pump runs replaces the
 * first. */
static volatile unsigned g_hotkey;

/* Focus and visibility (console_windows_now()). The screen reports its
 * windows under console_lock after each request; the changes are queued in
 * order and delivered after the request, one deliverer at a time, so a
 * window never hears LOST and GAINED the wrong way round. */
#define WS_MAX   RIBBON_MAX_WINDOWS
#define WS_QUEUE 32u
static console_win_state_t g_ws_now[WS_MAX];     /* guarded by: g_console_lock; as reported */
static unsigned            g_ws_now_n;           /* guarded by: g_console_lock */
static console_win_state_t g_ws_told[WS_MAX];    /* guarded by: g_console_lock; as last told */
static unsigned            g_ws_told_n;          /* guarded by: g_console_lock */
static struct { uint8_t type, id, ev; } g_ws_q[WS_QUEUE];   /* guarded by: g_ws_q_lock */
static uint32_t            g_ws_head, g_ws_tail;  /* guarded by: g_ws_q_lock */
static spinlock_t          g_ws_q_lock = SPINLOCK_INIT;
static ylock_t             g_ws_deliver_lock;     /* one deliverer: the queue's order kept */
static volatile bool       g_ws_blank_changed;    /* accepted: set by the blanking, taken by the input path */
/* accepted: as the blanking's other state below (one store, from several tasks) */
static volatile bool       g_blanked;

static void ws_queue(uint8_t type, uint8_t id, uint8_t ev) {
    uintptr_t f = spin_lock_irqsave(&g_ws_q_lock);
    if (g_ws_head - g_ws_tail >= WS_QUEUE) g_ws_tail++;      /* full: the oldest goes */
    uint32_t k = g_ws_head++ % WS_QUEUE;
    g_ws_q[k].type = type;
    g_ws_q[k].id = id;
    g_ws_q[k].ev = ev;
    spin_unlock_irqrestore(&g_ws_q_lock, f);
}

static uint8_t ws_told_flags(uint8_t type, uint8_t id) {
    for (unsigned i = 0; i < g_ws_told_n; i++)
        if (g_ws_told[i].type == type && g_ws_told[i].id == id) return g_ws_told[i].flags;
    return 0;                                            /* new: neither focused nor shown yet */
}

static bool ws_was_told(uint8_t type, uint8_t id) {
    for (unsigned i = 0; i < g_ws_told_n; i++)
        if (g_ws_told[i].type == type && g_ws_told[i].id == id) return true;
    return false;
}

/* Not an event type: a new window, told the system's state (sys_hello()). */
#define WS_HELLO 0xFFu

/* Under console_lock: what changed since the last telling, queued -- the
 * losses first, then the gains. A blanked screen shows nothing. */
static void ws_compare(void) {
    console_win_state_t eff[WS_MAX];
    unsigned n = g_ws_now_n;
    for (unsigned i = 0; i < n; i++) {
        eff[i] = g_ws_now[i];
        if (g_blanked) eff[i].flags &= (uint8_t)~CONSOLE_WIN_VISIBLE;
    }
    for (unsigned i = 0; i < n; i++)
        if (!ws_was_told(eff[i].type, eff[i].id)) ws_queue(eff[i].type, eff[i].id, WS_HELLO);
    for (int gains = 0; gains < 2; gains++) {
        for (unsigned i = 0; i < n; i++) {
            uint8_t was = ws_told_flags(eff[i].type, eff[i].id), is = eff[i].flags;
            uint8_t lost = (uint8_t)(was & ~is), got = (uint8_t)(is & ~was);
            uint8_t m = gains ? got : lost;
            if (m & CONSOLE_WIN_FOCUSED)
                ws_queue(eff[i].type, eff[i].id, gains ? WIN_EV_FOCUS_GAINED : WIN_EV_FOCUS_LOST);
            if (m & CONSOLE_WIN_VISIBLE)
                ws_queue(eff[i].type, eff[i].id, gains ? WIN_EV_RESUME : WIN_EV_SUSPEND);
        }
    }
    for (unsigned t = 0; t < g_ws_told_n; t++) {        /* canvas windows gone: nobody reads them now */
        if (g_ws_told[t].type != 2 /* RIBBON_WIN_CANVAS */) continue;
        bool still = false;
        for (unsigned i = 0; i < n && !still; i++) still = eff[i].type == 2 && eff[i].id == g_ws_told[t].id;
        if (!still) window_forget((win_t)g_ws_told[t].id);
    }
    for (unsigned i = 0; i < n; i++) g_ws_told[i] = eff[i];
    g_ws_told_n = n;
}

void console_windows_now(const console_win_state_t *w, unsigned n) {
    if (n > WS_MAX) n = WS_MAX;
    for (unsigned i = 0; i < n; i++) g_ws_now[i] = w[i];
    g_ws_now_n = n;
    ws_compare();
}

bool console_window_present(uint8_t type, uint8_t id) {
    console_lock();
    bool found = false;
    for (unsigned i = 0; i < g_ws_now_n && !found; i++) found = g_ws_now[i].type == type && g_ws_now[i].id == id;
    console_unlock();
    return found;
}

uint8_t console_window_told(uint8_t type, uint8_t id) {
    console_lock();
    uint8_t f = ws_told_flags(type, id);
    console_unlock();
    return f;
}

/* ev to window (type, id): a canvas's ring, or a view through the views task. */
static void ws_send(uint8_t type, uint8_t id, const win_event_t *ev) {
    if (type == 2 /* RIBBON_WIN_CANVAS */) {
        window_post((win_t)id, ev);
    } else {
        const view_ops_t *v = view_for_window(type);
        if (v) view_post_nowait(v, id, ev);
    }
}

static void sys_hello(uint8_t type, uint8_t id);

static void ws_deliver(void) {
    if (__atomic_load_n(&g_ws_head, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_ws_tail, __ATOMIC_ACQUIRE)) return;
    ylock_acquire(&g_ws_deliver_lock);
    for (;;) {
        uintptr_t f = spin_lock_irqsave(&g_ws_q_lock);
        bool got = g_ws_tail != g_ws_head;
        uint8_t type = 0, id = 0, evt = 0;
        if (got) {
            uint32_t k = g_ws_tail++ % WS_QUEUE;
            type = g_ws_q[k].type;
            id = g_ws_q[k].id;
            evt = g_ws_q[k].ev;
        }
        spin_unlock_irqrestore(&g_ws_q_lock, f);
        if (!got) break;
        if (evt == WS_HELLO) {
            sys_hello(type, id);
            continue;
        }
        win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = evt;
        ws_send(type, id, &ev);
    }
    ylock_release(&g_ws_deliver_lock);
}

/* The blanking changed: every window's visibility with it. From the input
 * path, not from where it happened (the touch task wakes the panel). */
static void ws_blank_check(void) {
    if (!__atomic_exchange_n(&g_ws_blank_changed, false, __ATOMIC_ACQ_REL)) return;
    console_lock();
    ws_compare();
    console_unlock();
    ws_deliver();
}

/* --- 47.11: idle blanking ------------------------------------------------
 *
 * Input is the only activity that counts: a program printing to a blanked
 * panel leaves it blanked. Checked from the pump, which every task waiting
 * for input -- or polling for Ctrl-C -- runs, so a system with a shell at a
 * prompt anywhere checks it all the time. */
/* accepted: input from several tasks (pump, touch, keyboard) updates these
 * with single stores; two at once can count a wake twice or blank-hook twice,
 * both harmless. */
static volatile uint32_t g_blank_after_s;       /* 0: never */
static volatile uint64_t g_last_input_ms;
static uint32_t          g_blankings, g_wakes, g_wakes_swallowed;

static void blank_hook(bool off) {
    if (g_screen && g_screen->blank) g_screen->blank(off);
    __atomic_store_n(&g_ws_blank_changed, true, __ATOMIC_RELEASE);
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

/* accepted: g_hotkey's payload (above). */
static volatile uint32_t g_tap_xy;     /* x << 16 | y */
static volatile int      g_scroll_dy = 0;
static volatile uint32_t g_scroll_ms = 0;
/* accepted: a drag's motion, summed by the touch task (console_touch_drag())
 * and taken whole by the input path (run_hotkey()); the end's payload is
 * written before g_drag_end is set, and read after it is taken. */
/* accepted: press feedback's point and flags, set by the touch task, taken
 * by the input path; their own words, so a pending tap is never replaced. */
static volatile uint32_t g_press_xy;
static volatile bool     g_press_pending = false, g_release_pending = false;
static volatile unsigned g_repeat_pending = 0;    /* held-key repeats since the input path last looked */
static volatile uint32_t g_move_xy;               /* a captured finger's latest point; replaced, not queued */
static volatile bool     g_move_pending = false;
static volatile bool     g_press_busy = false;    /* one shell at a time runs run_press() / run_release() */
static bool              g_osk_pressed = false;   /* guarded by: g_press_busy */
static int               g_press_win = -1;        /* guarded by: g_press_busy; the canvas window told of a press */
static const view_ops_t *g_press_view;            /* guarded by: g_press_busy; the view told of a press */
static uint8_t           g_press_inst;            /* guarded by: g_press_busy */
static volatile int      g_drag_dy = 0;
static volatile bool     g_drag_end = false;
static volatile int      g_drag_end_dy;
static volatile uint32_t g_drag_end_ms;

static void console_pump(void);
/* The on-screen keyboard is for a touch screen without a keyboard. */
static bool osk_wanted(void) { return touch_found() && !usbkbd_is_present(); }
static void run_hotkey(void);
static void run_pointer_once(void);

#define CANVAS_KEY_RING_SIZE 32
/* accepted: circular SPSC key ring for the focused RIBBON_WIN_CANVAS window;
 * written by console_pump() / console_feed_active_bytes(), consumed by
 * console_canvas_getc(). Emptied when the focused canvas changes, so a key
 * typed for one program never reaches another. */
static volatile char     g_canvas_key_ring[CANVAS_KEY_RING_SIZE];
static volatile uint32_t g_canvas_key_head = 0;
static volatile uint32_t g_canvas_key_tail = 0;
static int               g_canvas_key_owner = -1;   /* accepted: written with the ring's head */

static void console_canvas_feed_key(char c) {
    int owner = window_focused();
    if (owner != g_canvas_key_owner) {
        g_canvas_key_owner = owner;
        __atomic_store_n(&g_canvas_key_tail, __atomic_load_n(&g_canvas_key_head, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
    }
    uint32_t head = g_canvas_key_head;
    uint32_t next = (head + 1u) % CANVAS_KEY_RING_SIZE;
    if (next != g_canvas_key_tail) {
        g_canvas_key_ring[head] = c;
        __atomic_store_n(&g_canvas_key_head, next, __ATOMIC_RELEASE);
    }
}

bool console_canvas_has_key(void) {
    console_pump();
    run_hotkey();
    return g_canvas_key_tail != __atomic_load_n(&g_canvas_key_head, __ATOMIC_ACQUIRE);
}

int console_canvas_getc(void) {
    console_pump();
    run_hotkey();
    uint32_t tail = g_canvas_key_tail;
    if (tail == __atomic_load_n(&g_canvas_key_head, __ATOMIC_ACQUIRE)) {
        return -1;
    }
    char c = g_canvas_key_ring[tail];
    __atomic_store_n(&g_canvas_key_tail, (tail + 1u) % CANVAS_KEY_RING_SIZE, __ATOMIC_RELEASE);
    return (unsigned char)c;
}

void console_canvas_clear_events(void) {
    __atomic_store_n(&g_canvas_key_tail, __atomic_load_n(&g_canvas_key_head, __ATOMIC_RELAXED), __ATOMIC_RELEASE);
}

/* A canvas window was closed (close box, Cmd+W): its program hears of it. */
static void post_close(uint8_t cid) {
    win_event_t wev;
    memset(&wev, 0, sizeof(wev));
    wev.type = WIN_EV_CLOSE_REQUEST;
    window_post((win_t)cid, &wev);
}

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

void console_touch_press(unsigned x, unsigned y) {
    g_press_xy = ((uint32_t)x << 16) | (y & 0xFFFFu);
    __atomic_store_n(&g_release_pending, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_press_pending, true, __ATOMIC_RELEASE);
}

void console_touch_release(void) {
    __atomic_store_n(&g_release_pending, true, __ATOMIC_RELEASE);
}

/* The view under screen point (x, y) (drivers/view.h): its ops, its
 * instance and the point in its content; NULL when the point is not on an
 * open view. */
static const view_ops_t *view_at(unsigned x, unsigned y, uint8_t *inst, int *cx, int *cy) {
    uint8_t type = 0, id = 0;
    if (!lcdterm_window_at((int)x, (int)y, &type, &id, cx, cy)) return NULL;
    const view_ops_t *v = view_for_window(type);
    if (!v || !v->active(id)) return NULL;
    *inst = id;
    return v;
}

/* The focused window's view, and its instance; NULL when it is not one. */
static const view_ops_t *focused_view(uint8_t *inst) {
    uint8_t type = 0, id = 0;
    if (!lcdterm_focused_window(&type, &id)) return NULL;
    const view_ops_t *v = view_for_window(type);
    if (!v || !v->active(id)) return NULL;
    *inst = id;
    return v;
}

static void view_send(const view_ops_t *v, uint8_t inst, uint8_t type, uint8_t flags, int x, int y, int dy, uint32_t key, uint8_t mods) {
    win_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.button = 1;
    ev.flags = flags;
    ev.x = (int16_t)x;
    ev.y = (int16_t)y;
    ev.dy = (int16_t)dy;
    ev.key = key;
    ev.mods = mods;
    view_post(v, inst, &ev);                             /* the views task draws it */
}

/* guarded by: g_input_lock -- the console pump decodes the focused view's
 * keys with it. */
static keyseq_push_t g_view_keys;

/* The system's Cmd shortcuts, whatever has the focus: Cmd-1..9 a window,
 * Cmd-W close, Cmd-\\ maximise, Cmd-[ / ] move the window, Cmd-Left / Right
 * the focus, Cmd-Shift-3 a screenshot. True when k was one. */
static bool system_shortcut(key_event_t k) {
    if (!(k.mods & KMOD_SUPER)) return false;
    if ((k.mods & KMOD_SHIFT) && k.key == '3') { console_hotkey(CONSOLE_HOTKEY_SCREENSHOT); return true; }
    if (k.key >= '1' && k.key <= '9') { console_hotkey(CONSOLE_HOTKEY_JUMP((unsigned)(k.key - '0'))); return true; }
    switch (k.key) {
    case 'w': case 'W': console_hotkey(CONSOLE_HOTKEY_CLOSE); return true;
    case '\\':         console_hotkey(CONSOLE_HOTKEY_TOGGLE_MAX); return true;
    case '[':           console_hotkey(CONSOLE_HOTKEY_LEFT); return true;
    case ']':           console_hotkey(CONSOLE_HOTKEY_RIGHT); return true;
    case KEY_LEFT:      console_hotkey(CONSOLE_HOTKEY_FOCUS_LEFT); return true;
    case KEY_RIGHT:     console_hotkey(CONSOLE_HOTKEY_FOCUS_RIGHT); return true;
    default:            return false;
    }
}

bool console_touch_captures(unsigned x, unsigned y) {
    uint8_t inst = 0;
    int cx = 0, cy = 0;
    const view_ops_t *v = view_at(x, y, &inst, &cx, &cy);
    return v && v->captures && v->captures(inst, cx, cy);
}

void console_touch_move(unsigned x, unsigned y) {
    g_move_xy = (x << 16) | (y & 0xFFFFu);
    __atomic_store_n(&g_move_pending, true, __ATOMIC_RELEASE);
}

/* A captured finger's latest point, to the view it is on. Moves in between
 * are dropped: the control follows the finger at the pace it can draw. */
static void run_move_once(void) {
    if (!__atomic_exchange_n(&g_move_pending, false, __ATOMIC_ACQ_REL)) return;
    uint32_t xy = g_move_xy;
    uint8_t inst = 0;
    int cx = 0, cy = 0;
    const view_ops_t *v = view_at(xy >> 16, xy & 0xFFFFu, &inst, &cx, &cy);
    if (v) view_send(v, inst, WIN_EV_POINTER_MOVE, 0, cx, cy, 0, 0, 0);
}

void console_touch_repeat(void) {
    if (note_input(true)) return;
    __atomic_fetch_add(&g_repeat_pending, 1u, __ATOMIC_ACQ_REL);
}

static void console_feed_active_bytes(const char *s, uint32_t n);

/* A key of the on-screen keyboard, as the terminal reads it: the arrows as
 * their sequences, ^C, everything else below 0x100 as its byte. */
static void osk_feed_key(uint16_t key) {
    if (key == OSK_KEY_UP) {
        console_feed_active_bytes("\x1b[A", 3);
    } else if (key == OSK_KEY_DOWN) {
        console_feed_active_bytes("\x1b[B", 3);
    } else if (key == OSK_KEY_LEFT) {
        console_feed_active_bytes("\x1b[D", 3);
    } else if (key == OSK_KEY_RIGHT) {
        console_feed_active_bytes("\x1b[C", 3);
    } else if (key == OSK_KEY_CTRL_C) {
        console_feed_active_bytes("\x03", 1);
    } else if (key > 0 && key < 0x100) {
        char c = (char)key;
        console_feed_active_bytes(&c, 1);
    }
}

/* A held key's repeats: each one types the pressed key again, if it is one
 * that repeats (Delete, the arrows). At most a few per turn, so a stalled
 * input path does not catch up in a burst. */
static void run_repeat_once(void) {
    unsigned n = __atomic_exchange_n(&g_repeat_pending, 0u, __ATOMIC_ACQ_REL);
    if (!n || !g_osk_pressed) return;
    if (n > 3) n = 3;
    uint8_t req[2] = { 'y', 'r' }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 2, reply) || reply[0] != 0 || reply[8] != 0xBB) return;
    uint16_t key = (uint16_t)(reply[6] | (reply[7] << 8));
    while (n--) osk_feed_key(key);
}

/* Press feedback: before the hotkeys, a press arms the control under the
 * finger in the launcher or shows the on-screen keyboard's key pressed;
 * after them, a release lets go -- a tap that came with it has already
 * disarmed the launcher (launcher_tap()), so it is drawn once. Every shell
 * waiting for input runs this: one at a time (g_press_busy), the others skip
 * it, so a press is never shown after its own release. */
static void run_press_once(void) {
    if (!__atomic_exchange_n(&g_press_pending, false, __ATOMIC_ACQ_REL)) return;
    uint32_t xy = g_press_xy;
    uint8_t type = 0, id = 0;
    int cx = 0, cy = 0;
    if (!lcdterm_window_at((int)(xy >> 16), (int)(xy & 0xFFFFu), &type, &id, &cx, &cy)) return;
    const view_ops_t *v = view_for_window(type);
    if (v && v->active(id)) {
        view_send(v, id, WIN_EV_POINTER_DOWN, WIN_EVF_PRESS, cx, cy, 0, 0, 0);
        g_press_view = v;
        g_press_inst = id;
    } else if (type == 2 /* RIBBON_WIN_CANVAS */ && (win_t)id == window_focused() &&
               !(window_claims((win_t)id) & WIN_CLAIM_POINTER)) {
        win_event_t wev;
        memset(&wev, 0, sizeof(wev));
        wev.type = WIN_EV_POINTER_DOWN;
        wev.button = 1;
        wev.flags = WIN_EVF_PRESS;
        wev.x = (int16_t)cx;
        wev.y = (int16_t)cy;
        window_post((win_t)id, &wev);
        g_press_win = id;
    } else if (type == 1 /* RIBBON_WIN_TERM */) {
        /* the on-screen keyboard's key, if the point is on one */
        uint8_t req[5] = { 'y', (uint8_t)(xy >> 16), (uint8_t)(xy >> 24), (uint8_t)xy, (uint8_t)(xy >> 8) };
        uint8_t reply[SCREEN_REPLY_LEN];
        g_osk_pressed = true;
        (void)console_canvas(req, 5, reply);
    }
}

static void run_release_once(void) {
    if (!__atomic_exchange_n(&g_release_pending, false, __ATOMIC_ACQ_REL)) return;
    if (g_press_view) {
        if (g_press_view->active(g_press_inst)) view_send(g_press_view, g_press_inst, WIN_EV_POINTER_UP, WIN_EVF_PRESS, 0, 0, 0, 0, 0);
        g_press_view = NULL;
    }
    if (g_press_win >= 0) {
        win_event_t wev;
        memset(&wev, 0, sizeof(wev));
        wev.type = WIN_EV_POINTER_UP;
        wev.button = 1;
        wev.flags = WIN_EVF_PRESS;
        window_post((win_t)g_press_win, &wev);
        g_press_win = -1;
    }
    if (g_osk_pressed) {
        g_osk_pressed = false;
        uint8_t req[1] = { 'y' }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 1, reply);
    }
}

static void run_press(void) {
    if (__atomic_exchange_n(&g_press_busy, true, __ATOMIC_ACQ_REL)) return;
    run_press_once();
    run_move_once();
    run_repeat_once();
    run_pointer_once();
    __atomic_store_n(&g_press_busy, false, __ATOMIC_RELEASE);
}

static void run_release(void) {
    if (__atomic_exchange_n(&g_press_busy, true, __ATOMIC_ACQ_REL)) return;
    run_release_once();
    __atomic_store_n(&g_press_busy, false, __ATOMIC_RELEASE);
}

void console_touch_drag(int dy) {
    if (note_input(true)) return;
    __atomic_fetch_add(&g_drag_dy, dy, __ATOMIC_ACQ_REL);
    g_hotkey = CONSOLE_HOTKEY_DRAG;
}

void console_touch_drag_end(int total_dy, int dy, uint32_t ms) {
    (void)total_dy;                                      /* the drags before it moved by it already */
    g_drag_end_dy = dy;
    g_drag_end_ms = ms;
    __atomic_store_n(&g_drag_end, true, __ATOMIC_RELEASE);
    g_hotkey = CONSOLE_HOTKEY_DRAG_END;
}

/* The mouse (console_pointer()). Its driver's task keeps the button state
 * and decides what a change is; what needs the windows (hover, the context
 * click, the wheel) waits for the input path in these words, as a touch's
 * press does: the latest point replaces an unread one, wheel notches add up.
 * accepted: one producer (the mouse's task) and the input path, which takes
 * each word with an atomic exchange; a point read between two stores of a
 * new one is a pointer position a few pixels off, once. */
static volatile uint32_t g_hover_xy;
static volatile bool     g_hover_pending = false;
static volatile uint32_t g_context_xy;
static volatile bool     g_context_pending = false;
static volatile uint32_t g_wheel_xy;
static volatile int      g_wheel_dy = 0;

#define POINTER_DRAG_PX 6   /* moved this far with the left button held: a drag, not a click */

/* single task: the mouse's driver task (console_pointer() / console_wheel()). */
static struct {
    int     x, y;           /* the last point */
    uint8_t buttons;        /* WIN_BUTTON_* held */
    int     x0, y0;         /* where the left button went down */
    bool    dragging;       /* it moved past POINTER_DRAG_PX: a drag (no click at the release) */
    bool    captured;       /* it went down on a control that follows it (a slider) */
    win_t   raw;            /* the window claiming the pointer that has this press, -1: none */
    int     ox, oy;         /* raw's content origin */
    bool    waking;         /* a press that only woke the panel: nothing until every button is up */
} g_ptr = { .raw = -1 };

static uint32_t pack_xy(int x, int y) { return ((uint32_t)x << 16) | ((uint32_t)y & 0xFFFFu); }

/* The raw stream to a window that claimed the pointer: content coordinates,
 * the motion since the last event. */
static void pointer_raw(uint8_t type, uint8_t button, int x, int y, int px, int py) {
    win_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.button = button;
    ev.flags = WIN_EVF_MOUSE;
    ev.x = (int16_t)(x - g_ptr.ox);
    ev.y = (int16_t)(y - g_ptr.oy);
    ev.dx = (int16_t)(x - px);
    ev.dy = (int16_t)(y - py);
    window_post(g_ptr.raw, &ev);
}

/* The focused window, if it claims the pointer: the raw stream is its. */
static bool pointer_claimed(void) {
    win_t w = window_focused();
    if (w < 0 || !(window_claims(w) & WIN_CLAIM_POINTER) || !window_origin(w, &g_ptr.ox, &g_ptr.oy)) return false;
    g_ptr.raw = w;
    return true;
}

static uint8_t button_number(uint8_t mask) {
    return mask == WIN_BUTTON_LEFT ? 1u : mask == WIN_BUTTON_RIGHT ? 2u : 3u;
}

void console_pointer(int x, int y, uint8_t buttons) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > 0x7FFF) x = 0x7FFF;
    if (y > 0x7FFF) y = 0x7FFF;
    int px = g_ptr.x, py = g_ptr.y;
    uint8_t was = g_ptr.buttons, went_down = (uint8_t)(buttons & ~was), went_up = (uint8_t)(was & ~buttons);
    g_ptr.x = x;
    g_ptr.y = y;
    g_ptr.buttons = buttons;
    bool moved = x != px || y != py;
    if (!went_down && !went_up && !moved) return;
    if (g_ptr.waking) {
        if (!buttons) g_ptr.waking = false;
        return;
    }
    if (note_input(true)) {                              /* it only woke the panel */
        g_ptr.waking = buttons != 0 && g_ptr.raw < 0;
        return;
    }

    /* A press of a window that claims the pointer: the raw stream, every
     * button, until the last one is let go. */
    if (g_ptr.raw < 0 && went_down && !was && pointer_claimed()) {
        for (uint8_t m = WIN_BUTTON_LEFT; m <= WIN_BUTTON_MIDDLE; m <<= 1)
            if (went_down & m) pointer_raw(WIN_EV_POINTER_DOWN, button_number(m), x, y, x, y);
        return;
    }
    if (g_ptr.raw >= 0) {
        if (moved) pointer_raw(WIN_EV_POINTER_MOVE, buttons ? button_number((uint8_t)(buttons & -buttons)) : 0, x, y, px, py);
        for (uint8_t m = WIN_BUTTON_LEFT; m <= WIN_BUTTON_MIDDLE; m <<= 1) {
            if (went_down & m) pointer_raw(WIN_EV_POINTER_DOWN, button_number(m), x, y, px, py);
            if (went_up & m) pointer_raw(WIN_EV_POINTER_UP, button_number(m), x, y, px, py);
        }
        if (!buttons) g_ptr.raw = -1;
        return;
    }

    /* The left button: a finger. */
    if (went_down & WIN_BUTTON_LEFT) {
        g_ptr.x0 = x;
        g_ptr.y0 = y;
        g_ptr.dragging = false;
        g_ptr.captured = console_touch_captures((unsigned)x, (unsigned)y);
        console_touch_press((unsigned)x, (unsigned)y);
    } else if ((buttons & WIN_BUTTON_LEFT) && moved) {
        if (g_ptr.captured) {
            console_touch_move((unsigned)x, (unsigned)y);
        } else {
            int ax = x - g_ptr.x0, ay = y - g_ptr.y0;
            if (!g_ptr.dragging && (ax > POINTER_DRAG_PX || -ax > POINTER_DRAG_PX ||
                                    ay > POINTER_DRAG_PX || -ay > POINTER_DRAG_PX)) {
                g_ptr.dragging = true;
                console_touch_release();                 /* no longer a click */
            }
            if (g_ptr.dragging && y != py) console_touch_drag(y - py);
        }
    }
    if (went_up & WIN_BUTTON_LEFT) {
        if (g_ptr.captured) {
            g_ptr.captured = false;
            console_touch_move((unsigned)x, (unsigned)y);
            console_touch_release();
        } else if (g_ptr.dragging) {
            console_touch_drag_end(y - g_ptr.y0, 0, 0);  /* a mouse does not fling */
        } else {
            console_touch_release();
            console_touch_tap((unsigned)g_ptr.x0, (unsigned)g_ptr.y0);
        }
        g_ptr.dragging = false;
    }

    /* The right button: the context menu, where it was let go. */
    if (went_up & WIN_BUTTON_RIGHT) {
        g_context_xy = pack_xy(x, y);
        __atomic_store_n(&g_context_pending, true, __ATOMIC_RELEASE);
    }

    /* Hover: no button held. */
    if (!buttons && moved) {
        g_hover_xy = pack_xy(x, y);
        __atomic_store_n(&g_hover_pending, true, __ATOMIC_RELEASE);
    }
}

void console_wheel(int dx, int dy) {
    if (!dx && !dy) return;
    if (note_input(true)) return;
    if (dy) {
        win_t w = window_focused();
        if (g_ptr.raw < 0 && w >= 0 && (window_claims(w) & WIN_CLAIM_POINTER) && window_origin(w, &g_ptr.ox, &g_ptr.oy)) {
            win_event_t ev;                              /* a game's own, at the pointer */
            memset(&ev, 0, sizeof ev);
            ev.type = WIN_EV_SCROLL;
            ev.flags = WIN_EVF_WHEEL | WIN_EVF_MOUSE;
            ev.x = (int16_t)(g_ptr.x - g_ptr.ox);
            ev.y = (int16_t)(g_ptr.y - g_ptr.oy);
            ev.dy = (int16_t)dy;
            window_post(w, &ev);
        } else {
            g_wheel_xy = pack_xy(g_ptr.x, g_ptr.y);
            __atomic_fetch_add(&g_wheel_dy, dy, __ATOMIC_ACQ_REL);
        }
    }
    if (dx) console_hotkey(dx > 0 ? CONSOLE_HOTKEY_FOCUS_RIGHT : CONSOLE_HOTKEY_FOCUS_LEFT);
}

void console_touch_context(unsigned x, unsigned y) {
    if (note_input(true)) return;
    g_context_xy = pack_xy((int)x, (int)y);
    __atomic_store_n(&g_context_pending, true, __ATOMIC_RELEASE);
}

/* The mouse's words, in the input path: hover to the view under the pointer
 * or a canvas window that claims the pointer; the context click as a button
 * 2 tap to the view under it; the wheel to the view under the pointer. */
static const view_ops_t *g_hover_view;    /* guarded by: g_press_busy; the view last hovered */
static uint8_t           g_hover_inst;    /* guarded by: g_press_busy */

static void run_pointer_once(void) {
    uint8_t inst = 0;
    int cx = 0, cy = 0;
    if (__atomic_exchange_n(&g_hover_pending, false, __ATOMIC_ACQ_REL)) {
        uint32_t xy = g_hover_xy;
        uint8_t type = 0, id = 0;
        const view_ops_t *v = NULL;
        win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = WIN_EV_POINTER_MOVE;
        if (lcdterm_window_at((int)(xy >> 16), (int)(xy & 0xFFFFu), &type, &id, &cx, &cy)) {
            v = view_for_window(type);
            if (v && !v->active(id)) v = NULL;
            ev.x = (int16_t)cx;
            ev.y = (int16_t)cy;
            if (!v && type == 2 /* RIBBON_WIN_CANVAS */ && (window_claims((win_t)id) & WIN_CLAIM_POINTER)) {
                win_event_t raw = ev;
                raw.flags = WIN_EVF_MOUSE;
                window_post((win_t)id, &raw);
            }
        }
        if (g_hover_view && (g_hover_view != v || g_hover_inst != id)) {
            win_event_t gone = ev;                       /* the pointer left it: nothing hovered */
            gone.x = gone.y = -1;
            if (g_hover_view->active(g_hover_inst)) view_post(g_hover_view, g_hover_inst, &gone);
        }
        g_hover_view = v;
        g_hover_inst = id;
        if (v) view_post(v, id, &ev);
    }
    if (__atomic_exchange_n(&g_context_pending, false, __ATOMIC_ACQ_REL)) {
        uint32_t xy = g_context_xy;
        const view_ops_t *v = view_at(xy >> 16, xy & 0xFFFFu, &inst, &cx, &cy);
        if (v) {
            win_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.button = 2;
            ev.flags = WIN_EVF_TAP;
            ev.x = (int16_t)cx;
            ev.y = (int16_t)cy;
            ev.type = WIN_EV_POINTER_DOWN;
            view_post(v, inst, &ev);
            ev.type = WIN_EV_POINTER_UP;
            view_post(v, inst, &ev);
        }
    }
    int dy = __atomic_exchange_n(&g_wheel_dy, 0, __ATOMIC_ACQ_REL);
    if (dy) {
        uint32_t xy = g_wheel_xy;
        const view_ops_t *v = view_at(xy >> 16, xy & 0xFFFFu, &inst, &cx, &cy);
        if (v) {
            win_event_t ev;
            memset(&ev, 0, sizeof ev);
            ev.type = WIN_EV_SCROLL;
            ev.flags = WIN_EVF_WHEEL;
            ev.x = (int16_t)cx;
            ev.y = (int16_t)cy;
            ev.dy = (int16_t)dy;
            view_post(v, inst, &ev);
        }
    }
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
            const view_ops_t *v = view_for_window(reply[1]);
            if (v) {                                     /* a view: its content's size now */
                int rx0 = (int)(reply[4] | (reply[5] << 8));
                int rx1 = (int)(reply[6] | (reply[7] << 8));
                int vw = rx1 - rx0 - 1;
                int vh = (int)(reply[10] | (reply[11] << 8));
                if (vw > 0) {
                    win_event_t ev;
                    memset(&ev, 0, sizeof ev);
                    ev.type = WIN_EV_RESIZE;
                    ev.w = (uint16_t)vw;
                    ev.h = (uint16_t)vh;
                    view_post(v, reply[2], &ev);
                }
            }
        }
    }
}

static void console_feed_active_bytes(const char *s, uint32_t n);

/* A view's window was closed: the view lets go of it, after whatever was
 * posted to it before. */
static void view_closed(uint8_t type, uint8_t inst) {
    const view_ops_t *v = view_for_window(type);
    if (!v) return;
    win_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_CLOSE_REQUEST;
    view_post(v, inst, &ev);
}

/* A window was closed (the reply of 'C', 'q' or a close-box tap): its
 * owner told, the focus follows the screen's. */
static void window_closed(const uint8_t *reply) {
    uint8_t closed_type = reply[10];
    uint8_t closed_vid = reply[11];
    if (closed_type == 1 /* RIBBON_WIN_TERM */ && closed_vid > 0) vterm_destroy(closed_vid);
    if (closed_type == 2 /* RIBBON_WIN_CANVAS */) post_close(closed_vid);
    view_closed(closed_type, closed_vid);
    if (reply[12] != 0) {
        g_active_win_type = reply[12];
        if (reply[12] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[13]);
    } else {
        g_active_win_type = closed_type != 4 /* RIBBON_WIN_LAUNCHER */ && launcher_is_active() ? 4 : 1;
    }
    vterm_set_active(reply[1]);
    console_sync_readers();
}

bool console_close_canvas(uint8_t cid, bool free_it) {
    uint8_t req[3] = { 'q', cid, (uint8_t)(free_it ? 1 : 0) }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 3, reply) || reply[0] != 0) return false;
    if (reply[8] == 0xCC) {                              /* a window went, not only a detached canvas */
        if (free_it) window_closed(reply);
        else {                                           /* asked already: no second request */
            uint8_t r2[SCREEN_REPLY_LEN];
            for (unsigned i = 0; i < SCREEN_REPLY_LEN; i++) r2[i] = reply[i];
            r2[10] = 0;                                  /* no owner to tell again */
            window_closed(r2);
        }
    }
    return true;
}

/* Closing canvas window reply[11] was asked for (0xCE): a program that reads
 * its window is told and given time to answer (drivers/winclose.c); one that
 * does not, or whose task has ended, has its window closed at once. */
static void close_asked(const uint8_t *reply) {
    uint8_t cid = reply[11];
    int pid = (int16_t)(reply[14] | (reply[15] << 8));
    int st = pid >= 0 ? sched_task_state(pid) : TASK_UNUSED;
    bool alive = st != TASK_UNUSED && st != TASK_DEAD;
    if (alive && window_listened((win_t)cid)) {
        post_close(cid);
        winclose_watch(cid, pid);
        return;
    }
    (void)console_close_canvas(cid, true);
}

static void launcher_open_cb(void *arg) { (void)arg; (void)launcher_open(); }

/* --- The system's events (kernel/event.h): the keyboard, the rotation, the
 * power. Watched from the input path, every pass (two loads and a word the
 * power task stores), and each change told to every canvas window and view
 * on the ribbon. */
static volatile uint32_t g_sys_power;       /* accepted: one word, stored by the power task */
static volatile uint32_t g_sys_devices;     /* accepted: WIN_INPUT_* bits, set and cleared atomically */
static volatile bool     g_sys_busy;        /* one input path at a time looks */
static int               g_sys_input = -1;  /* guarded by: g_sys_busy; -1 not looked yet */

void console_input_present(uint32_t device, bool present) {
    device &= WIN_INPUT_KEYBOARD | WIN_INPUT_POINTER;
    if (present) __atomic_fetch_or(&g_sys_devices, device, __ATOMIC_ACQ_REL);
    else __atomic_fetch_and(&g_sys_devices, ~device, __ATOMIC_ACQ_REL);
}

/* What is attached now: WIN_INPUT_*. */
static uint32_t sys_input_mode(void) {
    uint32_t m = __atomic_load_n(&g_sys_devices, __ATOMIC_ACQUIRE);
    if (usbkbd_is_present()) m |= WIN_INPUT_KEYBOARD;
    if (usbkbd_mouse_present()) m |= WIN_INPUT_POINTER;
    if (touch_found()) m |= WIN_INPUT_TOUCH;
    return m;
}
static uint32_t          g_sys_power_told;  /* guarded by: g_sys_busy */

void console_power_state(int pct, bool charging, bool low) {
    uint32_t v = 0x80000000u | (uint32_t)(pct >= 0 && pct <= 100 ? pct : 0xFF) |
                 (charging ? (uint32_t)WIN_POWER_CHARGING << 8 : 0u) | (low ? (uint32_t)WIN_POWER_LOW << 8 : 0u);
    __atomic_store_n(&g_sys_power, v, __ATOMIC_RELEASE);
}

/* ev to every canvas window and view on the ribbon, as the screen last
 * reported them. */
static void sys_tell(const win_event_t *ev) {
    console_win_state_t w[WS_MAX];
    console_lock();
    unsigned n = g_ws_now_n;
    for (unsigned i = 0; i < n; i++) w[i] = g_ws_now[i];
    console_unlock();
    for (unsigned i = 0; i < n; i++) ws_send(w[i].type, w[i].id, ev);
}

/* A panel that turns (the P4's DSI panels): its rotation watched too. */
#if defined(CONFIG_BOARD_ESP32P4) && defined(LCDTERM_PRESENT)
#define SYS_ROTATES 1
static int g_sys_rot = -1;                  /* guarded by: g_sys_busy */

/* After a rotation: each canvas window's new content size (the views get
 * theirs from console_sync_readers()). */
static void sys_resize_canvases(void) {
    uint8_t req[2] = { 'R', 0 }, reply[SCREEN_REPLY_LEN];
    if (!console_canvas(req, 1, reply) || reply[0] != 0) return;
    uint8_t count = reply[1];
    for (uint8_t i = 0; i < count; i++) {
        req[1] = i;
        if (!console_canvas(req, 2, reply) || reply[0] != 0 || reply[1] != 2 /* RIBBON_WIN_CANVAS */) continue;
        int rx0 = (int)(reply[4] | (reply[5] << 8)), rx1 = (int)(reply[6] | (reply[7] << 8));
        win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = WIN_EV_RESIZE;
        ev.w = (uint16_t)(rx1 - rx0 - 1);
        ev.h = (uint16_t)(reply[10] | (reply[11] << 8));
        window_post((win_t)reply[2], &ev);
    }
}
#endif

static void sys_check(void) {
    if (__atomic_exchange_n(&g_sys_busy, true, __ATOMIC_ACQ_REL)) return;
    win_event_t ev;
    memset(&ev, 0, sizeof ev);

    /* The input devices. A keyboard attached: the on-screen keyboard folds
     * away; gone where there is touch, the launcher comes up (touch is all
     * there is). The first look is the state at boot: no event, and the boot
     * shell's own check opens the launcher. */
    int mode = (int)sys_input_mode();
    if (g_sys_input >= 0 && mode != g_sys_input) {
        ev.type = WIN_EV_INPUT_MODE;
        ev.key = (uint32_t)mode;
        sys_tell(&ev);
        uint32_t came = (uint32_t)(mode & ~g_sys_input), went = (uint32_t)(g_sys_input & ~mode);
        if (came & WIN_INPUT_KEYBOARD) {
            uint8_t kreq[2] = { 'k', 0 }, kreply[SCREEN_REPLY_LEN];
            (void)console_canvas(kreq, 2, kreply);
        } else if ((went & WIN_INPUT_KEYBOARD) && (mode & WIN_INPUT_TOUCH)) {
            view_post_call(launcher_open_cb, NULL);
        } else if ((came & WIN_INPUT_POINTER) && !(mode & (WIN_INPUT_KEYBOARD | WIN_INPUT_TOUCH))) {
            view_post_call(launcher_open_cb, NULL);    /* a mouse alone: the launcher is the way in */
        }
    }
    g_sys_input = mode;

#if defined(SYS_ROTATES)
    int rot = dsi_lcd_get_orientation();
    if (g_sys_rot >= 0 && rot != g_sys_rot) {
        ev.type = WIN_EV_ORIENT;
        ev.key = (uint32_t)rot;
        sys_tell(&ev);
        sys_resize_canvases();
        console_sync_readers();
    }
    g_sys_rot = rot;
#endif

    /* The power: at each 10 % step of the charge and each change of
     * charging or low, not at every percent. */
    uint32_t pw = __atomic_load_n(&g_sys_power, __ATOMIC_ACQUIRE);
    uint32_t told = g_sys_power_told;
    if (pw && pw != told &&
        (!told || (pw & 0xFFu) / 10u != (told & 0xFFu) / 10u || ((pw ^ told) & 0xFF00u))) {
        g_sys_power_told = pw;
        ev.type = WIN_EV_POWER;
        ev.key = pw & 0xFFu;
        ev.mods = (uint8_t)(pw >> 8);
        sys_tell(&ev);
    }
    __atomic_store_n(&g_sys_busy, false, __ATOMIC_RELEASE);
}

/* A new window: the system's state as last told, so a program knows it from
 * the start -- the keyboard, the rotation, the power, where known. */
static void sys_hello(uint8_t type, uint8_t id) {
    win_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = WIN_EV_INPUT_MODE;
    ev.key = sys_input_mode();
    ws_send(type, id, &ev);
#if defined(SYS_ROTATES)
    ev.type = WIN_EV_ORIENT;
    ev.key = (uint32_t)dsi_lcd_get_orientation();
    ws_send(type, id, &ev);
#endif
    uint32_t pw = __atomic_load_n(&g_sys_power, __ATOMIC_ACQUIRE);
    if (pw) {
        ev.type = WIN_EV_POWER;
        ev.key = pw & 0xFFu;
        ev.mods = (uint8_t)(pw >> 8);
        ws_send(type, id, &ev);
    }
}

#if defined(CONFIG_BOARD_TAB5)
static void reader_update_cb(void *arg) { (void)arg; reader_update(); }
#endif

/* From an input wait only: a hotkey draws (a layout change) or writes 48 KB
 * to the SD card (a screenshot), neither of which belongs inside a print. */
static void run_hotkey(void) {
    ws_blank_check();
    sys_check();
    run_press();
    for (int iter = 0; iter < 4; iter++) {
        unsigned hk = __atomic_exchange_n(&g_hotkey, 0u, __ATOMIC_ACQ_REL);
        if (!hk) break;
        if (hk == CONSOLE_HOTKEY_SCREENSHOT) {
            char path[48];
            (void)screenshot_save(NULL, path, sizeof(path));
            continue;
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
                if (osk_wanted()) {
                    uint8_t kreq[2] = { 'k', 1 };
                    (void)console_canvas(kreq, 2, reply);
                }
                console_sync_readers();
            }
            continue;
        }
        if (hk == CONSOLE_HOTKEY_FOCUS_LEFT) {
            uint8_t req[1] = { '<' }, reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 1, reply) && reply[0] == 0) {
                if (reply[10] != 0) g_active_win_type = reply[10];
                if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
                vterm_set_active(reply[1]);
                if (g_active_win_type == 1 /* RIBBON_WIN_TERM */ && osk_wanted()) {
                    uint8_t kreq[2] = { 'k', 1 };
                    (void)console_canvas(kreq, 2, reply);
                }
            }
            continue;
        }
        if (hk == CONSOLE_HOTKEY_FOCUS_RIGHT) {
            uint8_t req[1] = { '>' }, reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 1, reply) && reply[0] == 0) {
                if (reply[10] != 0) g_active_win_type = reply[10];
                if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
                vterm_set_active(reply[1]);
                if (g_active_win_type == 1 /* RIBBON_WIN_TERM */ && osk_wanted()) {
                    uint8_t kreq[2] = { 'k', 1 };
                    (void)console_canvas(kreq, 2, reply);
                }
            }
            continue;
        }
        if (hk == CONSOLE_HOTKEY_MOVE_LEFT) {
            uint8_t req[1] = { '[' }, reply[SCREEN_REPLY_LEN];
            (void)console_canvas(req, 1, reply);
            continue;
        }
        if (hk == CONSOLE_HOTKEY_MOVE_RIGHT) {
            uint8_t req[1] = { ']' }, reply[SCREEN_REPLY_LEN];
            (void)console_canvas(req, 1, reply);
            continue;
        }
        if (hk == CONSOLE_HOTKEY_CLOSE) {
            uint8_t req[1] = { 'C' }, reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 1, reply) && reply[0] == 0) {
                if (reply[8] == 0xCE) close_asked(reply);
                else window_closed(reply);
            }
            continue;
        }
        if (hk >= CONSOLE_HOTKEY_JUMP_BASE && hk <= CONSOLE_HOTKEY_JUMP_BASE + 8) {
            uint8_t target_idx = (uint8_t)(hk - CONSOLE_HOTKEY_JUMP_BASE);
            uint8_t req[2] = { 'J', target_idx }, reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 2, reply) && reply[0] == 0) {
                if (reply[10] != 0) g_active_win_type = reply[10];
                if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
                vterm_set_active(reply[1]);
                if (g_active_win_type == 1 /* RIBBON_WIN_TERM */ && osk_wanted()) {
                    uint8_t kreq[2] = { 'k', 1 };
                    (void)console_canvas(kreq, 2, reply);
                }
            }
            continue;
        }
        if (hk == CONSOLE_HOTKEY_TAP) {
            uint32_t xy = g_tap_xy;
            uint8_t req[6] = {
                'P',
                (uint8_t)(xy >> 16),
                (uint8_t)(xy >> 24),
                (uint8_t)xy,
                (uint8_t)(xy >> 8),
                (uint8_t)(usbkbd_is_present() ? 1 : 0)
            };
            uint8_t reply[SCREEN_REPLY_LEN];
            if (console_canvas(req, 6, reply) && reply[0] == 0) {
                if (reply[8] == 0xBB) {
                    uint16_t key = (uint16_t)(reply[6] | (reply[7] << 8));
                    uint8_t mods = reply[9];
                    if (mods & OSK_MOD_CMD) {
                        if (key == OSK_KEY_ENTER || key == '\r' || key == '\n') {
                            console_hotkey(CONSOLE_HOTKEY_NEW_TERM);
                            continue;
                        } else if (key == 'w' || key == 'W') {
                            console_hotkey(CONSOLE_HOTKEY_CLOSE);
                            continue;
                        } else if (key == OSK_KEY_LEFT) {
                            console_hotkey(CONSOLE_HOTKEY_FOCUS_LEFT);
                            continue;
                        } else if (key == OSK_KEY_RIGHT) {
                            console_hotkey(CONSOLE_HOTKEY_FOCUS_RIGHT);
                            continue;
                        }
                    }
                    osk_feed_key(key);
                    continue;
                }
                if (reply[8] == 0xCC) {
                    window_closed(reply);
                    continue;
                }
                if (reply[8] == 0xCE) {
                    close_asked(reply);
                    continue;
                }
                if (reply[8] == 0xAA) {
                    view_post_call(launcher_open_cb, NULL);
                    continue;
                }
                if (reply[10] != 0) g_active_win_type = reply[10];
                if (reply[10] == 3 /* RIBBON_WIN_READER */) reader_set_active_instance(reply[11]);
                vterm_set_active(reply[1]);
                if (g_active_win_type == 1 /* RIBBON_WIN_TERM */ && osk_wanted()) {
                    uint8_t kreq[2] = { 'k', 1 };
                    (void)console_canvas(kreq, 2, reply);
                }
                int cx = (int)(int16_t)(uint16_t)(reply[2] | (reply[3] << 8));
                int cy = (int)(int16_t)(uint16_t)(reply[4] | (reply[5] << 8));
                uint8_t vinst = 0;
                const view_ops_t *v = focused_view(&vinst);
                if (v) {
                    view_send(v, vinst, WIN_EV_POINTER_UP, WIN_EVF_TAP, cx, cy, 0, 0, 0);
                } else if (g_active_win_type == 2 /* RIBBON_WIN_CANVAS */) {
                    /* A tap on the focused canvas: its program's, as a
                     * DOWN / UP pair at the content point. */
                    win_t w = window_focused();
                    win_event_t wev;
                    memset(&wev, 0, sizeof(wev));
                    wev.button = 1;
                    wev.flags = WIN_EVF_TAP;
                    wev.x = (int16_t)cx;
                    wev.y = (int16_t)cy;
                    wev.type = WIN_EV_POINTER_DOWN;
                    window_post(w, &wev);
                    wev.type = WIN_EV_POINTER_UP;
                    window_post(w, &wev);
                }
            }
            continue;
        }
        if (hk == CONSOLE_HOTKEY_DRAG || hk == CONSOLE_HOTKEY_DRAG_END) {
            /* Whatever has moved since the last pass, then the end if it
             * came: a drag that ended is never left without its motion. */
            int dy = __atomic_exchange_n(&g_drag_dy, 0, __ATOMIC_ACQ_REL);
            bool end = __atomic_exchange_n(&g_drag_end, false, __ATOMIC_ACQ_REL);
            uint8_t vinst = 0;
            const view_ops_t *v = focused_view(&vinst);
            if (v) {
                if (dy) view_send(v, vinst, WIN_EV_DRAG, 0, 0, 0, dy, 0, 0);
                if (end) view_send(v, vinst, WIN_EV_DRAG_END, 0, 0, 0, g_drag_end_dy, g_drag_end_ms, 0);
            }
            continue;
        }
        if (hk == CONSOLE_HOTKEY_SCROLL_UP || hk == CONSOLE_HOTKEY_SCROLL_DOWN) {
            int dy = g_scroll_dy;
            uint32_t ms = g_scroll_ms;
            g_scroll_dy = 0;
            g_scroll_ms = 0;
            if (dy == 0) dy = hk == CONSOLE_HOTKEY_SCROLL_UP ? 1 : -1;      /* a step: its direction */
            uint8_t vinst = 0;
            const view_ops_t *v = focused_view(&vinst);
            if (v) view_send(v, vinst, WIN_EV_SCROLL, 0, 0, 0, dy, ms, 0);
            continue;
        }
        if (hk == CONSOLE_HOTKEY_TOGGLE_MAX) {
            uint8_t req[1] = { 'M' }, reply[SCREEN_REPLY_LEN];
            (void)console_canvas(req, 1, reply);
            console_sync_readers();
            continue;
        }
        if (hk == CONSOLE_HOTKEY_ORIENT) {
#if defined(CONFIG_BOARD_TAB5)
            int cur = dsi_lcd_get_orientation();
            int next = (cur == 0 || cur == 180) ? 270 : 0;
            orient_manual();
            dsi_lcd_set_orientation(next);
            console_sync_readers();
            if (reader_is_active()) view_post_call(reader_update_cb, NULL);
#endif
            continue;
        }
        uint8_t req[2] = { 'w', (uint8_t)(hk == CONSOLE_HOTKEY_RIGHT ? 1 : -1) }, reply[SCREEN_REPLY_LEN];
        (void)console_canvas(req, 2, reply);
        console_sync_readers();
    }
    run_release();
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
/* console_capture(): what a task (and the tasks it started) writes, kept
 * instead of shown. One slot per capturing task, so captures in two terminals
 * (`ea` in one, the editor in another) neither end nor overrun each other.
 * guarded by: g_cap_lock, for taking and giving back a slot and for every
 * write into one; g_cap_active lets console_putc() skip the lock when no
 * capture runs. */
#define CAP_SLOTS 4
typedef struct {
    char    *buf;          /* NULL: free */
    uint32_t max, len;
    int      pid;          /* only its writes, and its descendants', are kept */
} cap_slot_t;
static cap_slot_t        g_caps[CAP_SLOTS];   /* guarded by: g_cap_lock (above) */
static volatile uint32_t g_cap_active;
static spinlock_t        g_cap_lock = SPINLOCK_INIT;

void console_capture(char *buf, uint32_t cap) {
    if (!buf || !cap) return;
    int me = sched_current_pid();
    uintptr_t f = spin_lock_irqsave(&g_cap_lock);
    cap_slot_t *slot = NULL;
    for (unsigned i = 0; i < CAP_SLOTS; i++) {
        if (g_caps[i].buf && g_caps[i].pid == me) { slot = &g_caps[i]; break; }
        if (!g_caps[i].buf && !slot) slot = &g_caps[i];
    }
    if (slot) {
        if (!slot->buf) g_cap_active++;
        slot->buf = buf;
        slot->max = cap;
        slot->len = 0;
        slot->pid = me;
    }
    spin_unlock_irqrestore(&g_cap_lock, f);
}

uint32_t console_capture_end(void) {
    int me = sched_current_pid();
    uint32_t len = 0;
    uintptr_t f = spin_lock_irqsave(&g_cap_lock);
    for (unsigned i = 0; i < CAP_SLOTS; i++) {
        cap_slot_t *c = &g_caps[i];
        if (!c->buf || c->pid != me) continue;
        len = c->len;
        c->buf[c->len < c->max ? c->len : c->max - 1u] = '\0';
        c->buf = NULL;
        g_cap_active--;
        break;
    }
    spin_unlock_irqrestore(&g_cap_lock, f);
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

/* True when the running task's output went into a capture. */
static bool capture_putc(char c) {
    int me = sched_current_pid();
    bool kept = false;
    uintptr_t f = spin_lock_irqsave(&g_cap_lock);
    for (unsigned i = 0; i < CAP_SLOTS && !kept; i++) {
        cap_slot_t *s = &g_caps[i];
        if (!s->buf || !is_pid_captured(me, s->pid)) continue;
        if (s->len < s->max) s->buf[s->len++] = c;
        kept = true;
    }
    spin_unlock_irqrestore(&g_cap_lock, f);
    return kept;
}

void console_putc(char c) {
    if (g_cap_active && capture_putc(c)) return;
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

/* accepted: one pointer store each, by `console bind`. */
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

/* accepted: a latch for the root console, set by the pump (under
 * g_input_lock) on Ctrl-C and cleared by the command that handled it. */
static volatile bool g_interrupt_pending = false;

/* Push-back ring. See kernel/console.h for why this exists and what it is
 * not. 128 bytes: enough for a few typed-ahead commands, small enough that
 * it can never be mistaken for a real input queue. How it is shared between
 * its producer and its consumer: see pushback_full() below. */
#define CONSOLE_PUSHBACK_SIZE 128u

/* guarded by: the single-producer/single-consumer protocol below. */
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

/* Feed input bytes to whichever terminal is currently active.
 * For vterm 0 (the root console / boot shell), routes to the pushback ring;
 * for secondary terminals (vterm 1..N), routes to their vterm rx_buf. */
static void console_feed_active_bytes(const char *s, uint32_t n) {
    if (!s || n == 0) return;
    if (g_active_win_type == 2 /* RIBBON_WIN_CANVAS */) {
        for (uint32_t i = 0; i < n; i++) {
            console_canvas_feed_key(s[i]);
        }
        return;
    }
    vterm_t *act = vterm_active();
    if (act && act->id > 0) {
        (void)vterm_feed_bytes(act, s, n);
    } else {
        ylock_acquire(&g_input_lock);
        for (uint32_t i = 0; i < n; i++) {
            if (pushback_full()) break;
            char c = s[i];
            if (c == 0x03) g_interrupt_pending = true;
            pushback_put(c);
        }
        ylock_release(&g_input_lock);
    }
}

/* 36.9: the registered input sources (kernel/include/kernel/console.h). */
/* single task: the boot, before any reader runs; constant after. */
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
        uint8_t vinst = 0;
        const view_ops_t *view = src->local ? focused_view(&vinst) : NULL;
        if (view) {
            /* A view's keys, decoded here (kernel/keyseq.h), the system's
             * Cmd shortcuts taken out first. */
            key_event_t k;
            while (src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (note_input(src->local)) continue;
                if (keyseq_push(&g_view_keys, c, &k) && !system_shortcut(k))
                    view_send(view, vinst, WIN_EV_KEY_DOWN, 0, 0, 0, 0, k.key, k.mods);
            }
            if (keyseq_push_flush(&g_view_keys, &k) && !system_shortcut(k))
                view_send(view, vinst, WIN_EV_KEY_DOWN, 0, 0, 0, 0, k.key, k.mods);
        } else if (src->local && g_active_win_type == 2 /* RIBBON_WIN_CANVAS */) {
            while (src->has_char()) {
                int c = src->getc();
                if (c < 0) break;
                if (note_input(src->local)) continue;
                if (c == 0x03) {
                    if (act) act->interrupt_pending = true;
                    else g_interrupt_pending = true;
                }
                console_canvas_feed_key((char)c);
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
