/* The blocking face of kernel/kobj.c. See kernel/include/kernel/kobj_sched.h.
 * 45.3b, plan/phase45_esp32c6.md. */

#include "kernel/kobj_sched.h"
#include "kernel/sched.h"
#include "kernel/lock.h"
#include "kernel/time.h"
#include "kernel/palloc.h"
#include "kernel/printk.h"
#include <stddef.h>

/* One leaf spinlock around the whole object core. It is held only for the
 * handful of list operations an operation does, never across a block, never
 * while calling the scheduler -- the rule spinlock_t exists to enforce
 * (kernel/lock.h). */
static spinlock_t g_kobj_lock;
static bool g_inited;

void kos_init(void) {
    if (g_inited) return;
    kobj_init();
    spinlock_init(&g_kobj_lock);
    g_inited = true;
}

static void wake_all(const kwake_t *wk) {
    for (int i = 0; i < wk->n; i++) task_unblock(wk->who[i]);
}

/* Waits until `w` leaves KW_WAIT or the deadline passes. Does not cancel: the
 * caller finishes with the object's own cancel(), which also settles a grant
 * that raced the timeout. */
static void wait_for(const kwaiter_t *w, uint32_t timeout_ms) {
    uint64_t deadline = timeout_ms == KOS_FOREVER ? 0 : time_get_ms() + timeout_ms + 1;
    for (;;) {
        task_prepare_block();
        uintptr_t f = spin_lock_irqsave(&g_kobj_lock);
        bool done = w->state != KW_WAIT;
        spin_unlock_irqrestore(&g_kobj_lock, f);
        if (done) break;
        if (deadline && time_get_ms() >= deadline) break;
        task_block_until_ms(deadline);
    }
    task_wait_done();
}

#define LOCKED(stmt) do { uintptr_t _f = spin_lock_irqsave(&g_kobj_lock); stmt; \
                          spin_unlock_irqrestore(&g_kobj_lock, _f); } while (0)

/* ---- semaphores ---------------------------------------------------------- */

kh_t kos_sem_create(uint32_t max, uint32_t init, uintptr_t owner) {
    kh_t h;
    LOCKED(h = ksem_create(max, init); if (h) kobj_set_owner(h, owner));
    return h;
}

int kos_sem_take(kh_t h, uint32_t timeout_ms) {
    kwaiter_t w = {0};
    int r;
    LOCKED(r = ksem_take(h, timeout_ms ? &w : NULL, sched_current_pid()));
    if (r != KO_BLOCK) return r;
    wait_for(&w, timeout_ms);
    LOCKED(r = ksem_cancel(h, &w));
    return r;
}

int kos_sem_give(kh_t h) {
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = ksem_give(h, &wk));
    wake_all(&wk);
    return r;
}

int kos_sem_delete(kh_t h) {
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = ksem_delete(h, &wk));
    wake_all(&wk);
    return r;
}

/* ---- mutexes ------------------------------------------------------------- */

kh_t kos_mutex_create(bool recursive, uintptr_t owner) {
    kh_t h;
    LOCKED(h = kmutex_create(recursive); if (h) kobj_set_owner(h, owner));
    return h;
}

int kos_mutex_lock(kh_t h, uint32_t timeout_ms) {
    kwaiter_t w = {0};
    int r;
    LOCKED(r = kmutex_lock(h, timeout_ms ? &w : NULL, sched_current_pid()));
    if (r != KO_BLOCK) return r;
    wait_for(&w, timeout_ms);
    LOCKED(r = kmutex_cancel(h, &w));
    return r;
}

int kos_mutex_unlock(kh_t h) {
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = kmutex_unlock(h, sched_current_pid(), &wk));
    wake_all(&wk);
    return r;
}

int kos_mutex_delete(kh_t h) {
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = kmutex_delete(h, &wk));
    wake_all(&wk);
    return r;
}

/* ---- queues -------------------------------------------------------------- */

/* A queue's storage is whole pages (palloc is page-granular; the largest queue
 * is 16 KB), allocated here, *before* the lock is taken: an allocator reached
 * from under a spinlock is exactly what the kernel's lock checker faults on. */
kh_t kos_q_create(uint32_t len, uint32_t item_size, uintptr_t owner) {
    if (len == 0 || len > KQUEUE_MAX_LEN || item_size == 0 || item_size > KQUEUE_MAX_ITEM) return 0;
    uint32_t pages = (len * item_size + 4095u) / 4096u;
    void *mem = palloc_pages(pages);
    if (!mem) return 0;
    kh_t h;
    LOCKED(h = kq_create(len, item_size, mem); if (h) kobj_set_owner(h, owner));
    if (!h) palloc_free(mem, pages);
    return h;
}

int kos_q_send(kh_t h, const void *item, bool front, uint32_t timeout_ms) {
    kwaiter_t w = {0};
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = kq_send(h, item, front, timeout_ms ? &w : NULL, sched_current_pid(), &wk));
    wake_all(&wk);
    if (r != KO_BLOCK) return r;
    wait_for(&w, timeout_ms);
    LOCKED(r = kq_cancel(h, &w));
    return r;
}

int kos_q_recv(kh_t h, void *item, uint32_t timeout_ms) {
    kwaiter_t w = {0};
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = kq_recv(h, item, timeout_ms ? &w : NULL, sched_current_pid(), &wk));
    wake_all(&wk);
    if (r != KO_BLOCK) return r;
    wait_for(&w, timeout_ms);
    LOCKED(r = kq_cancel(h, &w));
    return r;
}

int kos_q_delete(kh_t h) {
    kwake_t wk; kwake_init(&wk);
    void *mem = NULL; uint32_t bytes = 0;
    int r;
    LOCKED(r = kq_delete(h, &wk, &mem, &bytes));
    wake_all(&wk);
    if (r == KO_OK && mem) palloc_free(mem, (bytes + 4095u) / 4096u);
    return r;
}

/* ---- event groups -------------------------------------------------------- */

kh_t kos_ev_create(uintptr_t owner) {
    kh_t h;
    LOCKED(h = kev_create(); if (h) kobj_set_owner(h, owner));
    return h;
}

int kos_ev_set(kh_t h, uint32_t bits, uint32_t *after) {
    kwake_t wk; kwake_init(&wk);
    uint32_t v = 0;
    int r = KO_FAIL;
    LOCKED(if (kobj_valid(h)) { v = kev_set(h, bits, &wk); r = KO_OK; });
    wake_all(&wk);
    if (after) *after = v;
    return r;
}

int kos_ev_clear(kh_t h, uint32_t bits, uint32_t *before) {
    uint32_t v = 0;
    int r = KO_FAIL;
    LOCKED(if (kobj_valid(h)) { v = kev_clear(h, bits); r = KO_OK; });
    if (before) *before = v;
    return r;
}

int kos_ev_wait(kh_t h, uint32_t want, bool all, bool clear, uint32_t timeout_ms, uint32_t *out) {
    kwaiter_t w = {0};
    uint32_t seen = 0;
    int r;
    LOCKED(r = kev_wait(h, want, all, clear, &seen, timeout_ms ? &w : NULL, sched_current_pid()));
    if (r == KO_BLOCK) {
        wait_for(&w, timeout_ms);
        LOCKED(r = kev_cancel(h, &w, &seen));
    }
    if (out) *out = seen;
    return r;
}

int kos_ev_delete(kh_t h) {
    kwake_t wk; kwake_init(&wk);
    int r;
    LOCKED(r = kev_delete(h, &wk));
    wake_all(&wk);
    return r;
}

/* ---- timers -------------------------------------------------------------- */

/* The pid of the thread blocked in kos_timer_wait(), so that arming a timer
 * can wake it to recompute its deadline. One radio, one timer thread. */
static volatile int g_timer_waiter = -1;

static void timer_poke(void) {
    int pid = g_timer_waiter;
    if (pid >= 0) task_unblock(pid);
}

int kos_timer_setfn(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg) {
    int r;
    LOCKED(r = ktimer_setfn(owner, key, fn, arg));
    return r;
}

int kos_timer_arm(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg,
                  uint64_t delay_us, bool periodic) {
    int r;
    uint64_t now = time_get_us();
    LOCKED(r = ktimer_arm(owner, key, fn, arg, now, delay_us, periodic));
    timer_poke();
    return r;
}

int kos_timer_disarm(uintptr_t owner, uintptr_t key) {
    int r;
    LOCKED(r = ktimer_disarm(owner, key));
    timer_poke();
    return r;
}

int kos_timer_done(uintptr_t owner, uintptr_t key) {
    int r;
    LOCKED(r = ktimer_done(owner, key));
    timer_poke();
    return r;
}

int kos_timer_wait(uintptr_t owner, uintptr_t *key, uintptr_t *fn, uintptr_t *arg,
                   uint32_t timeout_ms) {
    uint64_t limit = timeout_ms == KOS_FOREVER ? 0 : time_get_ms() + timeout_ms + 1;
    g_timer_waiter = sched_current_pid();
    for (;;) {
        task_prepare_block();
        uint64_t now = time_get_us(), dl_us = 0;
        bool got, has_next = false;
        LOCKED(got = ktimer_pop_due(owner, now, key, fn, arg);
               if (!got) has_next = ktimer_next(owner, &dl_us));
        if (got) { task_wait_done(); g_timer_waiter = -1; return KO_OK; }
        uint64_t dl_ms = has_next ? (dl_us + 999) / 1000 : 0;
        if (limit && (dl_ms == 0 || limit < dl_ms)) dl_ms = limit;
        if (limit && time_get_ms() >= limit) { task_wait_done(); g_timer_waiter = -1; return KO_TIMEOUT; }
        task_block_until_ms(dl_ms);
    }
}
