/* `kobjutest`: every kernel-object syscall, from a real U-mode task confined
 * to a real PMP/Sv39 domain (45.3b, plan/phase45_esp32c6.md).
 *
 * kobjselftest checks the objects from kernel tasks; what this adds is the
 * boundary. The task below runs in U-mode in a domain of exactly two regions --
 * a page holding its stack and the shared result block, and its own code page
 * -- so every service it uses has to come through an ecall, every pointer it
 * passes is validated against that domain, and every handle is checked against
 * the domain that made it.
 *
 * The checks that a boundary exists:
 *   - a handle created by the kernel (owner 0) is refused to this domain, as
 *     is a handle that was never issued;
 *   - a pointer into kernel memory is refused with KO_FAIL, not dereferenced
 *     and not faulted on;
 *   - the task blocks in the kernel and is woken by a kernel task that was
 *     never in its domain.
 *
 * The U-mode half obeys the rules every UATTR function here does
 * (kernel/umode_probe.c): its code is in a section the domain grants (its own
 * page, .utext_kobj), the translation unit is built -fno-jump-tables, there are
 * no string literals, and it calls nothing outside that page -- no libc, so no
 * struct copies the compiler might turn into a memcpy call. */

#include "kernel/kobj_sys.h"
#include "kernel/kobj_sched.h"
#include "kernel/sched.h"
#include "kernel/palloc.h"
#include "kernel/mem_domain.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/ipc.h"
#include "arch/umode.h"
#include "lugalos_config.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define KU __attribute__((section(".utext_kobj"), noinline, no_sanitize("undefined")))
extern char _utext_kobj_start[];

#define NCHECK 29

/* The shared block at the bottom of the task's page. The U half writes it, the
 * kernel half reads it after the task is dead. */
typedef struct {
    volatile uint32_t result[NCHECK];   /* 0 = not reached, 1 = pass, 2 = fail */
    volatile int32_t  detail[NCHECK];   /* what was observed */
    volatile uintptr_t kernel_sem;      /* a handle the kernel made: not this domain's */
    volatile uintptr_t kernel_ptr;      /* an address outside the domain */
    volatile uint32_t  handoff;         /* the semaphore U is about to block on */
    volatile uint32_t  finished;
    volatile uint32_t  thread_sem_a, thread_sem_b;   /* main -> thread, thread -> main */
    volatile uint32_t  thread_pid, thread_ran;
} kctx_t;

#define UCHECK(i, cond, obs) do { ctx->detail[i] = (int32_t)(obs); ctx->result[i] = (cond) ? 1u : 2u; } while (0)

static inline __attribute__((always_inline))
long ksys(long nr, long a1, long a2, long a3, long a4, long a5) {
    register long r0 __asm__("a0") = nr;
    register long r1 __asm__("a1") = a1;
    register long r2 __asm__("a2") = a2;
    register long r3 __asm__("a3") = a3;
    register long r4 __asm__("a4") = a4;
    register long r5 __asm__("a5") = a5;
    __asm__ volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5) : "memory");
    return r0;
}

#define KS(op, a1, a2, a3, a4, a5) ksys(SYS_KOBJ(op), (long)(a1), (long)(a2), (long)(a3), (long)(a4), (long)(a5))
#define SYS_UEXIT_NR  20
#define SYS_TIME_MS_NR 23

/* A second thread in the same domain: waits for the main thread, runs, and
 * answers. Entered as `void entry(uintptr_t arg)` by kthread_body(). */
KU static void kobj_utest_thread(uintptr_t arg) {
    kctx_t *ctx = (kctx_t *)arg;
    ctx->thread_pid = (uint32_t)KS(KOBJ_OP_THREAD_SELF, 0, 0, 0, 0, 0);
    if (KS(KOBJ_OP_SEM_TAKE, ctx->thread_sem_a, 1000, 0, 0, 0) == KO_OK) {
        ctx->thread_ran = 1;
        KS(KOBJ_OP_SEM_GIVE, ctx->thread_sem_b, 0, 0, 0, 0);
    }
    ksys(SYS_UEXIT_NR, 0, 0, 0, 0, 0);
    for (;;) { }
}

KU static void kobj_utest_body(uintptr_t arg) {
    kctx_t *ctx = (kctx_t *)arg;
    long r;

    /* 0-4: semaphore. Two gives, two takes, a poll, a timed wait. */
    long s = KS(KOBJ_OP_SEM_CREATE, 2, 0, 0, 0, 0);
    UCHECK(0, s > 0, s);
    KS(KOBJ_OP_SEM_GIVE, s, 0, 0, 0, 0);
    KS(KOBJ_OP_SEM_GIVE, s, 0, 0, 0, 0);
    r = KS(KOBJ_OP_SEM_GIVE, s, 0, 0, 0, 0);
    UCHECK(1, r == KO_FULL, r);                       /* the third give: at capacity */
    r = KS(KOBJ_OP_SEM_TAKE, s, 0, 0, 0, 0) + KS(KOBJ_OP_SEM_TAKE, s, 0, 0, 0, 0);
    UCHECK(2, r == KO_OK, r);
    r = KS(KOBJ_OP_SEM_TAKE, s, 0, 0, 0, 0);
    UCHECK(3, r == KO_AGAIN, r);
    long t0 = ksys(SYS_TIME_MS_NR, 0, 0, 0, 0, 0);
    r = KS(KOBJ_OP_SEM_TAKE, s, 25, 0, 0, 0);
    long dt = ksys(SYS_TIME_MS_NR, 0, 0, 0, 0, 0) - t0;
    UCHECK(4, r == KO_TIMEOUT && dt >= 25 && dt < 250, dt);

    /* 5-6: the boundary. A handle the kernel owns, and one nobody issued. */
    r = KS(KOBJ_OP_SEM_TAKE, ctx->kernel_sem, 0, 0, 0, 0);
    UCHECK(5, r == KO_FAIL, r);
    r = KS(KOBJ_OP_SEM_GIVE, 0x12345678, 0, 0, 0, 0);
    UCHECK(6, r == KO_FAIL, r);

    /* 7-11: a queue of 8-byte items, in order, then the boundary again. */
    long q = KS(KOBJ_OP_Q_CREATE, 4, 8, 0, 0, 0);
    UCHECK(7, q > 0, q);
    volatile uint32_t item[2], got[2];
    long ok = 1;
    for (uint32_t i = 1; i <= 3; i++) {
        item[0] = i; item[1] = i * 100;
        if (KS(KOBJ_OP_Q_SEND, q, item, 0, 0, 0) != KO_OK) ok = 0;
    }
    UCHECK(8, ok && KS(KOBJ_OP_Q_WAITING, q, 0, 0, 0, 0) == 3, ok);
    ok = 1;
    for (uint32_t i = 1; i <= 3; i++) {
        got[0] = got[1] = 0;
        if (KS(KOBJ_OP_Q_RECV, q, got, 0, 0, 0) != KO_OK || got[0] != i || got[1] != i * 100) ok = 0;
    }
    UCHECK(9, ok, ok);
    r = KS(KOBJ_OP_Q_RECV, q, got, 20, 0, 0);
    UCHECK(10, r == KO_TIMEOUT, r);
    r = KS(KOBJ_OP_Q_SEND, q, ctx->kernel_ptr, 0, 0, 0);          /* a kernel address as the item */
    UCHECK(11, r == KO_FAIL, r);

    /* 12-13: a recursive mutex. */
    long m = KS(KOBJ_OP_MUTEX_CREATE, 1, 0, 0, 0, 0);
    r = KS(KOBJ_OP_MUTEX_LOCK, m, 0, 0, 0, 0) + KS(KOBJ_OP_MUTEX_LOCK, m, 0, 0, 0, 0);
    UCHECK(12, m > 0 && r == KO_OK, r);
    r = KS(KOBJ_OP_MUTEX_UNLOCK, m, 0, 0, 0, 0) + KS(KOBJ_OP_MUTEX_UNLOCK, m, 0, 0, 0, 0);
    r += KS(KOBJ_OP_MUTEX_UNLOCK, m, 0, 0, 0, 0);                  /* a third: not held, KO_FAIL */
    UCHECK(13, r == KO_FAIL, r);

    /* 14-16: an event group: set, a satisfied wait that clears, an unsatisfied one. */
    long e = KS(KOBJ_OP_EV_CREATE, 0, 0, 0, 0, 0);
    long after = KS(KOBJ_OP_EV_SET, e, 0x5, 0, 0, 0);
    UCHECK(14, e > 0 && after == 0x5, after);
    volatile uint32_t bits = 0;
    r = KS(KOBJ_OP_EV_WAIT, e, 0x5, KOBJ_EV_ALL | KOBJ_EV_CLEAR, 0, &bits);
    UCHECK(15, r == KO_OK && bits == 0x5 && KS(KOBJ_OP_EV_CLEAR, e, 0xff, 0, 0, 0) == 0, bits);
    r = KS(KOBJ_OP_EV_WAIT, e, 0x2, 0, 15, &bits);
    UCHECK(16, r == KO_TIMEOUT, r);

    /* 17-19: timers. A one-shot, a periodic one, a disarm. */
    volatile uintptr_t rec[3];
    KS(KOBJ_OP_TIMER_SETFN, 0x7000, 0xf00d, 0xa11c, 0, 0);
    KS(KOBJ_OP_TIMER_ARM, 0x7000, 0, 0, 12000, 0);                 /* keeps the callback setfn gave it */
    r = KS(KOBJ_OP_TIMER_WAIT, rec, 300, 0, 0, 0);
    UCHECK(17, r == KO_OK && rec[0] == 0x7000 && rec[1] == 0xf00d && rec[2] == 0xa11c, r);
    KS(KOBJ_OP_TIMER_ARM, 0x7001, 0xbeef, 0x1, 8000, 1);
    long n = 0;
    for (int i = 0; i < 3; i++) if (KS(KOBJ_OP_TIMER_WAIT, rec, 200, 0, 0, 0) == KO_OK && rec[0] == 0x7001) n++;
    UCHECK(18, n == 3, n);
    KS(KOBJ_OP_TIMER_DISARM, 0x7001, 0, 0, 0, 0);
    r = KS(KOBJ_OP_TIMER_WAIT, rec, 40, 0, 0, 0);
    UCHECK(19, r == KO_TIMEOUT, r);

    /* 20: the clock, through a pointer in the domain; and the same call with a
     * pointer outside it. */
    volatile uint64_t us = 0;
    r = KS(KOBJ_OP_TIME_US, &us, 0, 0, 0, 0);
    UCHECK(20, r == KO_OK && us != 0, r);
    r = KS(KOBJ_OP_TIME_US, ctx->kernel_ptr, 0, 0, 0, 0);
    UCHECK(21, r == KO_FAIL, r);

    /* 22: block in the kernel until a task outside the domain wakes us. */
    long h = KS(KOBJ_OP_SEM_CREATE, 1, 0, 0, 0, 0);
    ctx->handoff = (uint32_t)h;
    t0 = ksys(SYS_TIME_MS_NR, 0, 0, 0, 0, 0);
    r = KS(KOBJ_OP_SEM_TAKE, h, 0xffffffffu, 0, 0, 0);
    dt = ksys(SYS_TIME_MS_NR, 0, 0, 0, 0, 0) - t0;
    UCHECK(22, r == KO_OK && dt >= 20, dt);

    /* 23: the critical section, entered twice and left twice. */
    r = KS(KOBJ_OP_CRIT_ENTER, 0, 0, 0, 0, 0) + KS(KOBJ_OP_CRIT_ENTER, 0, 0, 0, 0, 0);
    r += KS(KOBJ_OP_CRIT_LEAVE, 0, 0, 0, 0, 0) + KS(KOBJ_OP_CRIT_LEAVE, 0, 0, 0, 0, 0);
    UCHECK(23, r == KO_OK, r);

    /* 24-25: the kernel's random bytes and the node's MAC, through pointers in the domain. */
    volatile uint8_t rb[16];
    for (int i = 0; i < 16; i++) rb[i] = 0;
    r = KS(KOBJ_OP_RANDOM, rb, 16, 0, 0, 0);
    long nz = 0;
    for (int i = 0; i < 16; i++) nz |= rb[i];
    UCHECK(24, r == 16 && nz != 0, r);
    volatile uint8_t ma[6], mb[6];
    long m1 = KS(KOBJ_OP_MAC, 0, ma, 0, 0, 0), m2 = KS(KOBJ_OP_MAC, 1, mb, 0, 0, 0);
    UCHECK(25, m1 == KO_OK && m2 == KO_OK && ma[0] == mb[0] && (uint8_t)(ma[5] + 1) == mb[5], ma[5]);

    /* 26: a log line, rendered by the kernel. The text is built on the stack:
     * a literal would land in .rodata, outside the domain. */
    volatile char text[8];
    text[0] = 'u'; text[1] = '-'; text[2] = 'm'; text[3] = 'o'; text[4] = 'd'; text[5] = 'e'; text[6] = 0;
    r = KS(KOBJ_OP_LOG, 1, text, 6, 0, 0);
    UCHECK(26, r == KO_OK, r);

    /* 27: a second thread in this domain, woken and answering. */
    long sa = KS(KOBJ_OP_SEM_CREATE, 1, 0, 0, 0, 0), sb = KS(KOBJ_OP_SEM_CREATE, 1, 0, 0, 0, 0);
    ctx->thread_sem_a = (uint32_t)sa; ctx->thread_sem_b = (uint32_t)sb;
    uintptr_t tstack = (uintptr_t)ctx + 1024;
    long tpid = KS(KOBJ_OP_THREAD_CREATE, kobj_utest_thread, ctx, tstack, 1024, 23);
    KS(KOBJ_OP_SEM_GIVE, sa, 0, 0, 0, 0);
    r = KS(KOBJ_OP_SEM_TAKE, sb, 1000, 0, 0, 0);
    long self = KS(KOBJ_OP_THREAD_SELF, 0, 0, 0, 0, 0);
    UCHECK(27, tpid > 0 && r == KO_OK && ctx->thread_ran == 1 && ctx->thread_pid == (uint32_t)tpid && tpid != self, tpid);

    /* 28: a thread may not be given memory it does not own, nor code it may not run. */
    long b1 = KS(KOBJ_OP_THREAD_CREATE, kobj_utest_thread, ctx, ctx->kernel_ptr & ~7u, 1024, 23);     /* a kernel stack */
    long b2 = KS(KOBJ_OP_THREAD_CREATE, ctx->kernel_ptr, ctx, tstack, 1024, 23);                      /* entry in kernel data */
    UCHECK(28, b1 == KO_FAIL && b2 == KO_FAIL, b1 * 10 + b2);

    ctx->finished = 1;
    ksys(SYS_UEXIT_NR, 0, 0, 0, 0, 0);
    for (;;) { }
}

/* ---- kernel half --------------------------------------------------------- */

static mem_domain_t g_kutest_domain;
static kctx_t      *g_kctx;
static void        *g_kpage;
static volatile int g_kuser_ready;

static void kutest_peer(void *arg) {
    (void)arg;
    /* Wait for the U task to publish the semaphore it is about to block on,
     * give it time to actually block, then wake it from outside its domain. */
    for (int i = 0; i < 2000 && g_kctx->handoff == 0; i++) task_sleep_ms(1);
    task_sleep_ms(40);
    if (g_kctx->handoff) kos_sem_give((kh_t)g_kctx->handoff);
}

static void kutest_task(void *arg) {
    (void)arg;
    mem_domain_destroy(&g_kutest_domain);
    mem_domain_init(&g_kutest_domain);

    /* One page: the shared block at the bottom, the stack growing down from the
     * top. It is a single region, so it is a single power-of-two grant on PMP
     * and a single page on Sv39. */
    mem_domain_add(&g_kutest_domain, (uintptr_t)g_kpage, 4096, MEM_R | MEM_W);
    mem_domain_add(&g_kutest_domain, (uintptr_t)_utext_kobj_start, 16384, MEM_R | MEM_X);

    if (task_set_domain(sched_current_pid(), &g_kutest_domain) != 0) {
        printk("[kobjutest] Refusing to enter U-mode: the domain is not enforceable\n");
        return;
    }
    arch_enter_user((void (*)(void))kobj_utest_body, (uintptr_t)g_kpage + 4096, 0, (uintptr_t)g_kctx, 0);
}

int kobj_utest(void) {
    kos_init();
    if (!mem_domain_enforced()) {
        cprintf("kobjutest: no enforced memory domains on this build -- nothing to test.\n");
        return 0;
    }
    g_kpage = palloc_pages_aligned(1, 1);
    if (!g_kpage) { cprintf("KOBJUTEST_FAIL (no page)\n"); return 1; }
    g_kctx = (kctx_t *)g_kpage;
    g_kctx->kernel_sem = kos_sem_create(1, 1, 0);                  /* owner 0: not the task's */
    g_kctx->kernel_ptr = (uintptr_t)&g_kuser_ready;                /* kernel .bss: outside the domain */

    int peer = task_create("kobjpeer", kutest_peer, NULL);
    int pid = task_create("kobjutest", kutest_task, NULL);
    if (peer < 0 || pid < 0) { cprintf("KOBJUTEST_FAIL (no task)\n"); return 1; }
    for (int i = 0; i < 4000 && sched_task_state(pid) != TASK_DEAD; i++) task_sleep_ms(1);

    static const char *const names[NCHECK] = {
        "sem create", "sem give at capacity", "sem take x2", "sem poll empty", "sem timed take",
        "kernel's handle refused", "forged handle refused",
        "queue create", "queue send x3", "queue FIFO recv", "queue timed recv", "kernel pointer as item refused",
        "recursive mutex lock x2", "mutex unlock x3 (last fails)",
        "event set", "event wait all+clear", "event timed wait",
        "timer one-shot with setfn callback", "periodic timer x3", "timer disarm", "time_us into own buffer",
        "kernel pointer for time_us refused", "block in the kernel, woken from outside", "critical section x2",
        "kernel random bytes", "node MAC by type", "a log line", "a second thread in the domain",
        "threads refuse foreign stack/entry",
    };
    int fails = 0;
    for (int i = 0; i < NCHECK; i++) {
        uint32_t r = g_kctx->result[i];
        if (r != 1) fails++;
        cprintf("  %s %-40s (%d)\n", r == 1 ? "PASS" : (r == 0 ? "----" : "FAIL"), names[i], (int)g_kctx->detail[i]);
    }
    bool dead = sched_task_state(pid) == TASK_DEAD;
    long status = -1;
    bool clean = sched_task_exited_cleanly(pid, &status);
    if (!dead || !clean || !g_kctx->finished) { fails++; cprintf("  FAIL the task did not finish cleanly (dead=%d clean=%d)\n", dead, clean); }
    if (fails == 0) cprintf("KOBJUTEST_OK\n"); else cprintf("KOBJUTEST_FAIL (%d)\n", fails);
    /* Give the page back once nothing can touch it any more -- the test task is gone; a task that
     * did not end keeps it, as a leak is the lesser harm than a page freed under a running thread.
     * Every run used to keep it (45.8: 8 KB a run on the C6, where the radio leaves ~50 KB free). */
    if (dead) {
        kos_sem_delete(g_kctx->kernel_sem);
        kobj_sys_release_domain((uintptr_t)&g_kutest_domain);   /* what its U-mode code created and never deleted */
        palloc_free(g_kpage, 1); g_kpage = NULL; g_kctx = NULL;
    }
    return fails;
}
