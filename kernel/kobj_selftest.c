/* `kobjselftest`: the kernel objects under real tasks, real preemption and
 * real timeouts (45.3b, plan/phase45_esp32c6.md).
 *
 * tests/host/kobj_host.c checks the state machines against reference models
 * with scripted tasks. What it cannot check is the glue: that a wake arriving
 * before the waiter has gone to sleep is not lost, that a timed wait wakes at
 * the right time, that a timeout racing a grant hands the unit to exactly one
 * side. Those are properties of kernel/kobj_sched.c running on this
 * scheduler, so they are tested here, on it. */

#include "kernel/kobj_sched.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include <stdint.h>
#include <stdbool.h>

static int g_fail;

#define CHECK(cond, ...) do { if (!(cond)) { \
    cprintf("  FAIL (line %d): ", __LINE__); cprintf(__VA_ARGS__); cprintf("\n"); g_fail++; } } while (0)

/* Waits (politely) for a counter to reach `n`, up to a second. */
static bool wait_count(volatile int *c, int n) {
    for (int i = 0; i < 1000 && *c < n; i++) task_sleep_ms(1);
    return *c >= n;
}

/* ---- 1. polls, timeouts, FIFO hand-off ----------------------------------- */

static kh_t g_sem;
static volatile int g_order[4], g_norder, g_started;
static volatile int g_rc[4];

static void fifo_waiter(void *arg) {
    int id = (int)(intptr_t)arg;
    g_started++;
    int r = kos_sem_take(g_sem, 2000);
    g_rc[id] = r;
    g_order[g_norder++] = id;
}

static void test_basic(void) {
    cprintf("  semaphore: poll, timeout, FIFO\n");
    g_sem = kos_sem_create(4, 0, 0);
    CHECK(g_sem != 0, "create");
    CHECK(kos_sem_take(g_sem, 0) == KO_AGAIN, "a poll on an empty semaphore");

    uint64_t t0 = time_get_ms();
    int r = kos_sem_take(g_sem, 40);
    uint64_t dt = time_get_ms() - t0;
    CHECK(r == KO_TIMEOUT, "a timed take on an empty semaphore returned %d", r);
    CHECK(dt >= 40 && dt < 200, "the 40 ms timeout took %u ms", (unsigned)dt);

    g_norder = 0; g_started = 0;
    for (int id = 0; id < 3; id++) {
        task_create("kfifo", fifo_waiter, (void *)(intptr_t)id);
        CHECK(wait_count(&g_started, id + 1), "waiter %d never started", id);
        task_sleep_ms(5);            /* so it is queued before the next one starts */
    }
    for (int i = 0; i < 3; i++) { CHECK(kos_sem_give(g_sem) == KO_OK, "give %d", i); task_sleep_ms(5); }
    CHECK(wait_count(&g_norder, 3), "only %d of 3 waiters woke", g_norder);
    CHECK(g_order[0] == 0 && g_order[1] == 1 && g_order[2] == 2, "wake order %d %d %d, expected FIFO",
          g_order[0], g_order[1], g_order[2]);
    CHECK(g_rc[0] == KO_OK && g_rc[1] == KO_OK && g_rc[2] == KO_OK, "every waiter was granted");
    CHECK(kos_sem_delete(g_sem) == KO_OK, "delete");
}

/* ---- 2. a mutex under contention ----------------------------------------- */

static kh_t g_mtx;
static volatile int g_counter, g_in_cs, g_violations, g_mdone;

static void mutex_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 150; i++) {
        if (kos_mutex_lock(g_mtx, KOS_FOREVER) != KO_OK) { g_violations += 1000; break; }
        if (g_in_cs) g_violations++;
        g_in_cs = 1;
        int v = g_counter;
        sched_yield();               /* force a context switch inside the section */
        g_counter = v + 1;
        g_in_cs = 0;
        kos_mutex_unlock(g_mtx);
    }
    g_mdone++;
}

static void test_mutex(void) {
    cprintf("  mutex: three tasks contending, a yield inside the section\n");
    g_mtx = kos_mutex_create(true, 0);
    g_counter = g_in_cs = g_violations = g_mdone = 0;
    for (int i = 0; i < 3; i++) task_create("kmutex", mutex_worker, 0);
    CHECK(wait_count(&g_mdone, 3), "workers finished: %d of 3", g_mdone);
    CHECK(g_violations == 0, "%d entries into an occupied section", g_violations);
    CHECK(g_counter == 450, "counter %d, expected 450 (lost updates)", g_counter);
    CHECK(kos_mutex_lock(g_mtx, 0) == KO_OK && kos_mutex_lock(g_mtx, 0) == KO_OK, "recursive relock");
    CHECK(kos_mutex_unlock(g_mtx) == KO_OK && kos_mutex_unlock(g_mtx) == KO_OK, "and both unlocks");
    CHECK(kos_mutex_unlock(g_mtx) == KO_FAIL, "unlocking a free mutex");
    kos_mutex_delete(g_mtx);
}

/* ---- 3. a queue between two tasks ---------------------------------------- */

static kh_t g_q;
static volatile int g_pdone;

static void producer(void *arg) {
    (void)arg;
    for (uint32_t i = 1; i <= 300; i++)
        if (kos_q_send(g_q, &i, false, KOS_FOREVER) != KO_OK) break;
    g_pdone = 1;
}

static void test_queue(void) {
    cprintf("  queue: 300 items through a queue of 4, blocking both ways\n");
    g_q = kos_q_create(4, sizeof(uint32_t), 0);
    uint32_t v = 0, expect = 1, sum = 0;
    CHECK(kos_q_recv(g_q, &v, 0) == KO_AGAIN, "poll of an empty queue");
    uint64_t t0 = time_get_ms();
    CHECK(kos_q_recv(g_q, &v, 30) == KO_TIMEOUT, "timed receive on an empty queue");
    CHECK(time_get_ms() - t0 >= 30, "returned early");
    g_pdone = 0;
    task_create("kprod", producer, 0);
    for (int n = 0; n < 300; n++) {
        int r = kos_q_recv(g_q, &v, 1000);
        if (r != KO_OK) { CHECK(0, "receive %d returned %d", n, r); break; }
        if (v != expect++) { CHECK(0, "item %u out of order at %d", v, n); break; }
        sum += v;
    }
    CHECK(sum == 300u * 301u / 2u, "sum %u", sum);
    CHECK(wait_count(&g_pdone, 1), "producer finished");
    kos_q_delete(g_q);
}

/* ---- 4. an event group --------------------------------------------------- */

static kh_t g_ev;
static volatile uint32_t g_ev_out;
static volatile int g_ev_rc = -99, g_ev_done;

static void ev_waiter(void *arg) {
    (void)arg;
    uint32_t out = 0;
    g_ev_rc = kos_ev_wait(g_ev, 0x3, true, true, 500, &out);
    g_ev_out = out;
    g_ev_done = 1;
}

static void test_event(void) {
    cprintf("  event group: wait for two bits, set one then the other\n");
    g_ev = kos_ev_create(0);
    g_ev_done = 0; g_ev_rc = -99;
    task_create("kev", ev_waiter, 0);
    task_sleep_ms(10);
    kos_ev_set(g_ev, 0x1, 0);
    task_sleep_ms(10);
    CHECK(!g_ev_done, "released with only one of two bits");
    kos_ev_set(g_ev, 0x2, 0);
    CHECK(wait_count(&g_ev_done, 1), "never released");
    CHECK(g_ev_rc == KO_OK && g_ev_out == 0x3, "rc %d bits %x", g_ev_rc, (unsigned)g_ev_out);
    uint32_t before = 0xff;
    kos_ev_clear(g_ev, 0xff, &before);
    CHECK(before == 0, "the waiter's clear-on-exit left %x behind", (unsigned)before);
    kos_ev_delete(g_ev);
}

/* ---- 5. timers through the timer thread ---------------------------------- */

#define OWNER 0x1234u
static volatile uintptr_t g_fired[16];
static volatile uint32_t g_fired_at[16];
static volatile int g_nfired, g_tstop;

static void timer_thread(void *arg) {
    (void)arg;
    while (!g_tstop) {
        uintptr_t key, fn, a;
        if (kos_timer_wait(OWNER, &key, &fn, &a, 50) != KO_OK) continue;
        if (g_nfired < 16) { g_fired_at[g_nfired] = (uint32_t)time_get_ms(); g_fired[g_nfired++] = key; }
    }
    g_tstop = 2;
}

static void test_timers(void) {
    cprintf("  timers: ordering, a periodic one, disarm\n");
    g_nfired = 0; g_tstop = 0;
    task_create("ktimer", timer_thread, 0);
    task_sleep_ms(5);
    uint32_t t0 = (uint32_t)time_get_ms();
    kos_timer_arm(OWNER, 0xC, 1, 1, 90000, false);          /* armed out of order */
    kos_timer_arm(OWNER, 0xA, 1, 1, 30000, false);
    kos_timer_arm(OWNER, 0xB, 1, 1, 60000, false);
    CHECK(wait_count(&g_nfired, 3), "only %d of 3 one-shots fired", g_nfired);
    CHECK(g_fired[0] == 0xA && g_fired[1] == 0xB && g_fired[2] == 0xC,
          "fired %x %x %x, expected sorted by deadline", (unsigned)g_fired[0], (unsigned)g_fired[1], (unsigned)g_fired[2]);
    for (int i = 0; i < 3 && i < g_nfired; i++) {
        uint32_t want = 30u * (uint32_t)(i + 1), got = g_fired_at[i] - t0;
        CHECK(got >= want && got < want + 40, "timer %d fired after %u ms, wanted ~%u", i, got, want);
    }

    g_nfired = 0;
    kos_timer_arm(OWNER, 0xD, 1, 1, 20000, true);
    task_sleep_ms(130);
    int n = g_nfired;
    CHECK(n >= 4 && n <= 7, "a 20 ms periodic timer fired %d times in 130 ms", n);
    kos_timer_disarm(OWNER, 0xD);
    task_sleep_ms(10);
    n = g_nfired;
    task_sleep_ms(70);
    CHECK(g_nfired == n, "fired %d more times after the disarm", g_nfired - n);

    /* Re-arming an armed timer replaces it, and a wake from arm must make the
     * timer thread recompute rather than sleep through the earlier deadline. */
    g_nfired = 0;
    kos_timer_arm(OWNER, 0xE, 1, 1, 400000, false);          /* thread now waits ~400 ms ... */
    task_sleep_ms(10);
    kos_timer_arm(OWNER, 0xF, 1, 1, 20000, false);           /* ... and must wake for this */
    CHECK(wait_count(&g_nfired, 1) && g_fired[0] == 0xF, "an earlier timer armed under a sleeping thread was slept through");
    kos_timer_done(OWNER, 0xE);
    kos_timer_done(OWNER, 0xF);

    g_tstop = 1;
    for (int i = 0; i < 200 && g_tstop != 2; i++) task_sleep_ms(1);
    CHECK(g_tstop == 2, "the timer thread did not stop");
}

/* ---- 6. delete under a waiter, and a timeout racing a grant -------------- */

static volatile int g_del_rc = -99, g_del_done;

static void del_waiter(void *arg) {
    g_del_rc = kos_sem_take((kh_t)(uintptr_t)arg, KOS_FOREVER);
    g_del_done = 1;
}

static volatile int g_race_rc, g_race_done;
static kh_t g_race_sem;

static void race_waiter(void *arg) {
    (void)arg;
    g_race_rc = kos_sem_take(g_race_sem, 3);
    g_race_done = 1;
}

static void test_delete_and_race(void) {
    cprintf("  delete under a waiter; 100 timeouts racing a give\n");
    kh_t s = kos_sem_create(1, 0, 0);
    g_del_done = 0; g_del_rc = -99;
    task_create("kdel", del_waiter, (void *)(uintptr_t)s);
    task_sleep_ms(10);
    kos_sem_delete(s);
    CHECK(wait_count(&g_del_done, 1) && g_del_rc == KO_DELETED, "waiter saw %d, expected KO_DELETED", g_del_rc);
    CHECK(kos_sem_give(s) == KO_FAIL, "a stale handle after the delete");

    /* The waiter times out after 3 ms; the give lands somewhere between 1 and
     * 5 ms. Either the waiter got the unit, or the unit is still in the
     * semaphore -- never both, never neither. */
    g_race_sem = kos_sem_create(1, 0, 0);
    int granted = 0, left = 0;
    for (int i = 0; i < 100; i++) {
        g_race_done = 0; g_race_rc = -99;
        task_create("krace", race_waiter, 0);
        task_sleep_ms(1 + (uint32_t)(i % 5));
        kos_sem_give(g_race_sem);
        if (!wait_count(&g_race_done, 1)) { CHECK(0, "round %d: the waiter never returned", i); break; }
        int in_sem = kos_sem_take(g_race_sem, 0) == KO_OK;
        if (g_race_rc == KO_OK) granted++;
        CHECK(g_race_rc == KO_OK || g_race_rc == KO_TIMEOUT, "round %d: waiter returned %d", i, g_race_rc);
        CHECK((g_race_rc == KO_OK) + in_sem == 1, "round %d: rc %d, unit still in the semaphore %d", i, g_race_rc, in_sem);
        left += in_sem;
    }
    cprintf("    %d granted, %d left in the semaphore, 100 total\n", granted, left);
    CHECK(granted + left == 100, "units not conserved: %d + %d", granted, left);
    CHECK(granted > 5 && left > 5, "the race never went both ways (%d/%d) -- the test is not testing", granted, left);
    kos_sem_delete(g_race_sem);
}

int kobj_selftest(void) {
    kos_init();
    g_fail = 0;
    cprintf("kobjselftest:\n");
    test_basic();
    test_mutex();
    test_queue();
    test_event();
    test_timers();
    test_delete_and_race();
    if (g_fail == 0) cprintf("KOBJSELFTEST_OK\n");
    else cprintf("KOBJSELFTEST_FAIL (%d)\n", g_fail);
    return g_fail;
}
