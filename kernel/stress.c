/* `stress [seconds]` / `stress bg [seconds]`: load that makes collisions
 * likely instead of lucky.
 *
 * Worker tasks run the paths that more than one task shares, in tight loops,
 * and check every result they get:
 *   fmt    ksnprintf() of a known line, compared byte for byte
 *   ram    a file of its own on /ram0: write, read back, compare, remove
 *   sd     the same on /sd0 (the card and its block task)
 *   p9     /proc/version through the local 9P server (the "p9" endpoint)
 *   chan   chan_call_wait() into "stressq", whose handler holds its turn
 *          for 0-2 ms, so callers queue
 *   log    printk() bursts, so the klog ring wraps under the others
 *   shell  native commands and a Lisp form through the shell, output captured
 *   cache  (ESP32-P4) a line-aligned scratch buffer of its own, in SRAM and in
 *          PSRAM: pattern written, written back, invalidated, read back. Mostly
 *          load: cache operations from many contexts at once (the controller's
 *          sync registers are one for the chip), so a broken serialisation shows
 *          on the sd path within minutes; a short operation of its own is rarely
 *          the one that is interrupted
 *   rot    the Tab5's panel turned every 7 s (the orientation test hook)
 *
 * The workers are mixed: each picks a path at random every iteration, so two
 * meet on one path by chance rather than by construction, and the load fits
 * beside a board's own 16-18 tasks and its terminals. As many as the task
 * table allows, up to W_MIX_MAX; fewer than W_MIX_MIN is a failed run. The
 * Tab5's rotation has a task of its own.
 *
 * At the end: iterations and errors per path, and what the lock checker, the
 * I2C bus and the stacks saw meanwhile (deltas since the start). Prints
 * STRESS_OK or STRESS_FAIL. One run at a time. */
#include "kernel/stress.h"
#include "kernel/chan.h"
#include "kernel/console.h"
#include "kernel/klog.h"
#include "kernel/lock.h"
#include "kernel/printk.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/shell.h"
#include "kernel/time.h"
#include "drivers/i2c_bus.h"
#include "drivers/loopback_net.h"
#include "fs/vfs.h"
#include "lugalos_config.h"
#if defined(CONFIG_BOARD_TAB5)
#include "drivers/dsi_esp32p4.h"
#include "kernel/orient.h"
#endif
#include <string.h>

enum { P_FMT, P_RAM, P_SD, P_P9, P_CHAN, P_LOG, P_SHELL, P_CACHE, P_ROT, P_COUNT };
static const char *const P_NAME[P_COUNT] = { "fmt", "ram", "sd", "p9", "chan", "log", "shell", "cache", "rot" };
/* How often a mixed worker picks each path (rot is never picked: its own task). */
#if defined(CONFIG_BOARD_ESP32P4)
static const uint8_t P_WEIGHT[P_ROT] = { 4, 4, 1, 3, 4, 1, 2, 2 };
#else
static const uint8_t P_WEIGHT[P_ROT] = { 4, 4, 1, 3, 4, 1, 2, 0 };
#endif

#define W_MIX_MAX 8
#define W_MIX_MIN 3

typedef struct {
    uint8_t           index;
    bool              rot;
    volatile uint32_t iters;
    volatile bool     done;
    uint32_t          stack_depth;   /* deepest sp seen, bytes (sched_stack_sp_depth()) */
    uint32_t          stack_size;
} worker_t;

typedef struct {
    uint32_t iters, errors;      /* atomic adds */
    char     first_error[48];    /* written by whoever takes errors from 0 to 1 */
} path_stats_t;

/* accepted: one run at a time (g_stress_session); a worker writes only its
 * own slot, the path counters by atomic adds, and the run reads them as a
 * snapshot when the workers are done. */
static session_claim_t   g_stress_session;
static worker_t          g_workers[W_MIX_MAX + 1];
static path_stats_t      g_path[P_COUNT];
static volatile uint32_t g_shells_busy;      /* shell paths in progress: console capture has 4 places */
static volatile uint64_t g_until_ms;
static bool              g_have_sd;
static bool              g_rotate = true;   /* `stress ... norot` clears it for one run */
static uint32_t          g_only;            /* paths the next run may pick (bit per P_*); 0: all */
static chan_endpoint_t  *g_ep;
static uint8_t           g_req[16], g_resp[16];

static uint32_t rnd(void) {
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}

static void fail(unsigned path, const char *why) {
    if (__atomic_fetch_add(&g_path[path].errors, 1u, __ATOMIC_RELAXED) == 0) {
        strncpy(g_path[path].first_error, why, sizeof g_path[path].first_error - 1);
        g_path[path].first_error[sizeof g_path[path].first_error - 1] = '\0';
    }
}

static int stressq_handler(void *ctx, const uint8_t *req, uint32_t len,
                           uint8_t *resp, uint32_t max) {
    (void)ctx;
    if (len < 4 || max < 4) return -1;
    uint32_t hold = req[0] % 3u;          /* 0-2 ms with the endpoint held */
    if (hold) task_sleep_ms(hold);
    for (int i = 0; i < 4; i++) resp[i] = (uint8_t)~req[i];
    return 4;
}

static void do_fmt(const worker_t *w) {
    char a[96], b[96];
    uint32_t x = rnd();
    int n = ksnprintf(a, sizeof a, "w%u:%08x:%ld:%c:%5d|", (unsigned)w->index,
                      (unsigned)x, (long)(int32_t)x, 'A' + (char)(x % 26u), (int)(x % 99999u));
    /* the same line, built without the formatter */
    static const char hex[] = "0123456789abcdef";
    char *q = b;
    *q++ = 'w';
    *q++ = (char)('0' + w->index);
    *q++ = ':';
    for (int i = 7; i >= 0; i--) *q++ = hex[(x >> (4 * i)) & 0xfu];
    *q++ = ':';
    int32_t sv = (int32_t)x;
    char dig[12];
    int nd = 0;
    uint32_t mag = sv < 0 ? (uint32_t)(-(int64_t)sv) : (uint32_t)sv;
    do { dig[nd++] = (char)('0' + mag % 10u); mag /= 10u; } while (mag);
    if (sv < 0) *q++ = '-';
    while (nd) *q++ = dig[--nd];
    *q++ = ':';
    *q++ = (char)('A' + (char)(x % 26u));
    *q++ = ':';
    uint32_t v5 = x % 99999u;
    nd = 0;
    do { dig[nd++] = (char)('0' + v5 % 10u); v5 /= 10u; } while (v5);
    for (int pad = nd; pad < 5; pad++) *q++ = ' ';
    while (nd) *q++ = dig[--nd];
    *q++ = '|';
    *q = '\0';
    if (n != (int)(q - b) || strcmp(a, b) != 0) fail(P_FMT, "ksnprintf output differs");
}

static void do_file(const worker_t *w, unsigned path, const char *dir) {
    char name[40], data[200], back[200];
    ksnprintf(name, sizeof name, "%s/ST%u%u.TMP", dir, path, (unsigned)w->index);
    uint32_t len = 16u + rnd() % (sizeof data - 16u);
    uint32_t seed = rnd();
    for (uint32_t i = 0; i < len; i++) data[i] = (char)('a' + (seed + i * 7u) % 26u);
    int fd = vfs_open(name, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) { printk("[stress] %s: open for writing failed\n", name); fail(path, "write"); return; }
    int wn = vfs_pwrite(fd, data, len, 0);
    vfs_close(fd);
    if (wn != (int)len) {
        printk("[stress] %s: wrote %d of %lu\n", name, wn, (unsigned long)len);
        fail(path, "write");
        return;
    }
    int n = vfs_read(name, back, sizeof back);
    if (n != (int)len || memcmp(data, back, len) != 0) {
        /* What came back instead says where it came from: zeros, a shifted
         * copy, another worker's letters, or a sector of something else. */
        uint32_t at = 0;
        while (at < len && (int)at < n && data[at] == back[at]) at++;
        char got[17], want[17];
        uint32_t k = 0;
        for (; k < 16u && at + k < len; k++) {
            char c = (int)(at + k) < n ? back[at + k] : '~';
            got[k] = (c >= ' ' && c < 0x7f) ? c : '.';
            want[k] = data[at + k];
        }
        got[k] = want[k] = '\0';
        /* Read again: right this time means the card holds the data and the
         * first read was what failed. */
        char again[200];
        int n2 = vfs_read(name, again, sizeof again);
        bool second_ok = n2 == (int)len && memcmp(data, again, len) == 0;
        printk("[stress] %s: wrote %lu, read %d, first difference at %lu: got \"%s\" want \"%s\"; "
               "read again: %s\n", name, (unsigned long)len, n, (unsigned long)at, got, want,
               second_ok ? "right" : "still wrong");
        fail(path, "read back differs");
    }
    if (vfs_remove(name) < 0) fail(path, "remove");
}

static void do_p9(void) {
    char out[160];
    int n = loopback_9p_cat("/proc/version", out, sizeof out - 1);
    if (n <= 0) { fail(P_P9, "9P read failed"); return; }
    out[n] = '\0';
    if (!strstr(out, "LugalOS")) fail(P_P9, "9P read wrong data");
}

static void do_chan(void) {
    uint8_t q[4], r[4];
    uint32_t x = rnd();
    memcpy(q, &x, 4);
    int n = chan_call_wait(g_ep, q, 4, r, 4, 500u);
    if (n != 4) { fail(P_CHAN, "no turn in 500 ms"); return; }
    for (int i = 0; i < 4; i++)
        if (r[i] != (uint8_t)~q[i]) { fail(P_CHAN, "reply is not this call's"); return; }
}

/* Bursts that wrap the ring (and are counted as gaps), at a rate the
 * console's copy of them does not turn into the bottleneck. */
static void do_log(uint32_t k) {
    for (int i = 0; i < 2; i++)
        printk("[stress] log %u burst line %d of 2, filler to make the record longer\n",
               (unsigned)k, i);
    task_sleep_ms(200);
}

static void do_shell(uint32_t k) {
    static const char *const cmds[] = { "cat /proc/version", "ls /ram0", "(+ 40 2)", "cat /proc/meminfo" };
    static const char *const want[] = { "LugalOS", "", "42", "" };
    if (__atomic_fetch_add(&g_shells_busy, 1u, __ATOMIC_ACQUIRE) >= 2u) {   /* two at once at most */
        __atomic_fetch_sub(&g_shells_busy, 1u, __ATOMIC_RELEASE);
        return;
    }
    unsigned c = k % 4u;
    char out[512];
    console_capture(out, sizeof out);
    shell_exec(cmds[c]);
    uint32_t n = console_capture_end();
    __atomic_fetch_sub(&g_shells_busy, 1u, __ATOMIC_RELEASE);
    if (want[c][0] && (n == 0 || !strstr(out, want[c]))) fail(P_SHELL, cmds[c]);
}

static unsigned pick_path(void) {
    unsigned sum = 0;
    for (unsigned p = 0; p < P_ROT; p++) if (!g_only || (g_only & (1u << p))) sum += P_WEIGHT[p];
    if (!sum) return P_FMT;
    unsigned r = rnd() % sum;
    for (unsigned p = 0; p < P_ROT; p++) {
        if (g_only && !(g_only & (1u << p))) continue;
        if (r < P_WEIGHT[p]) return (p == P_SD && !g_have_sd) ? P_RAM : p;
        r -= P_WEIGHT[p];
    }
    return P_FMT;
}

#if defined(CONFIG_BOARD_ESP32P4)
#include "arch/esp32p4_intr.h"
#include "kernel/palloc.h"
/* Per worker: 256 bytes of SRAM and 16 KB of PSRAM, each whole cache lines.
 * The PSRAM slice is long on purpose: an operation is lost when another
 * context's lands while it is in progress, so the long one is the victim.
 * single task: each slice its worker's; the PSRAM page is taken before the
 * workers start and given back after they stop. */
#define CACHE_SLICE      256u
#define CACHE_EXT_SLICE  (16u * 1024u)
#define CACHE_EXT_PAGES  ((W_MIX_MAX + 1u) * CACHE_EXT_SLICE / 4096u)
static uint8_t  g_cache_sram[W_MIX_MAX + 1][CACHE_SLICE] __attribute__((aligned(64)));
static uint8_t *g_cache_psram;

static bool cache_round(uint8_t *buf, uint32_t len, bool ext, uint32_t seed) {
    for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)(seed + i * 13u);
    if (ext) { esp32p4_extmem_writeback((uintptr_t)buf, len); esp32p4_extmem_invalidate((uintptr_t)buf, len); }
    else     { esp32p4_dcache_writeback((uintptr_t)buf, len); esp32p4_dcache_invalidate((uintptr_t)buf, len); }
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != (uint8_t)(seed + i * 13u)) return false;
    return true;
}

static void do_cache(const worker_t *w) {
    uint32_t seed = rnd();
    if (!cache_round(g_cache_sram[w->index], CACHE_SLICE, false, seed)) fail(P_CACHE, "SRAM line lost its writeback");
    if (g_cache_psram && !cache_round(g_cache_psram + (size_t)w->index * CACHE_EXT_SLICE, CACHE_EXT_SLICE, true,
                                      seed ^ 0x5au))
        fail(P_CACHE, "PSRAM line lost its writeback");
}
#endif

static void worker_body(void *arg) {
    worker_t *w = (worker_t *)arg;
    while (time_get_ms() < g_until_ms) {
        unsigned p = w->rot ? P_ROT : pick_path();
        switch (p) {
        case P_FMT:   do_fmt(w); break;
        case P_RAM:   do_file(w, P_RAM, "/ram0"); break;
        case P_SD:    do_file(w, P_SD, "/sd0"); task_sleep_ms(20); break;
        case P_P9:    do_p9(); break;
        case P_CHAN:  do_chan(); break;
        case P_LOG:   do_log(w->iters); break;
        case P_SHELL: do_shell(w->iters); task_sleep_ms(10); break;
#if defined(CONFIG_BOARD_ESP32P4)
        case P_CACHE: do_cache(w); break;
#endif
        case P_ROT:
#if defined(CONFIG_BOARD_TAB5)
        {
            static const int rots[] = { 270, 0, 90, 180 };
            orient_manual();
            if (dsi_lcd_set_orientation(rots[w->iters % 4u]) != 0) fail(P_ROT, "orientation refused");
            task_sleep_ms(7000);
        }
#endif
            break;
        default: break;
        }
        __atomic_fetch_add(&g_path[p].iters, 1u, __ATOMIC_RELAXED);
        w->iters++;
        if ((w->iters & 15u) == 0) sched_yield();
    }
    int me = sched_current_pid();
    w->stack_depth = sched_stack_sp_depth(me);
    if (sched_stack_used(me) > w->stack_depth) w->stack_depth = sched_stack_used(me);
    w->stack_size = sched_stack_size(me);
    w->done = true;
}

typedef struct {
    uint32_t lock_faults, inversions, klog_gaps;
    i2c_fail_counts_t i2c;
    vfs_handle_stats_t vfs;
} snapshot_t;

static void snapshot(snapshot_t *s) {
    lock_order_stats_t o;
    lock_order_stats(&o);
    s->lock_faults = lock_faults();
    s->inversions = o.inversions;
    s->klog_gaps = klog_gaps();
    i2c_fail_counts(&s->i2c);
    vfs_handle_stats(&s->vfs);
}

void stress_set_rotation(bool on) { g_rotate = on; }

void stress_set_paths(const char *list) {
    g_only = 0;
    if (!list) return;
    for (unsigned p = 0; p < P_ROT; p++) {
        const char *n = P_NAME[p];
        size_t len = strlen(n);
        for (const char *q = list; *q; q++) {
            bool start = q == list || q[-1] == ',' || q[-1] == '=';
            if (start && strncmp(q, n, len) == 0 && (q[len] == ',' || q[len] == ' ' || q[len] == '\0'))
                g_only |= 1u << p;
        }
    }
}

int stress_run(uint32_t seconds) {
    if (!session_claim(&g_stress_session, "stress")) return -1;
    if (seconds == 0) seconds = 60;
    if (!g_ep) {
        if (chan_register("stressq", stressq_handler, NULL, g_req, sizeof g_req,
                          g_resp, sizeof g_resp) != 0 && !chan_lookup("stressq")) {
            cprintf("STRESS_FAIL (no endpoint)\n");
            session_release(&g_stress_session);
            return -1;
        }
        g_ep = chan_lookup("stressq");
    }
    vfs_stat_t st;
    g_have_sd = vfs_stat("/sd0", &st) == 0;
    memset(g_path, 0, sizeof g_path);
    snapshot_t s0, s1;
    snapshot(&s0);
    g_until_ms = time_get_ms() + (uint64_t)seconds * 1000u;
#if defined(CONFIG_BOARD_ESP32P4)
    g_cache_psram = palloc_pages_bulk(CACHE_EXT_PAGES);   /* NULL without PSRAM: the SRAM half still runs */
#endif

    unsigned nw = 0, mixed = 0;
#if defined(CONFIG_BOARD_TAB5)
    if (g_rotate) {
        worker_t *w = &g_workers[nw];
        memset(w, 0, sizeof *w);
        w->rot = true;
        if (task_create_sized("stressrot", worker_body, w, 3) >= 0) nw++;
    }
#endif
    for (unsigned i = 0; i < W_MIX_MAX; i++) {
        worker_t *w = &g_workers[nw];
        memset(w, 0, sizeof *w);
        w->index = (uint8_t)i;
        if (task_create_sized("stress", worker_body, w, 3) < 0) break;   /* the task table is full */
        nw++;
        mixed++;
    }
    cprintf("stress: %u mixed workers%s for %lu s%s\n", mixed,
            nw > mixed ? " and the rotation" : "", (unsigned long)seconds,
            g_have_sd ? "" : " (no /sd0: no card path)");
    uint64_t hard_end = g_until_ms + 30000u;
    for (;;) {
        bool all = true;
        for (unsigned i = 0; i < nw; i++) all = all && g_workers[i].done;
        if (all || time_get_ms() > hard_end) break;
        task_sleep_ms(200);
    }
    snapshot(&s1);

    uint32_t errors = 0, stuck = 0;
    for (unsigned i = 0; i < nw; i++)
        if (!g_workers[i].done) stuck++;
    for (unsigned p = 0; p < P_COUNT; p++) {
        path_stats_t *ps = &g_path[p];
        if (!ps->iters && !ps->errors) continue;
        errors += ps->errors;
        cprintf("  %-5s %9lu iterations, %lu errors%s%s%s\n", P_NAME[p],
                (unsigned long)ps->iters, (unsigned long)ps->errors,
                ps->errors ? " (first: " : "", ps->errors ? ps->first_error : "", ps->errors ? ")" : "");
    }
    uint32_t full = 0;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        int pid, state;
        const char *name;
        if (sched_task_info(i, &pid, &state, &name) && sched_stack_full(pid)) {
            cprintf("  stack FULL: #%d '%s'\n", pid, name);
            full++;
        }
    }
    uint32_t lf = s1.lock_faults - s0.lock_faults, inv = s1.inversions - s0.inversions;
    uint32_t i2c = (s1.i2c.no_turn - s0.i2c.no_turn) + (s1.i2c.timeout - s0.i2c.timeout) +
                   (s1.i2c.arb_lost - s0.i2c.arb_lost) + (s1.i2c.incomplete - s0.i2c.incomplete);
    cprintf("  lock faults %lu, order inversions %lu, I2C failures %lu (no turn %lu, timeout %lu, "
            "arbitration %lu, incomplete %lu), klog gaps %lu, stacks full %lu, workers stuck %lu\n",
            (unsigned long)lf, (unsigned long)inv, (unsigned long)i2c,
            (unsigned long)(s1.i2c.no_turn - s0.i2c.no_turn), (unsigned long)(s1.i2c.timeout - s0.i2c.timeout),
            (unsigned long)(s1.i2c.arb_lost - s0.i2c.arb_lost),
            (unsigned long)(s1.i2c.incomplete - s0.i2c.incomplete),
            (unsigned long)(s1.klog_gaps - s0.klog_gaps), (unsigned long)full, (unsigned long)stuck);
    uint32_t vnone = s1.vfs.none - s0.vfs.none;
    cprintf("  VFS handles: at most %lu of %u open, %lu opens waited for one, %lu found none\n",
            (unsigned long)s1.vfs.peak, (unsigned)VFS_MAX_HANDLES,
            (unsigned long)(s1.vfs.waits - s0.vfs.waits), (unsigned long)vnone);
    {
        uint32_t deep = 0, size = 0;
        for (unsigned i = 0; i < nw; i++)
            if (g_workers[i].stack_depth > deep) { deep = g_workers[i].stack_depth; size = g_workers[i].stack_size; }
        cprintf("  worker stacks: deepest %lu of %lu B\n", (unsigned long)deep, (unsigned long)size);
    }
    cprintf("  mount references now: sd0 %lu, ram0 %lu\n", (unsigned long)vfs_mount_users("sd0"),
            (unsigned long)vfs_mount_users("ram0"));
    bool ok = mixed >= W_MIX_MIN && errors == 0 && lf == 0 && inv == 0 && i2c == 0 &&
              full == 0 && stuck == 0 && vnone == 0;
    if (mixed < W_MIX_MIN) cprintf("  only %u workers: the task table is full\n", mixed);
    cprintf(ok ? "STRESS_OK\n" : "STRESS_FAIL\n");
#if defined(CONFIG_BOARD_ESP32P4)
    if (g_cache_psram) { palloc_free(g_cache_psram, CACHE_EXT_PAGES); g_cache_psram = NULL; }
#endif
    session_release(&g_stress_session);
    return ok ? 0 : -1;
}

/* accepted: written by `stress bg` before the task starts, read by it. */
static uint32_t g_bg_seconds;

static void stress_bg_body(void *arg) {
    (void)arg;
    (void)stress_run(g_bg_seconds);
}

void stress_start_bg(uint32_t seconds) {
    g_bg_seconds = seconds;
    if (task_create_sized("stressrun", stress_bg_body, NULL, 2) < 0) cprintf("stress: no task\n");
}
