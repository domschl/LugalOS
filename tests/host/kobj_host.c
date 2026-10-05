/*
 * kernel/kobj.c on the host, under ASan/UBSan or valgrind (45.3b,
 * plan/phase45_esp32c6.md).
 *
 * The module is a set of state machines that never block, so its semantics can
 * be checked without a scheduler: this plays a handful of scripted "tasks"
 * against it. Two kinds of test --
 *
 *   * deterministic scenarios for the behaviours the Wi-Fi blob's own code
 *     depends on (FIFO order, direct hand-off, a recursive mutex, a timeout
 *     that races a grant, an object deleted under its waiters, a stale handle);
 *   * randomized runs of each object type against an independent reference
 *     model (a plain array standing in for the queue, a counter and a FIFO of
 *     task numbers for the semaphore, ...), comparing every result and every
 *     wake list. A divergence is a bug in one of the two; the seed replays it.
 *
 * Usage: kobj_host [iterations [seed]].
 */

#include "kernel/kobj.h"
#include "shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define OWN 0     /* the kernel's own timers; test_ownership() uses real domains */
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); g_failures++; abort(); } } while (0)

static void *host_alloc(uint32_t n) { return calloc(1, n); }
static void host_free(void *p) { free(p); }
static const kobj_env_t ENV = { host_alloc, host_free };

static bool woke(const kwake_t *k, int who) {
    for (int i = 0; i < k->n; i++) if (k->who[i] == who) return true;
    return false;
}

/* ===================== deterministic scenarios ============================ */

static void test_semaphore(void) {
    kobj_init(&ENV);
    kh_t s = ksem_create(3, 1);
    CHECK(s != 0, "create");
    CHECK(ksem_create(0, 0) == 0 && ksem_create(2, 3) == 0, "bad create args accepted");
    kwake_t wk; kwaiter_t a, b, c;

    CHECK(ksem_take(s, NULL, 1) == KO_OK, "take the initial unit");
    CHECK(ksem_take(s, NULL, 1) == KO_AGAIN, "poll an empty semaphore");
    CHECK(ksem_take(s, &a, 10) == KO_BLOCK && a.state == KW_WAIT, "queue A");
    CHECK(ksem_take(s, &b, 11) == KO_BLOCK, "queue B");
    CHECK(ksem_take(s, &c, 12) == KO_BLOCK, "queue C");

    /* FIFO, and the unit goes to the waiter rather than into the count: a
     * fourth task polling right after a give must not steal it. */
    kwake_init(&wk);
    CHECK(ksem_give(s, &wk) == KO_OK && wk.n == 1 && wk.who[0] == 10, "give wakes A first");
    CHECK(a.state == KW_GRANT, "A granted");
    CHECK(ksem_count(s) == 0, "hand-off must not bump the count");
    CHECK(ksem_take(s, NULL, 13) == KO_AGAIN, "no stealing after a hand-off");

    /* B times out; C is still next. */
    CHECK(ksem_cancel(s, &b) == KO_TIMEOUT && b.state == KW_IDLE, "B's timeout");
    kwake_init(&wk);
    CHECK(ksem_give(s, &wk) == KO_OK && wk.n == 1 && wk.who[0] == 12, "C is next after B left");

    /* A timeout that loses the race to a grant: the grant wins. */
    kwaiter_t d;
    CHECK(ksem_take(s, &d, 14) == KO_BLOCK, "queue D");
    kwake_init(&wk);
    ksem_give(s, &wk);
    CHECK(ksem_cancel(s, &d) == KO_OK, "granted before the cancel arrived: the unit is D's");

    /* Capacity. */
    ksem_give(s, NULL); ksem_give(s, NULL); ksem_give(s, NULL);
    CHECK(ksem_count(s) == 3 && ksem_give(s, NULL) == KO_FULL, "give at capacity");

    /* Delete under waiters. */
    kh_t t = ksem_create(1, 0);
    kwaiter_t e, f;
    ksem_take(t, &e, 20); ksem_take(t, &f, 21);
    kwake_init(&wk);
    CHECK(ksem_delete(t, &wk) == KO_OK && wk.n == 2, "delete wakes every waiter");
    CHECK(e.state == KW_FAILED && f.state == KW_FAILED, "waiters see the deletion");
    CHECK(ksem_cancel(t, &e) == KO_DELETED, "cancel after delete");
    CHECK(ksem_take(t, NULL, 1) == KO_FAIL && ksem_give(t, NULL) == KO_FAIL, "stale handle refused");

    /* The slot is reused under a new generation: the old handle must still miss. */
    kh_t t2 = ksem_create(1, 1);
    CHECK(t2 != t, "slot reuse changes the handle");
    CHECK(ksem_take(t, NULL, 1) == KO_FAIL, "stale handle still refused after reuse");
    CHECK(ksem_take(t2, NULL, 1) == KO_OK, "the new object works");

    /* Forged handles. */
    CHECK(ksem_take(0, NULL, 1) == KO_FAIL, "handle 0");
    CHECK(ksem_take(0xdeadbeef, NULL, 1) == KO_FAIL, "garbage handle");
    CHECK(ksem_take(kmutex_create(false), NULL, 1) == KO_FAIL, "a mutex handle is not a semaphore");

    /* Exhaustion. */
    kobj_init(&ENV);
    int n = 0; while (ksem_create(1, 0) != 0) n++;
    CHECK(n == KSEM_MAX, "table holds exactly %d, got %d", KSEM_MAX, n);
}

static void test_mutex(void) {
    kobj_init(&ENV);
    kh_t m = kmutex_create(true), p = kmutex_create(false);
    kwake_t wk; kwaiter_t a, b;

    CHECK(kmutex_lock(m, NULL, 1) == KO_OK && kmutex_owner(m) == 1, "lock");
    CHECK(kmutex_lock(m, NULL, 1) == KO_OK, "recursive relock");
    CHECK(kmutex_lock(m, NULL, 2) == KO_AGAIN, "other task polls a held mutex");
    CHECK(kmutex_unlock(m, 2, NULL) == KO_FAIL, "unlock by a non-owner");
    CHECK(kmutex_lock(m, &a, 2) == KO_BLOCK, "queue A");
    CHECK(kmutex_lock(m, &b, 3) == KO_BLOCK, "queue B");
    kwake_init(&wk);
    CHECK(kmutex_unlock(m, 1, &wk) == KO_OK && wk.n == 0 && kmutex_owner(m) == 1, "depth 2 -> 1: still held");
    CHECK(kmutex_unlock(m, 1, &wk) == KO_OK && wk.n == 1 && wk.who[0] == 2, "released: A next");
    CHECK(kmutex_owner(m) == 2 && a.state == KW_GRANT, "ownership passed straight to A");
    CHECK(kmutex_unlock(m, 2, &wk) == KO_OK && kmutex_owner(m) == 3, "then B");
    CHECK(kmutex_unlock(m, 3, &wk) == KO_OK && kmutex_owner(m) == -1, "then free");

    /* A non-recursive mutex re-locked by its owner is a deadlock: refused. */
    CHECK(kmutex_lock(p, NULL, 5) == KO_OK, "plain lock");
    CHECK(kmutex_lock(p, &a, 5) == KO_FAIL, "plain relock is refused, not waited on");
}

static void test_queue(void) {
    kobj_init(&ENV);
    CHECK(kq_create(0, 4) == 0 && kq_create(4, 0) == 0 && kq_create(4, 4096) == 0, "bad create args");
    kh_t q = kq_create(3, sizeof(uint32_t));
    kwake_t wk; kwaiter_t r, s;
    uint32_t v, got;

    v = 1; CHECK(kq_send(q, &v, false, NULL, 1, &wk) == KO_OK, "send 1");
    v = 2; CHECK(kq_send(q, &v, false, NULL, 1, &wk) == KO_OK, "send 2");
    v = 9; CHECK(kq_send(q, &v, true, NULL, 1, &wk) == KO_OK, "send 9 to the front");
    CHECK(kq_waiting(q) == 3, "three queued");
    v = 4; CHECK(kq_send(q, &v, false, NULL, 1, &wk) == KO_FULL, "send to a full queue, no wait");
    CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_OK && got == 9, "front item first");
    CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_OK && got == 1, "then FIFO");
    CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_OK && got == 2, "then 2");
    CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_AGAIN, "empty, no wait");

    /* A receiver waits; the send copies straight into it. */
    uint32_t slot = 0;
    CHECK(kq_recv(q, &slot, &r, 7, &wk) == KO_BLOCK, "receiver waits");
    kwake_init(&wk);
    v = 42; CHECK(kq_send(q, &v, false, NULL, 1, &wk) == KO_OK && wk.n == 1 && wk.who[0] == 7, "wakes the receiver");
    CHECK(slot == 42 && r.state == KW_GRANT && kq_waiting(q) == 0, "item handed straight over, queue still empty");

    /* A sender blocks on a full queue; a receive admits it. */
    for (v = 100; v < 103; v++) kq_send(q, &v, false, NULL, 1, &wk);
    uint32_t pending = 200;
    CHECK(kq_send(q, &pending, false, &s, 8, &wk) == KO_BLOCK, "sender waits on a full queue");
    kwake_init(&wk);
    CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_OK && got == 100, "oldest first");
    CHECK(wk.n == 1 && wk.who[0] == 8 && s.state == KW_GRANT && kq_waiting(q) == 3, "blocked sender admitted");
    kq_recv(q, &got, NULL, 1, &wk); kq_recv(q, &got, NULL, 1, &wk);
    CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_OK && got == 200, "the admitted item comes out last");

    /* Wraparound: many more items than slots. */
    for (uint32_t i = 0; i < 1000; i++) {
        kq_send(q, &i, false, NULL, 1, &wk);
        CHECK(kq_recv(q, &got, NULL, 1, &wk) == KO_OK && got == i, "wraparound at %u", i);
    }

    /* Timeout, then delete under a waiter. */
    kwaiter_t t;
    CHECK(kq_recv(q, &slot, &t, 9, &wk) == KO_BLOCK, "wait");
    CHECK(kq_cancel(q, &t) == KO_TIMEOUT, "timeout");
    kq_recv(q, &slot, &t, 9, &wk);
    kwake_init(&wk);
    CHECK(kq_delete(q, &wk) == KO_OK && wk.n == 1 && t.state == KW_FAILED, "delete fails the waiter");
    CHECK(kq_cancel(q, &t) == KO_DELETED, "and the cancel says so");
    CHECK(kq_send(q, &v, false, NULL, 1, NULL) == KO_FAIL, "stale queue handle");
}

static void test_event_group(void) {
    kobj_init(&ENV);
    kh_t e = kev_create();
    kwake_t wk; kwaiter_t a, b, c;
    uint32_t out;

    CHECK(kev_wait(e, 0x3, true, false, &out, NULL, 1) == KO_AGAIN && out == 0, "wait-all on nothing");
    CHECK(kev_wait(e, 0, true, false, &out, NULL, 1) == KO_FAIL, "an empty mask is refused");
    kev_set(e, 0x1, &wk);
    CHECK(kev_wait(e, 0x3, true, false, &out, NULL, 1) == KO_AGAIN, "wait-all needs both bits");
    CHECK(kev_wait(e, 0x3, false, false, &out, NULL, 1) == KO_OK && out == 0x1, "wait-any with one");

    /* Three waiters; one set satisfies two of them, and the first clears the
     * bits it consumed before the second is judged. */
    CHECK(kev_wait(e, 0x6, true, true, &out, &a, 10) == KO_BLOCK, "A: all of 0x6, clear");
    CHECK(kev_wait(e, 0x4, false, false, &out, &b, 11) == KO_BLOCK, "B: any of 0x4");
    CHECK(kev_wait(e, 0x8, false, false, &out, &c, 12) == KO_BLOCK, "C: any of 0x8");
    kwake_init(&wk);
    uint32_t after = kev_set(e, 0x6, &wk);
    CHECK(woke(&wk, 10) && !woke(&wk, 11) && !woke(&wk, 12), "A is satisfied, and its clear starves B");
    CHECK(a.state == KW_GRANT && a.got == 0x7, "A saw the bits as they stood: %x", a.got);
    CHECK(after == 0x1, "bits left after A cleared 0x6: %x", after);
    kwake_init(&wk);
    kev_set(e, 0x8, &wk);
    CHECK(woke(&wk, 12) && c.state == KW_GRANT, "C granted");
    CHECK(kev_cancel(e, &b, &out) == KO_TIMEOUT && out == (0x1 | 0x8), "B timed out with the bits as they stand: %x", out);
    CHECK(kev_clear(e, 0xff) == 0x9, "clear returns the previous value");

    kwaiter_t d;
    kev_wait(e, 0x1, true, false, &out, &d, 13);
    kwake_init(&wk);
    CHECK(kev_delete(e, &wk) == KO_OK && d.state == KW_FAILED && wk.n == 1, "delete under a waiter");
}

static void test_ownership(void) {
    kobj_init(&ENV);
    const uintptr_t A = 0xA000, B = 0xB000;
    kh_t s = ksem_create(1, 1), q = kq_create(4, 8), m = kmutex_create(false), e = kev_create();
    CHECK(kobj_owned_by(s, 0) && kobj_owned_by(q, 0), "objects start kernel-owned");
    CHECK(kobj_set_owner(s, A) == KO_OK && kobj_set_owner(q, A) == KO_OK, "assign to domain A");
    CHECK(kobj_owned_by(s, A) && !kobj_owned_by(s, B) && !kobj_owned_by(s, 0), "A owns it, nobody else does");
    CHECK(kq_item_size(q) == 8 && kq_item_size(s) == 0 && kq_item_size(0) == 0, "item size, 0 for non-queues");
    CHECK(kobj_set_owner(0xdeadbeef, A) == KO_FAIL && !kobj_owned_by(0, 0), "no owner for a forged handle");
    ksem_delete(s, NULL);
    CHECK(!kobj_owned_by(s, A), "a deleted object has no owner");
    (void)m; (void)e;

    /* Timers: the same key in two domains is two timers, and each domain sees
     * only its own. */
    CHECK(ktimer_arm(A, 0x1000, 0xfa, 0xaa, 0, 100, false) == KO_OK, "A arms 0x1000");
    CHECK(ktimer_arm(B, 0x1000, 0xfb, 0xab, 0, 50, false) == KO_OK, "B arms the same key");
    uintptr_t key, fn, arg; uint64_t dl;
    CHECK(ktimer_next(A, &dl) && dl == 100 && ktimer_next(B, &dl) && dl == 50, "each sees its own deadline");
    CHECK(!ktimer_next(0, &dl), "the kernel's owner sees neither");
    CHECK(!ktimer_pop_due(A, 60, &key, &fn, &arg), "B's timer is not A's to pop");
    CHECK(ktimer_pop_due(B, 60, &key, &fn, &arg) && fn == 0xfb, "B pops its own");
    CHECK(ktimer_disarm(B, 0x1000) == KO_OK && ktimer_armed(A, 0x1000), "B disarming leaves A's armed");
    CHECK(ktimer_done(B, 0x1000) == KO_OK && ktimer_armed(A, 0x1000) && ktimer_done(B, 0x1000) == KO_FAIL, "done is per owner");
}

static void test_timers(void) {
    kobj_init(&ENV);
    uintptr_t key, fn, arg;

    CHECK(ktimer_setfn(OWN, 0x1000, 0xf1, 0xa1) == KO_OK, "setfn creates the slot");
    CHECK(!ktimer_armed(OWN, 0x1000), "setfn does not arm");
    CHECK(ktimer_arm(OWN, 0x1000, 0, 0, 1000, 500, false) == KO_OK, "arm one-shot; keeps the stored callback");
    CHECK(ktimer_arm(OWN, 0x2000, 0xf2, 0xa2, 1000, 200, false) == KO_OK, "arm another");
    CHECK(ktimer_arm(OWN, 0x3000, 0xf3, 0xa3, 1000, 200, false) == KO_OK, "and a third, same deadline");

    uint64_t next;
    CHECK(ktimer_next(OWN, &next) && next == 1200, "earliest deadline");
    CHECK(!ktimer_pop_due(OWN, 1199, &key, &fn, &arg), "nothing due yet");
    CHECK(ktimer_pop_due(OWN, 1200, &key, &fn, &arg) && key == 0x2000 && fn == 0xf2 && arg == 0xa2, "ties go in arming order");
    CHECK(ktimer_pop_due(OWN, 1200, &key, &fn, &arg) && key == 0x3000, "then the third");
    CHECK(!ktimer_pop_due(OWN, 1200, &key, &fn, &arg), "the one-shots are spent");
    CHECK(!ktimer_armed(OWN, 0x2000) && ktimer_count() == 3, "a fired one-shot is disarmed but keeps its slot");
    CHECK(ktimer_pop_due(OWN, 1500, &key, &fn, &arg) && key == 0x1000 && fn == 0xf1 && arg == 0xa1, "setfn's callback survived the arm");

    /* Periodic: reschedules from the previous deadline; if it fell behind,
     * from `now` -- one firing, not a burst. */
    ktimer_arm(OWN, 0x4000, 0xf4, 0xa4, 0, 100, true);
    CHECK(ktimer_pop_due(OWN, 100, &key, &fn, &arg) && key == 0x4000, "first period");
    CHECK(ktimer_next(OWN, &next) && next == 200, "next period from the old deadline, not from now");
    CHECK(ktimer_pop_due(OWN, 1000, &key, &fn, &arg) && key == 0x4000, "late by nine periods: fires once");
    CHECK(!ktimer_pop_due(OWN, 1000, &key, &fn, &arg), "...not nine times");
    CHECK(ktimer_next(OWN, &next) && next == 1100, "rescheduled from now");

    /* Re-arm replaces; disarm keeps; done frees. */
    ktimer_arm(OWN, 0x4000, 0, 0, 1000, 50, false);
    CHECK(ktimer_next(OWN, &next) && next == 1050, "re-arm replaced the periodic");
    CHECK(ktimer_disarm(OWN, 0x4000) == KO_OK && !ktimer_armed(OWN, 0x4000), "disarm");
    CHECK(!ktimer_next(OWN, &next), "nothing armed");
    uint32_t before = ktimer_count();
    CHECK(ktimer_done(OWN, 0x4000) == KO_OK && ktimer_count() == before - 1, "done frees the slot");
    CHECK(ktimer_done(OWN, 0x4000) == KO_FAIL && ktimer_disarm(OWN, 0x9999) == KO_FAIL, "unknown keys");
    CHECK(ktimer_arm(OWN, 0x5000, 1, 1, 0, 0, true) == KO_OK && ktimer_next(OWN, &next) && next == 1, "a zero period is clamped, not spun on");

    /* Exhaustion. */
    kobj_init(&ENV);
    int n = 0; while (ktimer_setfn(OWN, 0x10000 + 16 * (uintptr_t)n, 1, 1) == KO_OK) n++;
    CHECK(n == KTIMER_MAX, "table holds %d", n);
}

/* ===================== randomized runs against a model ==================== */

#define NT 8   /* scripted tasks */

typedef struct { bool waiting; kwaiter_t w; uint32_t slot; } ftask_t;

static uint64_t g_rs;
static uint32_t rnd(uint32_t n) { return host_rand(&g_rs) % n; }

/* ---- semaphore against (count, FIFO of task numbers) ---- */
static void fuzz_semaphore(int iters) {
    kobj_init(&ENV);
    const uint32_t max = 1 + rnd(4);
    uint32_t mcount = rnd(max + 1);
    kh_t s = ksem_create(max, mcount);
    ftask_t t[NT] = {0};
    int fifo[NT], nf = 0;

    for (int it = 0; it < iters; it++) {
        int who = (int)rnd(NT);
        switch (rnd(4)) {
        case 0:                                   /* take, blocking */
            if (t[who].waiting) break;
            if (mcount > 0) { CHECK(ksem_take(s, &t[who].w, who) == KO_OK, "model says a unit is free"); mcount--; }
            else { CHECK(ksem_take(s, &t[who].w, who) == KO_BLOCK, "model says none free");
                   t[who].waiting = true; fifo[nf++] = who; }
            break;
        case 1: {                                 /* give */
            kwake_t wk; kwake_init(&wk);
            int r = ksem_give(s, &wk);
            if (nf > 0) {
                int head = fifo[0]; memmove(fifo, fifo + 1, (size_t)(--nf) * sizeof fifo[0]);
                CHECK(r == KO_OK && wk.n == 1 && wk.who[0] == head, "grant goes to the FIFO head %d", head);
                CHECK(t[head].w.state == KW_GRANT, "granted");
                t[head].waiting = false;
            } else if (mcount < max) { CHECK(r == KO_OK && wk.n == 0, "count bump"); mcount++; }
            else CHECK(r == KO_FULL, "full");
            break; }
        case 2:                                   /* a waiter times out */
            if (!t[who].waiting) break;
            CHECK(ksem_cancel(s, &t[who].w) == KO_TIMEOUT, "still queued, so a timeout");
            for (int i = 0; i < nf; i++) if (fifo[i] == who) { memmove(fifo + i, fifo + i + 1, (size_t)(nf - i - 1) * sizeof fifo[0]); nf--; break; }
            t[who].waiting = false;
            break;
        case 3: {                                 /* poll */
            int r = ksem_take(s, NULL, who);
            if (mcount > 0) { CHECK(r == KO_OK, "poll"); mcount--; } else CHECK(r == KO_AGAIN, "poll empty");
            break; }
        }
        CHECK(ksem_count(s) == mcount, "count %u vs model %u", ksem_count(s), mcount);
    }
    kwake_t wk; kwake_init(&wk);
    ksem_delete(s, &wk);
    CHECK(wk.n == nf, "delete wakes exactly the remaining waiters");
}

/* ---- mutex against (owner, depth, FIFO) ---- */
static void fuzz_mutex(int iters) {
    kobj_init(&ENV);
    bool rec = rnd(2);
    kh_t m = kmutex_create(rec);
    ftask_t t[NT] = {0};
    int owner = -1; uint32_t depth = 0;
    int fifo[NT], nf = 0;

    for (int it = 0; it < iters; it++) {
        int who = (int)rnd(NT);
        if (rnd(2)) {                              /* lock */
            if (t[who].waiting) continue;
            int r = kmutex_lock(m, &t[who].w, who);
            if (owner < 0) { CHECK(r == KO_OK, "free mutex"); owner = who; depth = 1; }
            else if (owner == who) { if (rec) { CHECK(r == KO_OK, "recursive"); depth++; } else CHECK(r == KO_FAIL, "relock refused"); }
            else { CHECK(r == KO_BLOCK, "held"); t[who].waiting = true; fifo[nf++] = who; }
        } else {                                   /* unlock */
            kwake_t wk; kwake_init(&wk);
            int r = kmutex_unlock(m, who, &wk);
            if (owner != who) { CHECK(r == KO_FAIL && wk.n == 0, "non-owner unlock"); continue; }
            CHECK(r == KO_OK, "owner unlock");
            if (--depth > 0) { CHECK(wk.n == 0, "still held"); continue; }
            if (nf > 0) {
                int head = fifo[0]; memmove(fifo, fifo + 1, (size_t)(--nf) * sizeof fifo[0]);
                CHECK(wk.n == 1 && wk.who[0] == head, "passed to the FIFO head");
                owner = head; depth = 1; t[head].waiting = false;
            } else owner = -1;
        }
        CHECK(kmutex_owner(m) == owner, "owner %d vs model %d", kmutex_owner(m), owner);
    }
}

/* ---- queue against a plain array ---- */
static void fuzz_queue(int iters) {
    kobj_init(&ENV);
    const uint32_t len = 1 + rnd(5);
    kh_t q = kq_create(len, sizeof(uint32_t));
    uint32_t model[8]; uint32_t mn = 0;           /* model[0] is the next out */
    ftask_t rx[NT] = {0}, tx[NT] = {0};
    int rfifo[NT], nr = 0;                        /* waiting receivers */
    int sfifo[NT], ns = 0;                        /* waiting senders */
    uint32_t sval[NT]; bool sfront[NT];
    uint32_t next = 1;

    for (int it = 0; it < iters; it++) {
        int who = (int)rnd(NT);
        switch (rnd(4)) {
        case 0: case 1: {                         /* send (maybe to the front, maybe blocking) */
            if (tx[who].waiting) break;
            bool front = rnd(4) == 0, block = rnd(2);
            uint32_t v = next++;
            sval[who] = v; sfront[who] = front;
            kwake_t wk; kwake_init(&wk);
            int r = kq_send(q, &sval[who], front, block ? &tx[who].w : NULL, who, &wk);
            if (nr > 0) {                         /* a receiver is waiting, the queue is empty */
                int h = rfifo[0]; memmove(rfifo, rfifo + 1, (size_t)(--nr) * sizeof rfifo[0]);
                CHECK(r == KO_OK && wk.n == 1 && wk.who[0] == h && rx[h].slot == v, "handed straight to receiver %d", h);
                rx[h].waiting = false;
            } else if (mn < len) {
                CHECK(r == KO_OK, "room");
                if (front) { memmove(model + 1, model, mn * sizeof model[0]); model[0] = v; } else model[mn] = v;
                mn++;
            } else if (block) { CHECK(r == KO_BLOCK, "full: blocks"); tx[who].waiting = true; sfifo[ns++] = who; }
            else CHECK(r == KO_FULL, "full: refused");
            break; }
        case 2: {                                 /* receive */
            if (rx[who].waiting) break;
            bool block = rnd(2);
            kwake_t wk; kwake_init(&wk);
            int r = kq_recv(q, &rx[who].slot, block ? &rx[who].w : NULL, who, &wk);
            if (mn > 0) {
                CHECK(r == KO_OK && rx[who].slot == model[0], "got %u, model %u", rx[who].slot, model[0]);
                memmove(model, model + 1, (--mn) * sizeof model[0]);
                if (ns > 0) {
                    int h = sfifo[0]; memmove(sfifo, sfifo + 1, (size_t)(--ns) * sizeof sfifo[0]);
                    CHECK(wk.n == 1 && wk.who[0] == h, "blocked sender %d admitted", h);
                    if (sfront[h]) { memmove(model + 1, model, mn * sizeof model[0]); model[0] = sval[h]; } else model[mn] = sval[h];
                    mn++; tx[h].waiting = false;
                } else CHECK(wk.n == 0, "no sender to wake");
            } else if (block) { CHECK(r == KO_BLOCK, "empty: blocks"); rx[who].waiting = true; rfifo[nr++] = who; }
            else CHECK(r == KO_AGAIN, "empty: refused");
            break; }
        case 3: {                                 /* a waiter times out */
            if (rx[who].waiting) {
                CHECK(kq_cancel(q, &rx[who].w) == KO_TIMEOUT, "rx timeout");
                for (int i = 0; i < nr; i++) if (rfifo[i] == who) { memmove(rfifo + i, rfifo + i + 1, (size_t)(nr - i - 1) * sizeof rfifo[0]); nr--; break; }
                rx[who].waiting = false;
            } else if (tx[who].waiting) {
                CHECK(kq_cancel(q, &tx[who].w) == KO_TIMEOUT, "tx timeout");
                for (int i = 0; i < ns; i++) if (sfifo[i] == who) { memmove(sfifo + i, sfifo + i + 1, (size_t)(ns - i - 1) * sizeof sfifo[0]); ns--; break; }
                tx[who].waiting = false;
            }
            break; }
        }
        CHECK(kq_waiting(q) == mn, "depth %u vs model %u", kq_waiting(q), mn);
    }
    kwake_t wk; kwake_init(&wk);
    kq_delete(q, &wk);
    CHECK(wk.n == nr + ns, "delete wakes every waiter");
}

/* ---- timers against a sorted model ---- */
static void fuzz_timers(int iters) {
    kobj_init(&ENV);
    struct { bool used, armed, periodic; uint64_t dl, per, seq; } m[6] = {{0}};
    uint64_t now = 0, seq = 0;
    for (int it = 0; it < iters; it++) {
        int k = (int)rnd(6); uintptr_t key = 0x100 + 16u * (uintptr_t)k;
        switch (rnd(5)) {
        case 0: case 1: {
            uint64_t d = rnd(300); bool per = rnd(2);
            CHECK(ktimer_arm(OWN, key, 1, 1, now, d, per) == KO_OK, "arm");
            if (per && d == 0) d = 1;
            m[k].used = m[k].armed = true; m[k].periodic = per; m[k].dl = now + d; m[k].per = d; m[k].seq = ++seq;
            break; }
        case 2: if (m[k].used) { CHECK(ktimer_disarm(OWN, key) == KO_OK, "disarm"); m[k].armed = false; } break;
        case 3: if (m[k].used) { CHECK(ktimer_done(OWN, key) == KO_OK, "done"); m[k].used = m[k].armed = false; } break;
        case 4: {
            now += rnd(250);
            for (;;) {
                int best = -1;
                for (int i = 0; i < 6; i++)
                    if (m[i].used && m[i].armed && m[i].dl <= now &&
                        (best < 0 || m[i].dl < m[best].dl || (m[i].dl == m[best].dl && m[i].seq < m[best].seq))) best = i;
                uintptr_t kk, f, a;
                bool got = ktimer_pop_due(OWN, now, &kk, &f, &a);
                CHECK(got == (best >= 0), "pop at %llu: got %d, model %d", (unsigned long long)now, got, best >= 0);
                if (!got) break;
                CHECK(kk == 0x100 + 16u * (uintptr_t)best, "which timer: %zx vs %d", kk, best);
                if (m[best].periodic) {
                    uint64_t nx = m[best].dl + m[best].per; if (nx <= now) nx = now + m[best].per;
                    m[best].dl = nx; m[best].seq = ++seq;
                } else m[best].armed = false;
            }
            break; }
        }
        uint64_t nd = 0; bool any = ktimer_next(OWN, &nd);
        uint64_t mdl = 0; bool many = false;
        for (int i = 0; i < 6; i++) if (m[i].used && m[i].armed && (!many || m[i].dl < mdl)) { mdl = m[i].dl; many = true; }
        CHECK(any == many && (!any || nd == mdl), "next deadline");
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x40c0ffee;
    printf("kobj_host: seed %#llx, %d iterations per object type\n", (unsigned long long)seed, iterations);

    test_semaphore();
    test_mutex();
    test_queue();
    test_event_group();
    test_timers();
    test_ownership();
    printf("kobj_host: scenarios pass\n");

    g_rs = seed | 1;
    for (int run = 0; run < 40; run++) {
        fuzz_semaphore(iterations);
        fuzz_mutex(iterations);
        fuzz_queue(iterations);
        fuzz_timers(iterations);
    }
    kobj_init(&ENV);                  /* frees the last run's queue storage */
    printf("kobj_host: randomized runs agree with the models (40 x 4 x %d steps)\n", iterations);
    return g_failures ? 1 : 0;
}
