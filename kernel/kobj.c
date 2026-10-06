/* Kernel-owned synchronisation objects: the state machines.
 * See kernel/include/kernel/kobj.h for the contract and why this file never
 * blocks, never touches the scheduler and never reads a clock. 45.3b,
 * plan/phase45_esp32c6.md. */

#include "kernel/kobj.h"
#include <stddef.h>
#include <string.h>

/* ---- waiter lists (FIFO) ------------------------------------------------- */

typedef struct { kwaiter_t *head, *tail; } wlist_t;

static void wl_push(wlist_t *l, kwaiter_t *w) {
    w->next = NULL;
    w->state = KW_WAIT;
    if (l->tail) l->tail->next = w; else l->head = w;
    l->tail = w;
}

static kwaiter_t *wl_pop(wlist_t *l) {
    kwaiter_t *w = l->head;
    if (!w) return NULL;
    l->head = w->next;
    if (!l->head) l->tail = NULL;
    w->next = NULL;
    return w;
}

static bool wl_remove(wlist_t *l, kwaiter_t *w) {
    kwaiter_t *prev = NULL;
    for (kwaiter_t *c = l->head; c; prev = c, c = c->next) {
        if (c != w) continue;
        if (prev) prev->next = c->next; else l->head = c->next;
        if (l->tail == c) l->tail = prev;
        c->next = NULL;
        return true;
    }
    return false;
}

static void kw_add(kwake_t *wk, int who) {
    if (wk && wk->n < KWAKE_MAX) wk->who[wk->n++] = who;
}

/* Fails every queued waiter: the object is going away under them. */
static void wl_fail_all(wlist_t *l, kwake_t *wk) {
    kwaiter_t *w;
    while ((w = wl_pop(l)) != NULL) {
        w->state = KW_FAILED;
        kw_add(wk, w->who);
    }
}

/* ---- handles ------------------------------------------------------------- */

/* type:4 | generation:12 | slot+1:16. A deleted slot's generation moves on, so
 * a stale handle names nothing. */
#define H_MAKE(type, gen, idx) \
    ((kh_t)(((uint32_t)(type) << 28) | (((uint32_t)(gen) & 0xfffu) << 16) | ((uint32_t)(idx) + 1u)))
#define H_TYPE(h) ((int)((h) >> 28))
#define H_GEN(h)  ((uint16_t)(((h) >> 16) & 0xfffu))
#define H_IDX(h)  ((int)((h) & 0xffffu) - 1)

/* ---- tables -------------------------------------------------------------- */

typedef struct { bool used; uint16_t gen; uintptr_t owner; uint32_t count, max; wlist_t w; } sem_t_;
typedef struct { bool used; uint16_t gen; uintptr_t dom; bool rec; int owner; uint32_t depth; wlist_t w; } mutex_t_;
typedef struct {
    bool used; uint16_t gen; uintptr_t owner;
    uint32_t len, isz, head, count;
    uint8_t *buf;
    wlist_t rx, tx;
} queue_t_;
typedef struct { bool used; uint16_t gen; uintptr_t owner; uint32_t bits; wlist_t w; } evt_t_;
typedef struct {
    bool used, armed, periodic;
    uintptr_t owner, key, fn, arg;
    uint64_t deadline, period, seq;
} timer_t_;

static sem_t_   g_sem[KSEM_MAX];
static mutex_t_ g_mtx[KMUTEX_MAX];
static queue_t_ g_q[KQUEUE_MAX];
static evt_t_   g_ev[KEVT_MAX];
static timer_t_ g_tm[KTIMER_MAX];
static uint64_t g_tm_seq;

void kobj_reset(void (*release)(void *storage)) {
    for (int i = 0; i < KQUEUE_MAX; i++)
        if (g_q[i].used && g_q[i].buf && release) release(g_q[i].buf);
    memset(g_sem, 0, sizeof g_sem);
    memset(g_mtx, 0, sizeof g_mtx);
    memset(g_ev, 0, sizeof g_ev);
    memset(g_tm, 0, sizeof g_tm);
    memset(g_q, 0, sizeof g_q);
    g_tm_seq = 0;
}

void kobj_init(void) { kobj_reset(NULL); }

uint32_t kobj_live(int type) {
    uint32_t n = 0;
    switch (type) {
    case KOBJ_SEM:   for (int i = 0; i < KSEM_MAX; i++)   n += g_sem[i].used; break;
    case KOBJ_MUTEX: for (int i = 0; i < KMUTEX_MAX; i++) n += g_mtx[i].used; break;
    case KOBJ_QUEUE: for (int i = 0; i < KQUEUE_MAX; i++) n += g_q[i].used; break;
    case KOBJ_EVENT: for (int i = 0; i < KEVT_MAX; i++)   n += g_ev[i].used; break;
    }
    return n;
}

#define LOOKUP(arr, N, T, h, out)                                        \
    do {                                                                  \
        int _i = H_IDX(h);                                                \
        if ((h) == 0 || H_TYPE(h) != (T) || _i < 0 || _i >= (N) ||        \
            !(arr)[_i].used || (arr)[_i].gen != H_GEN(h)) return KO_FAIL; \
        (out) = &(arr)[_i];                                               \
    } while (0)

/* A variant for functions whose failure value is not KO_FAIL. */
#define LOOKUP_OR(arr, N, T, h, out, fail)                               \
    do {                                                                  \
        int _i = H_IDX(h);                                                \
        if ((h) == 0 || H_TYPE(h) != (T) || _i < 0 || _i >= (N) ||        \
            !(arr)[_i].used || (arr)[_i].gen != H_GEN(h)) return (fail);  \
        (out) = &(arr)[_i];                                               \
    } while (0)

/* The result of cancelling a wait, common to every object type: what the
 * waiter's own state says, once we know it is not granted. */
static int cancel_finish(wlist_t *l, kwaiter_t *w, bool found_obj) {
    if (w->state == KW_GRANT) return KO_OK;
    if (w->state == KW_FAILED) return KO_DELETED;
    if (!found_obj) { w->state = KW_FAILED; return KO_DELETED; }
    wl_remove(l, w);
    w->state = KW_IDLE;
    return KO_TIMEOUT;
}

/* ---- ownership ----------------------------------------------------------- */

static uintptr_t *owner_slot(kh_t h) {
    int i = H_IDX(h);
    if (h == 0 || i < 0) return NULL;
    switch (H_TYPE(h)) {
    case KOBJ_SEM:   if (i < KSEM_MAX   && g_sem[i].used && g_sem[i].gen == H_GEN(h)) return &g_sem[i].owner; break;
    case KOBJ_MUTEX: if (i < KMUTEX_MAX && g_mtx[i].used && g_mtx[i].gen == H_GEN(h)) return &g_mtx[i].dom; break;
    case KOBJ_QUEUE: if (i < KQUEUE_MAX && g_q[i].used   && g_q[i].gen   == H_GEN(h)) return &g_q[i].owner; break;
    case KOBJ_EVENT: if (i < KEVT_MAX   && g_ev[i].used  && g_ev[i].gen  == H_GEN(h)) return &g_ev[i].owner; break;
    }
    return NULL;
}

uint32_t kobj_owned(uintptr_t owner, kh_t *out, uint32_t cap) {
    uint32_t n = 0;
    if (owner == 0) return 0;                       /* the kernel's own: never released this way */
#define COLLECT(arr, N, T, field) \
    for (int i = 0; i < (N); i++) \
        if ((arr)[i].used && (arr)[i].field == owner && n < cap) out[n++] = H_MAKE(T, (arr)[i].gen, i);
    COLLECT(g_sem, KSEM_MAX, KOBJ_SEM, owner)
    COLLECT(g_mtx, KMUTEX_MAX, KOBJ_MUTEX, dom)
    COLLECT(g_q, KQUEUE_MAX, KOBJ_QUEUE, owner)
    COLLECT(g_ev, KEVT_MAX, KOBJ_EVENT, owner)
#undef COLLECT
    return n;
}

uint32_t ktimer_release_owner(uintptr_t owner) {
    uint32_t n = 0;
    if (owner == 0) return 0;
    for (int i = 0; i < KTIMER_MAX; i++)
        if (g_tm[i].used && g_tm[i].owner == owner) { memset(&g_tm[i], 0, sizeof g_tm[i]); n++; }
    return n;
}

int kobj_set_owner(kh_t h, uintptr_t owner) {
    uintptr_t *o = owner_slot(h);
    if (!o) return KO_FAIL;
    *o = owner;
    return KO_OK;
}

bool kobj_valid(kh_t h) { return owner_slot(h) != NULL; }

bool kobj_owned_by(kh_t h, uintptr_t owner) {
    uintptr_t *o = owner_slot(h);
    return o && *o == owner;
}

uint32_t kq_item_size(kh_t h) {
    int i = H_IDX(h);
    if (h == 0 || H_TYPE(h) != KOBJ_QUEUE || i < 0 || i >= KQUEUE_MAX ||
        !g_q[i].used || g_q[i].gen != H_GEN(h)) return 0;
    return g_q[i].isz;
}

/* ---- semaphores ---------------------------------------------------------- */

kh_t ksem_create(uint32_t max, uint32_t init) {
    if (max == 0 || init > max) return 0;
    for (int i = 0; i < KSEM_MAX; i++) {
        if (g_sem[i].used) continue;
        sem_t_ *s = &g_sem[i];
        uint16_t gen = s->gen;
        memset(s, 0, sizeof *s);
        s->used = true; s->gen = gen; s->max = max; s->count = init;
        return H_MAKE(KOBJ_SEM, gen, i);
    }
    return 0;
}

int ksem_take(kh_t h, kwaiter_t *w, int who) {
    sem_t_ *s; LOOKUP(g_sem, KSEM_MAX, KOBJ_SEM, h, s);
    if (s->count > 0) { s->count--; return KO_OK; }
    if (!w) return KO_AGAIN;
    w->who = who;
    wl_push(&s->w, w);
    return KO_BLOCK;
}

int ksem_give(kh_t h, kwake_t *wk) {
    sem_t_ *s; LOOKUP(g_sem, KSEM_MAX, KOBJ_SEM, h, s);
    kwaiter_t *w = wl_pop(&s->w);
    if (w) { w->state = KW_GRANT; kw_add(wk, w->who); return KO_OK; }
    if (s->count >= s->max) return KO_FULL;
    s->count++;
    return KO_OK;
}

int ksem_cancel(kh_t h, kwaiter_t *w) {
    sem_t_ *s = NULL;
    int i = H_IDX(h);
    if (h != 0 && H_TYPE(h) == KOBJ_SEM && i >= 0 && i < KSEM_MAX &&
        g_sem[i].used && g_sem[i].gen == H_GEN(h)) s = &g_sem[i];
    return cancel_finish(s ? &s->w : NULL, w, s != NULL);
}

int ksem_delete(kh_t h, kwake_t *wk) {
    sem_t_ *s; LOOKUP(g_sem, KSEM_MAX, KOBJ_SEM, h, s);
    wl_fail_all(&s->w, wk);
    s->used = false; s->gen++;
    return KO_OK;
}

uint32_t ksem_count(kh_t h) {
    sem_t_ *s; LOOKUP_OR(g_sem, KSEM_MAX, KOBJ_SEM, h, s, 0);
    return s->count;
}

/* ---- mutexes ------------------------------------------------------------- */

kh_t kmutex_create(bool recursive) {
    for (int i = 0; i < KMUTEX_MAX; i++) {
        if (g_mtx[i].used) continue;
        mutex_t_ *m = &g_mtx[i];
        uint16_t gen = m->gen;
        memset(m, 0, sizeof *m);
        m->used = true; m->gen = gen; m->rec = recursive; m->owner = -1;
        return H_MAKE(KOBJ_MUTEX, gen, i);
    }
    return 0;
}

int kmutex_lock(kh_t h, kwaiter_t *w, int who) {
    mutex_t_ *m; LOOKUP(g_mtx, KMUTEX_MAX, KOBJ_MUTEX, h, m);
    if (m->owner < 0) { m->owner = who; m->depth = 1; return KO_OK; }
    if (m->owner == who) {
        /* A plain mutex locked again by its owner would wait for itself
         * forever. Refuse it loudly instead of hanging the radio. */
        if (!m->rec) return KO_FAIL;
        m->depth++;
        return KO_OK;
    }
    if (!w) return KO_AGAIN;
    w->who = who;
    wl_push(&m->w, w);
    return KO_BLOCK;
}

int kmutex_unlock(kh_t h, int who, kwake_t *wk) {
    mutex_t_ *m; LOOKUP(g_mtx, KMUTEX_MAX, KOBJ_MUTEX, h, m);
    if (m->owner != who) return KO_FAIL;
    if (--m->depth > 0) return KO_OK;
    kwaiter_t *w = wl_pop(&m->w);
    if (w) {
        m->owner = w->who; m->depth = 1;
        w->state = KW_GRANT; kw_add(wk, w->who);
    } else {
        m->owner = -1;
    }
    return KO_OK;
}

int kmutex_cancel(kh_t h, kwaiter_t *w) {
    mutex_t_ *m = NULL;
    int i = H_IDX(h);
    if (h != 0 && H_TYPE(h) == KOBJ_MUTEX && i >= 0 && i < KMUTEX_MAX &&
        g_mtx[i].used && g_mtx[i].gen == H_GEN(h)) m = &g_mtx[i];
    return cancel_finish(m ? &m->w : NULL, w, m != NULL);
}

int kmutex_delete(kh_t h, kwake_t *wk) {
    mutex_t_ *m; LOOKUP(g_mtx, KMUTEX_MAX, KOBJ_MUTEX, h, m);
    wl_fail_all(&m->w, wk);
    m->used = false; m->gen++;
    return KO_OK;
}

int kmutex_owner(kh_t h) {
    mutex_t_ *m; LOOKUP_OR(g_mtx, KMUTEX_MAX, KOBJ_MUTEX, h, m, -1);
    return m->owner;
}

/* ---- queues -------------------------------------------------------------- */

kh_t kq_create(uint32_t len, uint32_t item_size, void *storage) {
    if (len == 0 || len > KQUEUE_MAX_LEN || item_size == 0 || item_size > KQUEUE_MAX_ITEM) return 0;
    if (!storage) return 0;
    for (int i = 0; i < KQUEUE_MAX; i++) {
        if (g_q[i].used) continue;
        queue_t_ *q = &g_q[i];
        uint16_t gen = q->gen;
        memset(q, 0, sizeof *q);
        q->used = true; q->gen = gen; q->len = len; q->isz = item_size; q->buf = storage;
        return H_MAKE(KOBJ_QUEUE, gen, i);
    }
    return 0;
}

static uint8_t *q_slot(queue_t_ *q, uint32_t logical) {
    return q->buf + ((q->head + logical) % q->len) * q->isz;
}

static void q_push(queue_t_ *q, const void *item, bool front) {
    if (front) {
        q->head = (q->head + q->len - 1) % q->len;
        memcpy(q->buf + q->head * q->isz, item, q->isz);
    } else {
        memcpy(q_slot(q, q->count), item, q->isz);
    }
    q->count++;
}

int kq_send(kh_t h, const void *item, bool front, kwaiter_t *w, int who, kwake_t *wk) {
    queue_t_ *q; LOOKUP(g_q, KQUEUE_MAX, KOBJ_QUEUE, h, q);
    kwaiter_t *r = wl_pop(&q->rx);          /* a receiver implies an empty queue */
    if (r) {
        memcpy(r->item, item, q->isz);
        r->state = KW_GRANT; kw_add(wk, r->who);
        return KO_OK;
    }
    if (q->count < q->len) { q_push(q, item, front); return KO_OK; }
    if (!w) return KO_FULL;
    w->who = who;
    w->item = (void *)(uintptr_t)item;      /* read back when a slot frees; caller keeps it alive */
    w->all = front;
    wl_push(&q->tx, w);
    return KO_BLOCK;
}

int kq_recv(kh_t h, void *item, kwaiter_t *w, int who, kwake_t *wk) {
    queue_t_ *q; LOOKUP(g_q, KQUEUE_MAX, KOBJ_QUEUE, h, q);
    if (q->count > 0) {
        memcpy(item, q_slot(q, 0), q->isz);
        q->head = (q->head + 1) % q->len;
        q->count--;
        kwaiter_t *s = wl_pop(&q->tx);      /* a slot freed: admit the first blocked sender */
        if (s) {
            q_push(q, s->item, s->all != 0);
            s->state = KW_GRANT; kw_add(wk, s->who);
        }
        return KO_OK;
    }
    if (!w) return KO_AGAIN;
    w->who = who;
    w->item = item;
    wl_push(&q->rx, w);
    return KO_BLOCK;
}

int kq_cancel(kh_t h, kwaiter_t *w) {
    queue_t_ *q = NULL;
    int i = H_IDX(h);
    if (h != 0 && H_TYPE(h) == KOBJ_QUEUE && i >= 0 && i < KQUEUE_MAX &&
        g_q[i].used && g_q[i].gen == H_GEN(h)) q = &g_q[i];
    if (w->state == KW_GRANT) return KO_OK;
    if (w->state == KW_FAILED) return KO_DELETED;
    if (!q) { w->state = KW_FAILED; return KO_DELETED; }
    /* A waiter is on exactly one of the two lists. */
    if (!wl_remove(&q->rx, w)) wl_remove(&q->tx, w);
    w->state = KW_IDLE;
    return KO_TIMEOUT;
}

uint32_t kq_waiting(kh_t h) {
    queue_t_ *q; LOOKUP_OR(g_q, KQUEUE_MAX, KOBJ_QUEUE, h, q, 0);
    return q->count;
}

int kq_delete(kh_t h, kwake_t *wk, void **storage, uint32_t *bytes) {
    queue_t_ *q; LOOKUP(g_q, KQUEUE_MAX, KOBJ_QUEUE, h, q);
    wl_fail_all(&q->rx, wk);
    wl_fail_all(&q->tx, wk);
    if (storage) *storage = q->buf;
    if (bytes) *bytes = q->len * q->isz;
    q->buf = NULL;
    q->used = false; q->gen++;
    return KO_OK;
}

/* ---- event groups -------------------------------------------------------- */

kh_t kev_create(void) {
    for (int i = 0; i < KEVT_MAX; i++) {
        if (g_ev[i].used) continue;
        evt_t_ *e = &g_ev[i];
        uint16_t gen = e->gen;
        memset(e, 0, sizeof *e);
        e->used = true; e->gen = gen;
        return H_MAKE(KOBJ_EVENT, gen, i);
    }
    return 0;
}

static bool ev_satisfied(uint32_t bits, uint32_t want, bool all) {
    return all ? (bits & want) == want : (bits & want) != 0;
}

/* Grants every waiter the current bits satisfy, in arrival order. A waiter
 * that asked for the bits to be cleared clears them before the next one is
 * judged, which is FreeRTOS's behaviour. */
static void ev_scan(evt_t_ *e, kwake_t *wk) {
    kwaiter_t *prev = NULL, *w = e->w.head;
    while (w) {
        kwaiter_t *next = w->next;
        if (ev_satisfied(e->bits, w->want, w->all != 0)) {
            if (prev) prev->next = next; else e->w.head = next;
            if (e->w.tail == w) e->w.tail = prev;
            w->next = NULL;
            w->got = e->bits;
            if (w->clear) e->bits &= ~w->want;
            w->state = KW_GRANT; kw_add(wk, w->who);
        } else {
            prev = w;
        }
        w = next;
    }
}

uint32_t kev_set(kh_t h, uint32_t bits, kwake_t *wk) {
    evt_t_ *e; LOOKUP_OR(g_ev, KEVT_MAX, KOBJ_EVENT, h, e, 0);
    e->bits |= bits;
    ev_scan(e, wk);
    return e->bits;
}

uint32_t kev_clear(kh_t h, uint32_t bits) {
    evt_t_ *e; LOOKUP_OR(g_ev, KEVT_MAX, KOBJ_EVENT, h, e, 0);
    uint32_t before = e->bits;
    e->bits &= ~bits;
    return before;
}

int kev_wait(kh_t h, uint32_t want, bool all, bool clear, uint32_t *out,
             kwaiter_t *w, int who) {
    evt_t_ *e; LOOKUP(g_ev, KEVT_MAX, KOBJ_EVENT, h, e);
    if (want == 0) return KO_FAIL;
    if (ev_satisfied(e->bits, want, all)) {
        *out = e->bits;
        if (clear) e->bits &= ~want;
        return KO_OK;
    }
    if (!w) { *out = e->bits; return KO_AGAIN; }
    w->who = who; w->want = want; w->all = all; w->clear = clear; w->got = 0;
    wl_push(&e->w, w);
    return KO_BLOCK;
}

int kev_cancel(kh_t h, kwaiter_t *w, uint32_t *out) {
    evt_t_ *e = NULL;
    int i = H_IDX(h);
    if (h != 0 && H_TYPE(h) == KOBJ_EVENT && i >= 0 && i < KEVT_MAX &&
        g_ev[i].used && g_ev[i].gen == H_GEN(h)) e = &g_ev[i];
    int r = cancel_finish(e ? &e->w : NULL, w, e != NULL);
    if (r == KO_OK) *out = w->got;
    else if (r == KO_TIMEOUT && e) *out = e->bits;    /* FreeRTOS returns the bits as they stand */
    return r;
}

int kev_delete(kh_t h, kwake_t *wk) {
    evt_t_ *e; LOOKUP(g_ev, KEVT_MAX, KOBJ_EVENT, h, e);
    wl_fail_all(&e->w, wk);
    e->used = false; e->gen++;
    return KO_OK;
}

/* ---- timers -------------------------------------------------------------- */

static timer_t_ *tm_find(uintptr_t owner, uintptr_t key) {
    for (int i = 0; i < KTIMER_MAX; i++)
        if (g_tm[i].used && g_tm[i].key == key && g_tm[i].owner == owner) return &g_tm[i];
    return NULL;
}

static timer_t_ *tm_get(uintptr_t owner, uintptr_t key) {
    timer_t_ *t = tm_find(owner, key);
    if (t) return t;
    for (int i = 0; i < KTIMER_MAX; i++) {
        if (g_tm[i].used) continue;
        memset(&g_tm[i], 0, sizeof g_tm[i]);
        g_tm[i].used = true; g_tm[i].key = key; g_tm[i].owner = owner;
        return &g_tm[i];
    }
    return NULL;
}

int ktimer_setfn(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg) {
    timer_t_ *t = tm_get(owner, key);
    if (!t) return KO_FAIL;
    t->fn = fn; t->arg = arg;
    return KO_OK;
}

int ktimer_arm(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg,
               uint64_t now_us, uint64_t delay_us, bool periodic) {
    timer_t_ *t = tm_get(owner, key);
    if (!t) return KO_FAIL;
    if (fn) { t->fn = fn; t->arg = arg; }
    if (periodic && delay_us == 0) delay_us = 1;    /* a zero period would spin the thread */
    t->armed = true; t->periodic = periodic;
    t->deadline = now_us + delay_us;
    t->period = delay_us;
    t->seq = ++g_tm_seq;
    return KO_OK;
}

int ktimer_disarm(uintptr_t owner, uintptr_t key) {
    timer_t_ *t = tm_find(owner, key);
    if (!t) return KO_FAIL;
    t->armed = false;
    return KO_OK;
}

int ktimer_done(uintptr_t owner, uintptr_t key) {
    timer_t_ *t = tm_find(owner, key);
    if (!t) return KO_FAIL;
    memset(t, 0, sizeof *t);
    return KO_OK;
}

bool ktimer_armed(uintptr_t owner, uintptr_t key) {
    timer_t_ *t = tm_find(owner, key);
    return t && t->armed;
}

bool ktimer_pop_due(uintptr_t owner, uint64_t now_us, uintptr_t *key, uintptr_t *fn, uintptr_t *arg) {
    timer_t_ *best = NULL;
    for (int i = 0; i < KTIMER_MAX; i++) {
        timer_t_ *t = &g_tm[i];
        if (!t->used || !t->armed || t->owner != owner || t->deadline > now_us) continue;
        if (!best || t->deadline < best->deadline ||
            (t->deadline == best->deadline && t->seq < best->seq)) best = t;
    }
    if (!best) return false;
    *key = best->key; *fn = best->fn; *arg = best->arg;
    if (best->periodic) {
        uint64_t next = best->deadline + best->period;
        if (next <= now_us) next = now_us + best->period;    /* behind: drop the missed periods */
        best->deadline = next;
        best->seq = ++g_tm_seq;
    } else {
        best->armed = false;
    }
    return true;
}

bool ktimer_next(uintptr_t owner, uint64_t *deadline_us) {
    bool any = false;
    for (int i = 0; i < KTIMER_MAX; i++) {
        timer_t_ *t = &g_tm[i];
        if (!t->used || !t->armed || t->owner != owner) continue;
        if (!any || t->deadline < *deadline_us) { *deadline_us = t->deadline; any = true; }
    }
    return any;
}

uint32_t ktimer_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < KTIMER_MAX; i++) n += g_tm[i].used;
    return n;
}
