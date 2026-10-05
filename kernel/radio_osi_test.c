/* `radioosi`: the radio shim's OS-table entries, called as the blob will call
 * them, from threads of a real U-mode domain (45.3b,
 * plan/phase45_esp32c6.md §4.5).
 *
 * The shim (drivers/radio/osi_impl.c and the heap, formatter and key/value store
 * under it) is linked into the domain's own text by the linker script, and its
 * state lives in the domain's own data region. This test builds that domain --
 * three regions: a page for the test's own context, the 16 KB of radio text, the
 * 32 KB of radio data and heap -- and runs a thread in it that goes through the
 * entries: memory, every synchronisation object, a timer thread that calls back
 * into the domain, a blob-style task, the key/value store, a formatted log line.
 *
 * What it proves that kobjutest does not: the shim *as a whole* works under the
 * confinement it is built for -- no libc, no kernel function called, every
 * service reached by ecall -- and that function pointers into it work, which is
 * how the blob reaches it.
 *
 * Same rules as kobj_utest.c for the U-mode half: its code is in the granted
 * section, nothing it calls is outside it, and a string it reads lives in the
 * granted text region (a literal in ordinary .rodata would fault). */

#include "kernel/radio_osi_test.h"
#include "kernel/kobj_abi.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/mem_domain.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/ipc.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include "../drivers/radio/osi_impl.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KU __attribute__((section(".utext_kobj"), noinline, no_sanitize("undefined")))
#define KSTR __attribute__((section(".utext_kobj_ro"))) static const

extern char _utext_kobj_start[], _udata_kobj_start[];

#define NCHECK 22
#define ARENA_BYTES 24576u

/* The arena, in the radio data region (and 8-byte aligned, which the heap needs). */
static uint8_t g_arena[ARENA_BYTES] __attribute__((section(".udata_kobj"), aligned(8)));

typedef struct {
    volatile uint32_t result[NCHECK];
    volatile int32_t  detail[NCHECK];
    volatile uint32_t timer_hits, timer_thread_pid, periodic_hits;
    volatile uint32_t task_ran, task_handle, task_param;
    volatile uint32_t thread_sem_a, thread_sem_b;
    volatile uint32_t isr_in, isr_out, isr_done;
    volatile uint32_t finished;
    uint32_t etstimer[8];            /* stands in for the blob's ETSTimer: only its address matters */
    uint32_t etstimer2[8];
} rctx_t;

#define RCHECK(i, cond, obs) do { ctx->detail[i] = (int32_t)(obs); ctx->result[i] = (cond) ? 1u : 2u; } while (0)

KSTR char k_fmt[]    = "mac %02x:%02x n=%d s=%s w=%5u|";
KSTR char k_str[]    = "ok";
KSTR char k_tag[]    = "radioosi";
KSTR char k_ns[]     = "phy";
KSTR char k_key[]    = "cal_data";
KSTR char k_missing[] = "no_such_key";

/* ---- callbacks the blob would register ---------------------------------- */

KU static void timer_cb(void *arg) {
    rctx_t *ctx = (rctx_t *)arg;
    ctx->timer_hits++;
    ctx->timer_thread_pid = (uint32_t)(uintptr_t)radio_osi_task_get_current_task();
}

KU static void periodic_cb(void *arg) { ((rctx_t *)arg)->periodic_hits++; }

/* A blob-style task: a function and a parameter, run on a stack the shim
 * allocated from the radio heap, which simply returns. */
KU static void blob_task(void *param) {
    rctx_t *ctx = (rctx_t *)param;
    ctx->task_param = 0xB10B;
    ctx->task_ran = 1;
}

/* A thread that registers its own stack as "the interrupt thread's" and asks
 * is_from_isr -- true inside it, false outside. */
KU static void isr_probe_thread(void *param) {
    rctx_t *ctx = (rctx_t *)param;
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    radio_osi_set_isr_stack(sp & ~(uintptr_t)1023u, (sp | 1023u) + 1);
    ctx->isr_in = radio_osi_is_from_isr() ? 1u : 2u;
    radio_osi_set_isr_stack(0, 0);
    ctx->isr_out = radio_osi_is_from_isr() ? 2u : 1u;
    ctx->isr_done = 1;
}

KU static void thread_sem_other(void *param) {
    rctx_t *ctx = (rctx_t *)param;
    ctx->thread_sem_b = (uint32_t)(uintptr_t)radio_osi_wifi_thread_semphr_get();
}

/* ---- the test, in U-mode ------------------------------------------------- */

KU static void radio_utest_body(uintptr_t arg) {
    rctx_t *ctx = (rctx_t *)arg;

    /* 0-3: the heap, through the table's entries. */
    bool up = radio_osi_init(g_arena, ARENA_BYTES);
    RCHECK(0, up, up);
    uint32_t free0 = radio_osi_get_free_heap_size();
    uint8_t *p = (uint8_t *)radio_osi_malloc(100);
    bool in_arena = p >= g_arena && p + 100 <= g_arena + ARENA_BYTES && ((uintptr_t)p & 7) == 0;
    RCHECK(1, p && in_arena && radio_osi_get_free_heap_size() < free0, (uintptr_t)p & 7);
    for (int i = 0; i < 100; i++) p[i] = (uint8_t)(i + 1);
    p = (uint8_t *)radio_osi_realloc(p, 3000);
    long keep = p != 0;
    for (int i = 0; p && i < 100; i++) if (p[i] != (uint8_t)(i + 1)) keep = 0;
    uint8_t *z = (uint8_t *)radio_osi_zalloc(64), *c = (uint8_t *)radio_osi_calloc(8, 8);
    long zero = z && c;
    for (int i = 0; z && c && i < 64; i++) if (z[i] || c[i]) zero = 0;
    RCHECK(2, keep && zero, keep * 10 + zero);
    radio_osi_free(p); radio_osi_free(z); radio_osi_free(c);
    RCHECK(3, radio_osi_get_free_heap_size() == free0, radio_osi_get_free_heap_size());

    /* 4: semaphores: pdTRUE/pdFALSE, a full give, a timed take. */
    void *s = radio_osi_semphr_create(1, 0);
    long r1 = radio_osi_semphr_give(s), r2 = radio_osi_semphr_give(s), r3 = radio_osi_semphr_take(s, 5), r4 = radio_osi_semphr_take(s, 5);
    RCHECK(4, s && r1 == 1 && r2 == 0 && r3 == 1 && r4 == 0, r1 * 1000 + r2 * 100 + r3 * 10 + r4);
    radio_osi_semphr_delete(s);

    /* 5: a plain and a recursive mutex. */
    void *m = radio_osi_mutex_create(), *rm = radio_osi_recursive_mutex_create();
    long ok = radio_osi_mutex_lock(m) == 1 && radio_osi_mutex_unlock(m) == 1
           && radio_osi_mutex_lock(rm) == 1 && radio_osi_mutex_lock(rm) == 1
           && radio_osi_mutex_unlock(rm) == 1 && radio_osi_mutex_unlock(rm) == 1
           && radio_osi_mutex_unlock(rm) == 0;
    RCHECK(5, m && rm && ok, ok);
    radio_osi_mutex_delete(m); radio_osi_mutex_delete(rm);

    /* 6: a queue: back, front, from "ISR", receive, count. */
    void *q = radio_osi_queue_create(4, 4);
    uint32_t a = 1, b = 2, d = 3, got = 0;
    long qok = radio_osi_queue_send(q, &a, 10) == 1 && radio_osi_queue_send_to_front(q, &b, 10) == 1;
    int32_t hptw = 7;
    qok = qok && radio_osi_queue_send_from_isr(q, &d, &hptw) == 1 && hptw == 0 && radio_osi_queue_msg_waiting(q) == 3;
    qok = qok && radio_osi_queue_recv(q, &got, 10) == 1 && got == 2;      /* the front item first */
    qok = qok && radio_osi_queue_recv(q, &got, 10) == 1 && got == 1;
    qok = qok && radio_osi_queue_recv(q, &got, 10) == 1 && got == 3;
    qok = qok && radio_osi_queue_recv(q, &got, 5) == 0;                    /* empty: pdFALSE after the timeout */
    RCHECK(6, q && qok, qok);
    radio_osi_queue_delete(q);

    /* 7: an event group. */
    void *e = radio_osi_event_group_create();
    radio_osi_event_group_set_bits(e, 0x6);
    uint32_t seen = radio_osi_event_group_wait_bits(e, 0x6, 1, 1, 5);
    uint32_t again = radio_osi_event_group_wait_bits(e, 0x6, 1, 1, 5);
    RCHECK(7, e && seen == 0x6 && again == 0, seen);
    radio_osi_event_group_delete(e);

    /* 8: one semaphore per thread: the same one twice here, a different one elsewhere. */
    void *t1 = radio_osi_wifi_thread_semphr_get(), *t2 = radio_osi_wifi_thread_semphr_get();
    radio_thread_create(thread_sem_other, ctx, 1024, 12);
    for (int i = 0; i < 200 && !ctx->thread_sem_b; i++) radio_osi_task_delay(5);
    RCHECK(8, t1 && t1 == t2 && ctx->thread_sem_b && (void *)(uintptr_t)ctx->thread_sem_b != t1, ctx->thread_sem_b);

    /* 9-10: timers. The thread that calls the callbacks is a thread of this domain. */
    int32_t tp = radio_thread_create(radio_timer_thread, 0, 1024, 10);
    uint32_t me = (uint32_t)(uintptr_t)radio_osi_task_get_current_task();
    radio_osi_timer_setfn(ctx->etstimer, (void *)(uintptr_t)timer_cb, ctx);
    radio_osi_timer_arm(ctx->etstimer, 10, false);
    for (int i = 0; i < 100 && !ctx->timer_hits; i++) radio_osi_task_delay(5);
    RCHECK(9, tp > 0 && ctx->timer_hits == 1 && ctx->timer_thread_pid == (uint32_t)tp && ctx->timer_thread_pid != me, ctx->timer_thread_pid);
    radio_osi_timer_setfn(ctx->etstimer2, (void *)(uintptr_t)periodic_cb, ctx);
    radio_osi_timer_arm_us(ctx->etstimer2, 8000, true);
    for (int i = 0; i < 100 && ctx->periodic_hits < 3; i++) radio_osi_task_delay(5);
    radio_osi_timer_disarm(ctx->etstimer2);
    uint32_t n = ctx->periodic_hits;
    radio_osi_task_delay(40);
    RCHECK(10, n >= 3 && ctx->periodic_hits == n, n);
    radio_osi_timer_done(ctx->etstimer); radio_osi_timer_done(ctx->etstimer2);

    /* 11: a blob-style task, with the handle written back. */
    uint32_t handle = 0;
    int32_t tr = radio_osi_task_create_pinned_to_core((void *)(uintptr_t)blob_task, 0, 2048, ctx, 23, &handle, 0);
    for (int i = 0; i < 100 && !ctx->task_ran; i++) radio_osi_task_delay(5);
    RCHECK(11, tr == 1 && ctx->task_ran && ctx->task_param == 0xB10B && handle > 0, handle);

    /* 12: is_from_isr is a property of which thread's stack we are on. */
    bool before = radio_osi_is_from_isr();
    radio_thread_create(isr_probe_thread, ctx, 2048, 12);
    for (int i = 0; i < 200 && !ctx->isr_done; i++) radio_osi_task_delay(5);
    RCHECK(12, !before && ctx->isr_in == 1 && ctx->isr_out == 1, ctx->isr_in * 10 + ctx->isr_out);

    /* 13: the blob's critical section, twice over. */
    uint32_t f1 = radio_osi_wifi_int_disable(0), f2 = radio_osi_wifi_int_disable(0);
    radio_osi_wifi_int_restore(0, f2); radio_osi_wifi_int_restore(0, f1);
    RCHECK(13, true, f1 + f2);

    /* 14-15: the key/value store: a missing key says so; a blob round-trips. */
    uint32_t h = 0;
    int r = radio_osi_nvs_open(k_ns, 1, &h);
    uint8_t cal[48], back[48];
    for (int i = 0; i < 48; i++) cal[i] = (uint8_t)(i * 5);
    size_t len = sizeof back;
    uint8_t byte = 0;
    int miss = radio_osi_nvs_get_u8(h, k_missing, &byte);
    RCHECK(14, r == 0 && h != 0 && miss == 0x1102, miss);
    int w = radio_osi_nvs_set_blob(h, k_key, cal, sizeof cal);
    int g = radio_osi_nvs_get_blob(h, k_key, back, &len);
    long same = len == sizeof cal;
    for (int i = 0; i < 48; i++) if (back[i] != cal[i]) same = 0;
    RCHECK(15, w == 0 && g == 0 && same, g);
    radio_osi_nvs_close(h);

    /* 16: a formatted log line, rendered here and printed by the kernel. */
    radio_osi_log_write(1, k_tag, k_fmt, 0xac, 0x3a, -7, k_str, 99u);
    RCHECK(16, true, 0);

    /* 17-19: time, randomness, the MAC. */
    int64_t t0 = radio_osi_esp_timer_get_time();
    radio_osi_task_delay(20);
    int64_t t1_ = radio_osi_esp_timer_get_time();
    long tv[2] = { -1, -1 };
    radio_osi_get_time(tv);
    RCHECK(17, t1_ - t0 >= 15000 && t1_ - t0 < 400000 && tv[0] >= 0 && tv[1] >= 0, (long)(t1_ - t0));
    uint8_t rb[16]; for (int i = 0; i < 16; i++) rb[i] = 0;
    long nz = 0;
    radio_osi_get_random(rb, 16);
    for (int i = 0; i < 16; i++) nz |= rb[i];
    RCHECK(18, nz != 0 && radio_osi_rand() != radio_osi_rand(), nz);
    uint8_t m0[6], m1[6];
    RCHECK(19, radio_osi_read_mac(m0, 0) == 0 && radio_osi_read_mac(m1, 1) == 0 && m0[0] == m1[0] && (uint8_t)(m0[5] + 1) == m1[5], m0[5]);

    /* 20: the entries that are constants, and an event with nowhere to go yet. */
    RCHECK(20, radio_osi_task_ms_to_tick(37) == 37 && radio_osi_task_get_max_priority() == 25
               && radio_osi_slowclk_cal_get() == 3495253u && radio_osi_env_is_chip()
               && radio_osi_one_i32() == 1 && !radio_osi_false(), 0);
    int32_t ev = radio_osi_event_post(k_tag, 1, 0, 0, 0);
    RCHECK(21, ev == 0, ev);                       /* PD_FALSE: no event sink on this build (45.7) */

    ctx->finished = 1;
    register long r0 __asm__("a0") = SYS_UEXIT;
    __asm__ volatile("ecall" : "+r"(r0) :: "memory");
    for (;;) { }
}

/* ---- kernel half --------------------------------------------------------- */

static mem_domain_t g_rdomain;
static void        *g_rpage;

static void radio_task(void *arg) {
    (void)arg;
    mem_domain_destroy(&g_rdomain);
    mem_domain_init(&g_rdomain);
    /* The three regions of the radio's domain. The page holds the test context and
     * the main thread's stack (top 2 KB); the text and data regions are the
     * linker's, one power-of-two block each. */
    mem_domain_add(&g_rdomain, (uintptr_t)g_rpage, 4096, MEM_R | MEM_W);
    mem_domain_add(&g_rdomain, (uintptr_t)_utext_kobj_start, 16384, MEM_R | MEM_X);
    mem_domain_add(&g_rdomain, (uintptr_t)_udata_kobj_start, 32768, MEM_R | MEM_W);
    if (task_set_domain(sched_current_pid(), &g_rdomain) != 0) {
        printk("[radioosi] Refusing to enter U-mode: the domain is not enforceable\n");
        return;
    }
    arch_enter_user((void (*)(void))radio_utest_body, (uintptr_t)g_rpage + 4096, 0, (uintptr_t)g_rpage, 0);
}

int radio_osi_test(void) {
    if (!mem_domain_enforced()) {
        cprintf("radioosi: no enforced memory domains on this build -- nothing to test.\n");
        return 0;
    }
    g_rpage = palloc_pages_aligned(1, 1);
    if (!g_rpage) { cprintf("RADIOOSI_FAIL (no page)\n"); return 1; }
    int pid = task_create("radioosi", radio_task, NULL);
    if (pid < 0) { cprintf("RADIOOSI_FAIL (no task)\n"); return 1; }
    for (int i = 0; i < 6000 && sched_task_state(pid) != TASK_DEAD; i++) task_sleep_ms(1);

    static const char *const names[NCHECK] = {
        "radio_osi_init", "malloc inside the arena, 8-aligned", "realloc keeps data; zalloc/calloc zero",
        "free returns every byte", "semaphore: pdTRUE/pdFALSE, timed take", "mutex and recursive mutex",
        "queue: back, front, from-ISR, empty timeout", "event group wait-all + clear", "one semaphore per thread",
        "timer callback runs in the timer thread", "periodic timer, then disarm", "blob-style task, handle written back",
        "is_from_isr follows the thread's stack", "critical section x2", "nvs: a missing key is NOT_FOUND",
        "nvs: blob round trip", "formatted log line", "monotonic time and os_time", "random bytes, rand",
        "read_mac by type", "constants", "event_post has no sink yet (pdFALSE)",
    };
    rctx_t *ctx = (rctx_t *)g_rpage;
    int fails = 0;
    for (int i = 0; i < NCHECK; i++) {
        uint32_t r = ctx->result[i];
        if (r != 1) fails++;
        cprintf("  %s %-42s (%d)\n", r == 1 ? "PASS" : (r == 0 ? "----" : "FAIL"), names[i], (int)ctx->detail[i]);
    }
    long status = -1;
    bool clean = sched_task_exited_cleanly(pid, &status);
    if (sched_task_state(pid) != TASK_DEAD || !clean || !ctx->finished) {
        fails++;
        cprintf("  FAIL the main thread did not finish cleanly (clean=%d)\n", clean);
    }
    if (fails == 0) cprintf("RADIOOSI_OK\n"); else cprintf("RADIOOSI_FAIL (%d)\n", fails);
    return fails;
}
