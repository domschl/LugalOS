/*
 * The panel terminal, portable -- 47.6, plan/phase47_esp32p4_lcd7b_ribbon.md.
 *
 * Moved out of drivers/lcd7_rp2350.c, where it grew in phases 36-38 and 44:
 * the screen_t on a 1-bpp frame, run by the U-mode `lcdterm` task (36.6a),
 * the batch the console and the kernel log feed it through, the menu bar's
 * title and status (37.1), canvas requests (37.3b), the tee to a host, and
 * the console's screen hooks. Nothing here touches a panel: the panel driver
 * brings its glass up and describes its frame in an lcdterm_backend_t
 * (drivers/lcdterm.h) -- the RP2350-LCD-7's PIO scan-out today, the
 * LCD-7B's DSI frame from 47.7.
 *
 * The RP2350 is the regression target: tests/hw/lcdterm_shots.py takes the
 * same eight screenshots before and after, and they must match pixel for
 * pixel.
 */

#include "drivers/lcdterm.h"

#include <string.h>

#include "drivers/fbtext.h"
#include "drivers/font8x16.h"
#include "drivers/uart.h"
#include "drivers/screen.h"
#include "drivers/vtterm.h"
#include "drivers/driver_task.h"
#include "drivers/lcdterm_attr.h"
#include "kernel/chan.h"
#include "kernel/device.h"
#include "kernel/ipc.h"
#include "kernel/mem_domain.h"
#include "kernel/sched.h"
#include "arch/umode.h"
#include "kernel/console.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include "kernel/vterm.h"
#include "fs/vfs.h"
#include "fs/9p.h"

#if defined(CONFIG_BOARD_RP2350)
#include "drivers/usb_cdc.h"
#endif
#if defined(CONFIG_BOARD_ESP32P4)
#include "drivers/uart.h"
#endif

#include <stdbool.h>
#include <stdint.h>

#if defined(LCDTERM_PRESENT)

/* --- The panel this terminal runs on (47.6) ------------------------------- */
static const lcdterm_backend_t *g_be;
static void damage(unsigned y0, unsigned y1);

/* --- Text (36.5) -----------------------------------------------------------
 *
 * The cell grid of 8 x 16 glyphs that tiles the panel: 100 x 30 on the
 * RP2350-LCD-7, 128 x 37 on the LCD-7B. */
#define TEXT_COLS            (g_be->w / FONT8X16_W)
#define TEXT_ROWS            (g_be->h / FONT8X16_H)

/* 37.1: the screen's state -- the chrome's, the terminal, and the cell
 * shadow (98 x 27 x 2 bytes since 37.1a) -- in its own 8 KB, 8 KB-aligned block, which
 * the lcdterm domain is granted as one more naturally aligned region. The
 * framebuffer's 1 152-byte spare tail, where 36.6a kept the terminal, is too
 * small for a shadow. Five regions in all (stack, text, framebuffer as two,
 * this), which is exactly what a domain may have (kernel/mem_domain.h). */
/* 47.6: sized from the panel -- the smallest power-of-two block, and never
 * smaller than the RP2350-LCD-7's, so its numbers stay exactly what they
 * were (16 pages here, 64 below). */
#define STATE_PAGES_MIN      16u
static uint32_t g_state_bytes;

/* 38.8, plan/phase38_psram.md: with PSRAM, the state block grows into a
 * 256 KB naturally aligned run of the bulk zone -- the state, then the
 * canvas stores (drivers/screen.h, 4 x 48 000 bytes). Still one region, so
 * the domain stays at five: the budget had no room for a sixth, and this
 * gives two SRAM pages back besides. The state is touched on every
 * character; `lcd outbench` measured the cost. Without a bulk zone, the
 * 8 KB block in SRAM, and no stores. */
#define STORE_BLOCK_PAGES_MIN 64u

static uint32_t pow2_pages(uint32_t bytes, uint32_t min_pages) {
    uint32_t need = (bytes + 4095u) / 4096u, p = min_pages;
    while (p < need) p <<= 1;
    return p;
}

static void text_line(fbtext_t *t, unsigned row, const char *s, bool inverse) {
    for (unsigned col = 0; *s && col < t->cols; col++, s++) fbtext_putc(t, col, row, *s, inverse);
}

/* --- The screen as a terminal (36.6) ----------------------------------------
 *
 * Two writers feed it, the console stream (lcdterm_console_putc) and the kernel
 * log's `lcd` sink (lcdterm_screen_putc), and both run under console_lock(), so
 * the emulator never sees two at once. Since 37.1 it is a screen_t
 * (drivers/screen.h): a status bar on the top row and the terminal below. */
/* --- 36.6a: the terminal as the U-mode `lcdterm` task ---------------------
 *
 * The task's domain is exactly: its stack, .lcdtermtext (code and font,
 * R/X), the framebuffer as a 32 KB and a 16 KB naturally aligned piece, and
 * the screen's 8 KB state block (37.1). No MMIO at all: drawing is plain
 * stores to RAM, and PIO/DMA stay kernel-owned.
 *
 * Writers batch into g_batch and hand it over in one chan_call(); the batch
 * is flushed at every console_flush() and before the console waits for input
 * (kernel/console.c's screen hooks), so an echoed keystroke appears at once.
 * Until the task is alive -- boot, or if it never starts -- the facade draws
 * directly, the fallback every driver task here has. */
static screen_t *g_scr;             /* the state block: g_scr_bytes, naturally aligned */
static uint32_t  g_scr_bytes;
static bool      g_vt_ready;

#define LCDTERM_BATCH     256u
#define LCDTERM_OP_WRITE  'W'       /* terminal output */
#define LCDTERM_OP_TITLE  'T'       /* 37.1: the status bar's title */
#define LCDTERM_OP_RIGHT  'S'       /* 37.1: its indicators */
#define LCDTERM_OP_REPAINT 'R'      /* 37.1a: everything, from the shadow */
#define LCDTERM_OP_CANVAS 'C'       /* 37.3b: a canvas request; the reply comes back */
static uint8_t          g_batch[LCDTERM_BATCH];
static uint32_t         g_batch_len;
#if defined(LCDTERM_UMODE)
static uint8_t          g_lcdterm_req[2u + LCDTERM_BATCH];   /* the U-mode task's request buffer */
#endif
static uint8_t          g_lcdterm_resp[SCREEN_REPLY_LEN];
static chan_endpoint_t *g_lcdterm_ep;
static int              g_lcdterm_pid = -1;
static uint32_t         g_lcdterm_calls;

static bool lcdterm_alive(void) {
    if (g_lcdterm_pid < 0 || !g_lcdterm_ep) return false;
    int st = sched_task_state(g_lcdterm_pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

#if defined(LCDTERM_UMODE)
/* Hand-rolled per file, as every U-mode driver here does: an LCDTERM_UTEXT
 * function must not call anything outside its own region. */
__attribute__((always_inline)) static inline long lcdterm_usys_serve_wait(const char *name, uint8_t *buf, long max) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_WAIT;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = max;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

__attribute__((always_inline)) static inline long lcdterm_usys_serve_reply(const char *name, const uint8_t *buf, long len) {
    register long r_a0 __asm__("a0") = SYS_CHAN_SERVE_REPLY;
    register long r_a1 __asm__("a1") = (long)name;
    register long r_a2 __asm__("a2") = (long)buf;
    register long r_a3 __asm__("a3") = len;
    __asm__ __volatile__("ecall" : "+r"(r_a0) : "r"(r_a1), "r"(r_a2), "r"(r_a3) : "memory");
    return r_a0;
}

/* The task, in U-mode. `arg` is the emulator's state (arch_enter_user() puts
 * the spec's arg in a0, as spisd's body relies on). The endpoint name is built
 * in a volatile stack array, not a literal: a literal is .rodata, outside
 * this domain -- the fault drivers/tm1638_rp2350.c documents. */
LCDTERM_UTEXT static void lcdterm_umode_body(uintptr_t arg) {
    screen_t *scr = (screen_t *)arg;
    volatile char name[8];
    name[0] = 'l'; name[1] = 'c'; name[2] = 'd'; name[3] = 't';
    name[4] = 'e'; name[5] = 'r'; name[6] = 'm'; name[7] = '\0';
    for (;;) {
        uint8_t req[2u + LCDTERM_BATCH];
        uint8_t reply[SCREEN_REPLY_LEN];
        long n = lcdterm_usys_serve_wait((const char *)name, req, (long)sizeof(req));
        if (n >= 1 && req[0] == LCDTERM_OP_CANVAS) {
            uint8_t vid = (n >= 2) ? req[1] : 0;
            const uint8_t *creq = (n >= 2) ? (req + 2) : (req + 1);
            uint32_t clen = (n >= 2) ? (uint32_t)(n - 2) : (uint32_t)(n - 1);
            screen_canvas_vterm(scr, vid, creq, clen, reply);
            lcdterm_usys_serve_reply((const char *)name, reply, (long)sizeof(reply));
            continue;
        }
        if (n >= 1) {
            const char *p = (const char *)req + 1;
            uint32_t len = (uint32_t)(n - 1);
            if (req[0] == LCDTERM_OP_WRITE) {
                uint8_t vid = (n >= 2) ? req[1] : 0;
                const char *wp = (const char *)req + 2;
                uint32_t wlen = (n >= 2) ? (uint32_t)(n - 2) : 0;
                screen_write_vterm(scr, vid, wp, wlen);
            }
            else if (req[0] == LCDTERM_OP_TITLE) screen_set_title(scr, p, len);
            else if (req[0] == LCDTERM_OP_RIGHT) {
                uint8_t flags = (len >= 1) ? (uint8_t)p[0] : 0;
                const char *tp = p + 1;
                uint32_t tlen = (len >= 1) ? (len - 1) : 0;
                screen_set_right(scr, flags, tp, tlen);
            }
            else if (req[0] == LCDTERM_OP_REPAINT) screen_repaint(scr);
        }
        lcdterm_usys_serve_reply((const char *)name, 0, 0);
    }
}

/* 2 KB: screen_write's deepest chain is a handful of frames, plus the
 * 257-byte request buffer. */
static uint8_t g_lcdterm_ustack[2048] __attribute__((aligned(2048)))
                                       __attribute__((section(".ustacks2048")));

static void lcdterm_task_body(void *arg) {
    (void)arg;
    while (!g_lcdterm_ep) sched_yield();
    /* The frame's pieces as the panel describes them, then the state block. */
    driver_umode_spec_t spec = {
        .name         = "lcdterm",
        .fallback     = "the terminal keeps drawing from the kernel.",
        .body         = (void (*)(void))lcdterm_umode_body,
        .text_region  = board_lcdterm_text_region,
        .stack_base   = (uintptr_t)g_lcdterm_ustack,
        .stack_size   = sizeof(g_lcdterm_ustack),
        .arg          = (uintptr_t)g_scr,
    };
    unsigned n = 0;
    for (unsigned i = 0; i < g_be->fb_region_count; i++)
        spec.regions[n++] = (driver_region_t){ g_be->fb_regions[i].base,
                                                       g_be->fb_regions[i].size, MEM_R | MEM_W };
    spec.regions[n++] = (driver_region_t){ (uintptr_t)g_scr, g_scr_bytes, MEM_R | MEM_W };
    spec.region_count = n;
    (void)driver_umode_enter(&spec);
}

int lcdterm_task_start(void) {
    if (!g_vt_ready) return -1;
    int pid = task_create_driver("lcdterm", lcdterm_task_body, NULL, 1);
    if (pid < 0) {
        printk("[LCD] could not start the lcdterm task; the terminal keeps drawing from the kernel\n");
        return -1;
    }
    if (chan_register_task("lcdterm", pid, g_lcdterm_req, sizeof(g_lcdterm_req),
                           g_lcdterm_resp, sizeof(g_lcdterm_resp)) != 0) {
        printk("[LCD] could not register the lcdterm endpoint; the terminal keeps drawing from the kernel\n");
        return -1;
    }
    g_lcdterm_pid = pid;
    g_lcdterm_ep = chan_lookup("lcdterm");
    printk("[LCD] terminal running as U-mode task #%d, reachable via chan_call(\"lcdterm\", ...)\n", pid);
    return pid;
}

#else /* no U-mode text region on this board: the kernel draws (47.7) */
int lcdterm_task_start(void) { return -1; }
#endif

uint32_t lcdterm_task_call_count(void) { return g_lcdterm_calls; }

/* One request to the task: op, then payload. False if the task is not
 * there to take it (it died, or never started), and the caller draws. */
static bool lcdterm_send(uint8_t op, const char *p, uint32_t len) {
    if (!lcdterm_alive()) return false;
    uint8_t req[2u + LCDTERM_BATCH];
    if (len > 1u + LCDTERM_BATCH) len = 1u + LCDTERM_BATCH;
    req[0] = op;
    for (uint32_t i = 0; i < len; i++) req[1u + i] = (uint8_t)p[i];
    for (int attempt = 0; attempt < 8; attempt++) {
        if (chan_call(g_lcdterm_ep, req, 1u + len, g_lcdterm_resp, sizeof(g_lcdterm_resp)) >= 0) {
            g_lcdterm_calls++;
            return true;
        }
        sched_yield();
    }
    return false;
}

static uint8_t g_batch_vid;

/* Called with the batch full, from console_flush(), and before input waits.
 * Takes console_lock itself (re-entrant) because writers append under it. */
void lcdterm_screen_flush(void) {
    if (g_batch_len == 0) return;
    console_lock();
    if (g_batch_len > 0) {
        uint32_t len = g_batch_len;
        uint8_t vid = g_batch_vid;
        g_batch_len = 0;
        /* The task died or never answered: draw it here rather than lose it. */
        uint8_t req[1u + 1u + LCDTERM_BATCH];
        req[0] = LCDTERM_OP_WRITE;
        req[1] = vid;
        for (uint32_t i = 0; i < len; i++) req[2u + i] = g_batch[i];
        if (!lcdterm_send(LCDTERM_OP_WRITE, (const char *)req + 1, 1u + len))
            screen_write_vterm(g_scr, vid, (const char *)g_batch, len);
        damage(0, g_be->h);
    }
    console_unlock();
}

void lcdterm_screen_putc_vterm(int vid, char c) {
    if (!g_vt_ready) return;
    /* The batch below is shared by every terminal's writers, and only
     * console_lock() makes that safe: an unlocked append preempted between
     * the increment and the flush test lets the next one run off the end
     * (phase31 §7 T5). Named once rather than assumed. */
    static bool s_unlocked_reported;
    if (!s_unlocked_reported && !console_lock_held()) {
        s_unlocked_reported = true;
        printk("[LCD BUG] console write without console_lock() from task %d\n",
               sched_current_pid());
    }
    if (g_batch_len > 0 && g_batch_vid != (uint8_t)vid) {
        lcdterm_screen_flush();
    }
    g_batch_vid = (uint8_t)vid;
    if (!lcdterm_alive()) {
        screen_write_vterm(g_scr, (uint8_t)vid, &c, 1);
        damage(0, g_be->h);
        return;
    }
    g_batch[g_batch_len++] = (uint8_t)c;
    if (g_batch_len == LCDTERM_BATCH) lcdterm_screen_flush();
}

void lcdterm_screen_putc(char c) {
    lcdterm_screen_putc_vterm(vterm_current_id(), c);
}

/* The batch goes first, so a title and the output before it arrive in the
 * order they were made. */
static void lcdterm_op(uint8_t op, const char *p, uint32_t len) {
    console_lock();
    lcdterm_screen_flush();
    if (!lcdterm_send(op, p, len)) {
        if (op == LCDTERM_OP_TITLE) screen_set_title(g_scr, p, len);
        else if (op == LCDTERM_OP_RIGHT) {
            uint8_t flags = (len >= 1) ? (uint8_t)p[0] : 0;
            const char *tp = (len >= 1) ? (p + 1) : "";
            uint32_t tlen = (len >= 1) ? (len - 1) : 0;
            screen_set_right(g_scr, flags, tp, tlen);
        }
        else screen_repaint(g_scr);
    }
    damage(0, g_be->h);
    console_unlock();
}

/* 37.3b: a canvas request through the task, its reply copied back; drawn
 * here if the task is not there. The text batch goes first, so a layout
 * change lands after the output written before it. */
static bool lcdterm_canvas(const uint8_t *req, uint32_t n, uint8_t *reply) {
    if (!g_vt_ready || n == 0 || n > LCDTERM_BATCH) return false;
    console_lock();
    lcdterm_screen_flush();
    bool sent = false;
    int pid = sched_current_pid();
    int my_vid = (pid >= 0) ? task_get_vterm(pid) : 0;
    if (my_vid < 0) my_vid = 0;

    if (lcdterm_alive()) {
        uint8_t buf[2u + LCDTERM_BATCH];
        buf[0] = LCDTERM_OP_CANVAS;
        buf[1] = (uint8_t)my_vid;
        for (uint32_t i = 0; i < n; i++) buf[2u + i] = req[i];
        for (int attempt = 0; attempt < 8 && !sent; attempt++) {
            if (chan_call(g_lcdterm_ep, buf, 2u + n, g_lcdterm_resp, sizeof(g_lcdterm_resp)) >= 0) {
                g_lcdterm_calls++;
                for (uint32_t i = 0; i < SCREEN_REPLY_LEN; i++) reply[i] = g_lcdterm_resp[i];
                sent = true;
            } else {
                sched_yield();
            }
        }
    }
    if (!sent) screen_canvas_vterm(g_scr, (uint8_t)my_vid, req, n, reply);
    damage(0, g_be->h);
    console_unlock();
    return true;
}

void lcdterm_repaint(void) {
    if (g_vt_ready) lcdterm_op(LCDTERM_OP_REPAINT, "", 0);
}

void lcdterm_set_title(const char *title) {
    if (!g_vt_ready) return;
    uint32_t n = (uint32_t)strlen(title);
    /* The task owns the state; reading it here is only a check that saves a
     * round trip when the title is already showing. */
    if (n < VT_TITLE_MAX && strcmp(g_scr->vt.title, title) == 0) return;
    lcdterm_op(LCDTERM_OP_TITLE, title, n);
}

/* The menu bar's right side: the console's indicators (37.2: the
 * keyboard's compose state), then the clock -- HH:MM once the clock has been
 * set, nothing before (an unset clock shows the build's instant, which would
 * be a lie). Rebuilt on every console flush, which runs on every turn of an
 * input wait and at every write, and sent only when it changed; the clock
 * itself is read at most once a second. A program that computes for minutes
 * in silence leaves it stale until it next writes or reads. */
static uint64_t g_clock_next_us;
static char     g_clock[6];             /* "HH:MM", or "" */

static void status_tick(void) {
    uint64_t now = time_get_us();
    if (now >= g_clock_next_us) {
        g_clock_next_us = now + 1000000u;
        vfs_volume_probe("sd0");       /* the SD icon is the card, not the boot */
        g_clock[0] = '\0';
        if (time_is_set()) {
            rtc_time_t tm;
            time_get_local(&tm);
            g_clock[0] = (char)('0' + tm.hour / 10); g_clock[1] = (char)('0' + tm.hour % 10);
            g_clock[2] = ':';
            g_clock[3] = (char)('0' + tm.min / 10);  g_clock[4] = (char)('0' + tm.min % 10);
            g_clock[5] = '\0';
        }
    }
    char right[SCREEN_RIGHT_MAX];
    uint32_t n = console_indicators(right, sizeof(right));
    if (n > 0 && g_clock[0] && n + 2u < sizeof(right)) { right[n++] = ' '; right[n++] = ' '; }
    for (uint32_t i = 0; g_clock[i] && n + 1u < sizeof(right); i++) right[n++] = g_clock[i];
    right[n] = '\0';
    uint8_t flags = 0;
    if (vfs_volume_mounted("sd0")) flags |= SCREEN_STATUS_SD;
    if (p9_is_connected()) flags |= SCREEN_STATUS_P9;
    if (strcmp(right, g_scr->right) != 0 || flags != g_scr->status_flags) {
        char payload[1 + SCREEN_RIGHT_MAX];
        payload[0] = (char)flags;
        for (uint32_t i = 0; i <= n; i++) payload[1 + i] = right[i];
        lcdterm_op(LCDTERM_OP_RIGHT, payload, 1 + n);
    }
}

static void lcdterm_console_flush(void) {
    if (!g_vt_ready) return;
    lcdterm_screen_flush();
    status_tick();
}

static bool lcdterm_console_size(unsigned *cols, unsigned *rows) {
    if (!g_vt_ready) return false;
    screen_text_size(g_scr, cols, rows);
    return true;
}

/* 37.5a: the frame, for a screenshot. Read from the kernel while the task
 * may be drawing: a screenshot taken mid-scroll shows that scroll half done,
 * which is what was on the glass. */
static bool lcdterm_pixels(const uint8_t **fb, unsigned *w, unsigned *h, unsigned *stride) {
    if (!g_be) return false;
    *fb = g_be->fb;
    *w = g_be->w;
    *h = g_be->h;
    *stride = g_be->stride;
    return true;
}

static void lcdterm_blank(bool off) {
    if (g_be && g_be->blank) g_be->blank(off);
}

static const console_screen_t g_console_screen = {
    .flush     = lcdterm_console_flush,
    .size      = lcdterm_console_size,
    .set_title = lcdterm_set_title,
    .canvas    = lcdterm_canvas,
    .pixels    = lcdterm_pixels,
    .blank     = lcdterm_blank,
};

const console_screen_t *lcdterm_console_screen(void) {
    return g_vt_ready ? &g_console_screen : NULL;
}

#if defined(LCDTERM_UMODE)
/* `lcdtermisotest`: the lcdterm domain, but a body that stores into kernel
 * memory. Must fault, and the canary must survive -- the check every isolated
 * driver here has (st7735isotest, blkisotest, ...). */
static volatile uintptr_t g_lcdterm_canary = 0xC0FFEE;
static volatile bool      g_lcdterm_intruder_entered;

LCDTERM_UTEXT static void lcdterm_intruder(void) {
    g_lcdterm_canary = 0xDEAD;
    for (;;) { }                    /* only reached if the store was not stopped */
}

static void lcdterm_intruder_task_body(void *arg) {
    uint8_t *ustack = (uint8_t *)arg;
    mem_domain_t dom;
    mem_domain_init(&dom);
    mem_domain_add(&dom, (uintptr_t)ustack, 4096, MEM_R | MEM_W);
    uintptr_t tbase, tsize;
    board_lcdterm_text_region(&tbase, &tsize);
    mem_domain_add(&dom, tbase, tsize, MEM_R | MEM_X);
    for (unsigned i = 0; i < g_be->fb_region_count; i++)
        mem_domain_add(&dom, g_be->fb_regions[i].base, g_be->fb_regions[i].size, MEM_R | MEM_W);
    mem_domain_add(&dom, (uintptr_t)g_scr, g_state_bytes, MEM_R | MEM_W);
    if (task_set_domain(sched_current_pid(), &dom) != 0) {
        printk("[LcdtermIso] refusing to enter U-mode: memory domain not enforceable\n");
        return;
    }
    g_lcdterm_intruder_entered = true;
    arch_enter_user(lcdterm_intruder, (uintptr_t)ustack + 4096, 0, 0, 0);
}

bool lcdterm_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    g_lcdterm_canary = 0xC0FFEE;
    g_lcdterm_intruder_entered = false;
    if (!g_be || !g_scr) return false;
    void *ustack = palloc_pages(1);
    if (!ustack) return false;
    int pid = task_create("lcdterm_intruder", lcdterm_intruder_task_body, ustack);
    if (pid < 0) {
        palloc_free(ustack, 1);
        return false;
    }
    for (int i = 0; i < 10000 && sched_task_state(pid) != TASK_DEAD; i++) sched_yield();
    *out_exited_clean = (sched_task_state(pid) == TASK_DEAD);
    *out_canary = g_lcdterm_canary;
    palloc_free(ustack, 1);
    return g_lcdterm_intruder_entered;
}

#else
bool lcdterm_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) {
    (void)out_canary; (void)out_exited_clean;
    return false;
}
#endif

/* The `lcd` console device: the screen, plus a tee so a host sees what the
 * screen shows. Where the tee goes is a setting (`lcd tee`), because the
 * UART path paces everything: uart_putc() feeds UART0 at 115200 baud and
 * mirrors to USB ACM0, and the screen then draws no faster than 11.5 KB/s --
 * visible as the `e` editor painting its box line by line (the owner's
 * observation, 2026-09-30). */
/* USB by default. Measured on the board with `lcd outbench` (1980 characters
 * through console_putc): tee to the UART path 11 313 chars/s -- exactly
 * 115200 baud -- against 89 795 chars/s to ACM0 alone. The host console is on
 * USB anyway; nothing is attached to header H7 unless someone puts an adapter
 * there, and then `lcd tee uart` is one command. The kernel log still reaches
 * UART0 through its own sink either way. */
#if defined(CONFIG_BOARD_RP2350)
static uint8_t g_tee = LCD_TEE_USB;
#else
static uint8_t g_tee = LCD_TEE_UART;    /* 47.6: no USB device stack; UART0 is the host's console */
#endif

void lcdterm_set_tee(unsigned mode) {
    if (mode <= LCD_TEE_OFF) g_tee = (uint8_t)mode;
}

unsigned lcdterm_tee(void) {
    return g_tee;
}

void lcdterm_console_putc(char c) {
    lcdterm_screen_putc(c);
    if (g_tee == LCD_TEE_UART) uart_putc(c);
#if defined(CONFIG_BOARD_RP2350)
    else if (g_tee == LCD_TEE_USB) (void)usb_cdc_putc_wait(c, 20000u);   /* never drop mid-sequence */
#endif
}

uint32_t lcdterm_unknown_sequences(void) {
    return g_vt_ready ? g_scr->vt.unknown : 0;
}

/* `lcd test text`: every glyph, a pangram, reversed video, and a cursor. */
int lcdterm_text_test(void) {
    if (!g_be) return -1;
    fbtext_t t;
    fbtext_init(&t, g_be->fb, g_be->stride, TEXT_COLS, TEXT_ROWS);
    fbtext_clear_rows(&t, 0, TEXT_ROWS);
    text_line(&t, 0, g_be->text_test_title, false);
    unsigned row = 2, col = 0;
    for (int c = FONT8X16_FIRST; c <= FONT8X16_LAST; c++) {
        fbtext_putc(&t, col, row, (char)c, false);
        if (++col == 64u) { col = 0; row++; }
    }
    text_line(&t, 5, "The quick brown fox jumps over the lazy dog. 0123456789", false);
    text_line(&t, 6, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG! (){}[]<>=+-*/\\|~^", false);
    text_line(&t, 8, " Reverse video: SGR 7 is exact on a 1-bpp screen. ", true);
    text_line(&t, 10, "Cursor after this >", false);
    fbtext_cursor_xor(&t, 19, 10);
    for (unsigned c = 0; c < TEXT_COLS; c++) fbtext_putc(&t, c, TEXT_ROWS - 1u, (char)('0' + c % 10u), false);
    damage(0, g_be->h);
    return 0;
}

/* `lcd scroll <n>`: n numbered lines, each written after scrolling the grid up
 * one row -- what `cat` of a long file does. Times each scroll alone. */
void lcdterm_scroll_test(unsigned n) {
    if (!g_be) return;
    if (n == 0) n = 1000u;
    fbtext_t t;
    fbtext_init(&t, g_be->fb, g_be->stride, TEXT_COLS, TEXT_ROWS);
    uint64_t total = 0, worst = 0, best = ~(uint64_t)0;
    uint64_t t_all = time_get_us();
    char buf[256];
    for (unsigned i = 1; i <= n; i++) {
        uint64_t t0 = time_get_us();
        fbtext_scroll_up(&t, 1);
        uint64_t d = time_get_us() - t0;
        total += d;
        if (d > worst) worst = d;
        if (d < best) best = d;
        ksnprintf(buf, sizeof(buf), "line %5u of %u: the quick brown fox jumps over the lazy dog", i, n);
        text_line(&t, TEXT_ROWS - 1u, buf, false);
        damage(0, g_be->h);
    }
    uint64_t all = time_get_us() - t_all;
    cprintf("lcd scroll: %u lines in %lu ms; one scroll (a %lu-byte memmove) took %lu us on average, "
            "best %lu, worst %lu\n",
            n, (unsigned long)(all / 1000u),
            (unsigned long)((TEXT_ROWS - 1u) * FONT8X16_H * g_be->stride),
            (unsigned long)(total / n), (unsigned long)best, (unsigned long)worst);
}


/* Rows [y0, y1) were drawn: a converting backend copies them to its glass
 * (47.7); a scan-out that reads the frame itself has no flush, and this
 * costs nothing. Whole-frame for now: 47.7 adds the per-row dirty bitmap. */
static void damage(unsigned y0, unsigned y1) {
    if (g_be && g_be->flush) g_be->flush(y0, y1);
}

/* The screen's state block and canvas stores, then the screen on the frame.
 * With a bulk zone: one naturally aligned run of it holding the state and
 * the stores (38.8). Without: the state alone, in SRAM. */
int lcdterm_init(const lcdterm_backend_t *be) {
    if (!be || !be->fb || (be->w % 8u) != 0) return -1;
    g_be = be;
    uint32_t state_pages = pow2_pages(SCREEN_BYTES(be->w, be->h), STATE_PAGES_MIN);
    g_state_bytes = state_pages * 4096u;
    uint32_t block_pages = pow2_pages(g_state_bytes + SCREEN_STORE_BYTES(be->w, be->h),
                                      STORE_BLOCK_PAGES_MIN);

    g_scr = (screen_t *)palloc_pages_bulk_aligned(block_pages, block_pages);
    if (g_scr && !palloc_is_bulk(g_scr)) {      /* the fast zone's: too dear */
        palloc_free(g_scr, block_pages);
        g_scr = NULL;
    }
    g_scr_bytes = block_pages * 4096u;
    bool stores = (g_scr != NULL);
    if (!g_scr) {
        g_scr = (screen_t *)palloc_pages_aligned(state_pages, state_pages);
        g_scr_bytes = g_state_bytes;
    }
    if (!g_scr) {
        printk("[LCD] no memory for the screen's state; the panel runs without a terminal\n");
        return -1;
    }
    screen_init(g_scr, be->fb, be->stride, be->w, be->h);
    if (stores)
        screen_set_store(g_scr, (uint8_t *)g_scr + g_state_bytes, g_scr_bytes - g_state_bytes);
    g_vt_ready = true;
    damage(0, be->h);
    return 0;
}

/* The terminal's half of `lcd`; the panel driver prints its own. */
void lcdterm_report(void) {
    if (g_vt_ready) {
        cprintf("lcd: text window %u x %u in its tile, cursor at row %u col %u, "
                "%lu unknown sequences swallowed; title \"%s\"\n",
                (unsigned)g_scr->vt.text.cols, (unsigned)g_scr->vt.text.rows,
                (unsigned)g_scr->vt.row, (unsigned)g_scr->vt.col,
                (unsigned long)g_scr->vt.unknown, g_scr->vt.title);
        /* 38.8: where the state is, and which canvases are stored. */
        char stores[32] = "none";
        if (g_scr->store)
            ksnprintf(stores, sizeof stores, "%u x %lu bytes", (unsigned)SCREEN_STORES,
                      (unsigned long)(g_be->w / 8u * g_be->h));
        cprintf("lcd: screen state %lu KB at 0x%08lx (%s), canvas stores %s, held 0x%x, damage %u\n",
                (unsigned long)(g_scr_bytes / 1024u), (unsigned long)(uintptr_t)g_scr,
                palloc_is_bulk(g_scr) ? "PSRAM" : "SRAM", stores,
                (unsigned)g_scr->store_ok, (unsigned)g_scr->damage);
    }
    cprintf("lcd: terminal %s (task #%d, %lu batches served)\n",
            lcdterm_alive() ? "in the U-mode lcdterm task" : "drawn from the kernel",
            g_lcdterm_pid, (unsigned long)g_lcdterm_calls);
}

#else /* no panel */

int lcdterm_init(const lcdterm_backend_t *be) { (void)be; return -1; }
int lcdterm_task_start(void) { return -1; }
void lcdterm_screen_putc(char c) { (void)c; }
void lcdterm_screen_putc_vterm(int vid, char c) { (void)vid; (void)c; }
void lcdterm_console_putc(char c) { (void)c; }
void lcdterm_screen_flush(void) { }
void lcdterm_set_tee(unsigned mode) { (void)mode; }
unsigned lcdterm_tee(void) { return LCD_TEE_OFF; }
const console_screen_t *lcdterm_console_screen(void) { return NULL; }
void lcdterm_set_title(const char *title) { (void)title; }
void lcdterm_repaint(void) { }
uint32_t lcdterm_unknown_sequences(void) { return 0; }
uint32_t lcdterm_task_call_count(void) { return 0; }
bool lcdterm_isolation_test(uintptr_t *out_canary, bool *out_exited_clean) { (void)out_canary; (void)out_exited_clean; return false; }
int lcdterm_text_test(void) { return -1; }
void lcdterm_scroll_test(unsigned n) { (void)n; }
void lcdterm_report(void) { }

#endif /* LCDTERM_PRESENT */
