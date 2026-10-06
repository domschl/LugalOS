#ifndef LUGALOS_KERNEL_KOBJ_H
#define LUGALOS_KERNEL_KOBJ_H

#include <stdint.h>
#include <stdbool.h>
#include "kernel/kobj_abi.h"

/* Kernel-owned synchronisation objects for a confined runtime (45.3b,
 * plan/phase45_esp32c6.md §4.5).
 *
 * The Espressif Wi-Fi blob reaches its operating system through a table of
 * ~127 function pointers: counting semaphores, recursive mutexes, queues,
 * event groups, one-shot and periodic timers. This kernel has none of those --
 * it has block/unblock-by-pid, ylocks and synchronous channels -- and the
 * radio runs in U-mode (45.3a), so whatever implements them must also be
 * somewhere the blob cannot scribble on. A semaphore whose count lives in the
 * blob's own memory is a semaphore a stray store can corrupt into a deadlock;
 * one in a kernel table is reachable only through a handle.
 *
 * ## Core and glue, and why they are two files
 *
 * This file's implementation (kernel/kobj.c) is a set of state machines. It
 * never blocks, never calls the scheduler, and never reads a clock: an
 * operation that cannot complete *queues a caller-owned waiter* and says so,
 * and an operation that completes someone else's wait *hands the result to
 * that waiter and names it*, for the caller to wake. The blocking, the
 * timeouts and the locking are the glue's (kernel/kobj_sched.c), which is the
 * only place that knows about tasks.
 *
 * That split is what makes the semantics testable: tests/host/kobj_host.c
 * drives these functions with scripted "tasks" -- no threads, no timing -- and
 * checks FIFO order, direct hand-off, cancellation on timeout, and what
 * deleting an object under its waiters does, under ASan/UBSan.
 *
 * ## Direct hand-off
 *
 * A give that finds a waiter does not increment a count a third task could
 * steal before the waiter runs: the unit (or the mutex, or the item, or the
 * satisfied event bits) goes to the waiter, which is marked KW_GRANT. A woken
 * waiter therefore never has to re-test, and FIFO order is the order granted.
 * There is no priority inheritance: the scheduler has none, and the sections
 * the blob guards are short.
 *
 * ## Handles
 *
 * `kh_t` packs a type, a generation and a slot index, so a stale handle (an
 * object deleted and its slot reused) and a forged one (a blob handing back a
 * pointer-sized value that was never issued) are both refused rather than
 * operating on whatever now lives there. 0 is never valid.
 *
 * ## Not here
 *
 * Time. Timeouts are the glue's; the timer table below stores deadlines it
 * is *given* and answers "what is due at `now`".
 */

typedef uint32_t kh_t;

/* Results: KO_OK, KO_BLOCK (not an error: the waiter was queued), KO_AGAIN, KO_FULL,
 * KO_FAIL, KO_TIMEOUT, KO_DELETED -- defined in kobj_abi.h, because U-mode code
 * receives them too. */

/* A waiter lives in the *caller's* storage (the blocked task's kernel stack)
 * for exactly as long as it is queued. */
#define KW_IDLE   0
#define KW_WAIT   1
#define KW_GRANT  2
#define KW_FAILED 3      /* object deleted while queued */

typedef struct kwaiter {
    struct kwaiter *next;
    int      who;        /* opaque to this file: the caller's pid */
    uint8_t  state;
    /* queue receive: where a handed-off item is copied (kernel memory). */
    void    *item;
    /* event group: what the waiter wants, and what it saw when granted. */
    uint32_t want, got;
    uint8_t  all, clear;
} kwaiter_t;

/* Who to wake after an operation. Capacity covers every task the kernel can
 * have (MAX_TASKS), because deleting an object wakes every waiter on it. */
#define KWAKE_MAX 32
typedef struct {
    int who[KWAKE_MAX];
    int n;
} kwake_t;

static inline void kwake_init(kwake_t *k) { k->n = 0; }

/* This module allocates nothing. A queue's storage is the caller's: handed in
 * at creation, handed back at deletion, so that no allocator is ever reached
 * from under the lock that serialises these state machines (spinlock_t is a
 * leaf; the kernel's lock checker faults on a spinlock held across palloc,
 * and did, the first time this was written the other way). */
void kobj_init(void);
/* Forgets every object. `release`, if given, is called with the storage of each
 * live queue first (the host harness frees its heap there). */
void kobj_reset(void (*release)(void *storage));

/* Table capacities, i.e. how many of each the radio may hold at once. */
#define KSEM_MAX    32
#define KMUTEX_MAX  32
#define KQUEUE_MAX  16
#define KEVT_MAX     8
#define KTIMER_MAX  64
#define KQUEUE_MAX_ITEM  64u     /* bytes per queue item */
#define KQUEUE_MAX_LEN   256u    /* items per queue */

/* ---- counting semaphores (binary: max 1) -------------------------------- */

kh_t ksem_create(uint32_t max, uint32_t init);
/* `w` NULL means "poll": KO_AGAIN instead of queueing. */
int  ksem_take(kh_t h, kwaiter_t *w, int who);
int  ksem_give(kh_t h, kwake_t *wk);
/* Ends a wait the glue is abandoning (timeout): KO_OK if it had in fact been
 * granted in the meantime (the caller now owns the unit), else removes it and
 * returns KO_TIMEOUT. Safe after the object is deleted. */
int  ksem_cancel(kh_t h, kwaiter_t *w);
int  ksem_delete(kh_t h, kwake_t *wk);
uint32_t ksem_count(kh_t h);

/* ---- mutexes, optionally recursive -------------------------------------- */

kh_t kmutex_create(bool recursive);
int  kmutex_lock(kh_t h, kwaiter_t *w, int who);
/* Fails (KO_FAIL) unless `who` owns it. Passes ownership to the first waiter. */
int  kmutex_unlock(kh_t h, int who, kwake_t *wk);
int  kmutex_cancel(kh_t h, kwaiter_t *w);
int  kmutex_delete(kh_t h, kwake_t *wk);
int  kmutex_owner(kh_t h);                 /* -1 when free or invalid */

/* ---- queues of fixed-size items ----------------------------------------- */

/* `storage` is len * item_size bytes the queue owns until kq_delete() returns
 * it. Returns 0 if an argument is bad or the table is full; the caller then
 * still owns `storage`. */
kh_t kq_create(uint32_t len, uint32_t item_size, void *storage);
/* `front` pushes ahead of everything queued. A send that finds a waiting
 * receiver copies straight into its `item` and grants it. */
int  kq_send(kh_t h, const void *item, bool front, kwaiter_t *w, int who, kwake_t *wk);
int  kq_recv(kh_t h, void *item, kwaiter_t *w, int who, kwake_t *wk);
int  kq_cancel(kh_t h, kwaiter_t *w);
uint32_t kq_waiting(kh_t h);               /* items queued */
/* Hands the storage back through *storage (and its size in *bytes) for the
 * caller to free once it has left its critical section. */
int  kq_delete(kh_t h, kwake_t *wk, void **storage, uint32_t *bytes);

/* ---- event groups -------------------------------------------------------- */

kh_t kev_create(void);
uint32_t kev_set(kh_t h, uint32_t bits, kwake_t *wk);     /* returns the value after waking */
uint32_t kev_clear(kh_t h, uint32_t bits);                /* returns the value before clearing */
/* Satisfied at once: KO_OK with *out = bits seen (cleared if `clear`).
 * Otherwise queues `w` (KO_BLOCK) or, with w == NULL, KO_AGAIN. */
int  kev_wait(kh_t h, uint32_t want, bool all, bool clear, uint32_t *out,
              kwaiter_t *w, int who);
int  kev_cancel(kh_t h, kwaiter_t *w, uint32_t *out);
int  kev_delete(kh_t h, kwake_t *wk);

/* ---- timers --------------------------------------------------------------
 *
 * Keyed by (owner, address): the address is one the runtime owns (the blob's
 * own ETSTimer), and the owner is its domain, so two runtimes cannot see each
 * other's timers or be handed each other's callbacks. The table stores that key, the callback and its argument, and a deadline, and never
 * dereferences any of them. Calling the callback is the U-mode timer thread's
 * job (kernel/kobj_sched.c hands it expired entries).
 *
 * Times are microseconds on the kernel's monotonic clock. A periodic timer
 * that has fallen behind fires once and reschedules from `now`: skipped
 * periods are dropped, not queued as a burst. */

int  ktimer_arm(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg,
                uint64_t now_us, uint64_t delay_us, bool periodic);
/* Sets the callback without arming. Creates the slot. */
int  ktimer_setfn(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg);
int  ktimer_disarm(uintptr_t owner, uintptr_t key);          /* keeps the slot */
int  ktimer_done(uintptr_t owner, uintptr_t key);            /* frees it */
bool ktimer_armed(uintptr_t owner, uintptr_t key);
/* Pops one expired timer (earliest first). Returns true and fills fn/arg/key,
 * or false if none is due. */
bool ktimer_pop_due(uintptr_t owner, uint64_t now_us, uintptr_t *key, uintptr_t *fn, uintptr_t *arg);
/* The earliest armed deadline, or false if none. */
bool ktimer_next(uintptr_t owner, uint64_t *deadline_us);
uint32_t ktimer_count(void);

/* ---- ownership -----------------------------------------------------------
 *
 * Every object, and every timer, belongs to whatever created it: the syscall
 * layer passes the calling task's memory domain as the owner and refuses a
 * handle that is not the caller's. Without that, any U-mode task could
 * operate on the radio's semaphores by guessing a handle -- and handles are
 * small, structured numbers. Kernel-internal callers use owner 0. */
int  kobj_set_owner(kh_t h, uintptr_t owner);
bool kobj_owned_by(kh_t h, uintptr_t owner);
/* Every live semaphore, mutex, queue and event group `owner` holds (at most `cap`, into `out`), and
 * how many. Owner 0 -- the kernel's own -- always answers none. For releasing what a finished domain
 * left behind (kos_release_owner()). */
uint32_t kobj_owned(uintptr_t owner, kh_t *out, uint32_t cap);
/* Frees every timer slot `owner` holds; returns how many. Owner 0 is refused (0). */
uint32_t ktimer_release_owner(uintptr_t owner);
/* Does `h` name a live object of any type? */
bool kobj_valid(kh_t h);
/* How many bytes one queue item is, so the syscall layer can size its copy. */
uint32_t kq_item_size(kh_t h);

/* Diagnostics for `ps`-style reporting and the tests. */
uint32_t kobj_live(int type);
#define KOBJ_SEM 1
#define KOBJ_MUTEX 2
#define KOBJ_QUEUE 3
#define KOBJ_EVENT 4

#endif /* LUGALOS_KERNEL_KOBJ_H */
