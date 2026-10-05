#ifndef LUGALOS_KERNEL_KOBJ_SCHED_H
#define LUGALOS_KERNEL_KOBJ_SCHED_H

#include "kernel/kobj.h"

/* The blocking face of kernel/kobj.h: the same objects, with the waiting,
 * timeouts and wake-ups the pure state machines leave to the scheduler
 * (45.3b, plan/phase45_esp32c6.md). Each call is made on behalf of the
 * *calling task*; the syscall layer wraps these for U-mode.
 *
 * Timeouts are in milliseconds: 0 polls, KOS_FOREVER waits without limit.
 * Results are the KO_* codes of kobj.h; KO_TIMEOUT means the wait expired,
 * KO_AGAIN that a poll found nothing, KO_DELETED that the object was deleted
 * while the caller waited.
 *
 * Every wait is a loop of prepare / test / block (kernel/sched.h), so a give
 * that lands before the waiter has gone to sleep is not lost.
 */

#define KOS_FOREVER 0xffffffffu

/* One-time setup, after the page allocator is up. Idempotent. */
void kos_init(void);

kh_t kos_sem_create(uint32_t max, uint32_t init, uintptr_t owner);
int  kos_sem_take(kh_t h, uint32_t timeout_ms);
int  kos_sem_give(kh_t h);
int  kos_sem_delete(kh_t h);

kh_t kos_mutex_create(bool recursive, uintptr_t owner);
int  kos_mutex_lock(kh_t h, uint32_t timeout_ms);
int  kos_mutex_unlock(kh_t h);
int  kos_mutex_delete(kh_t h);

kh_t kos_q_create(uint32_t len, uint32_t item_size, uintptr_t owner);
int  kos_q_send(kh_t h, const void *item, bool front, uint32_t timeout_ms);
int  kos_q_recv(kh_t h, void *item, uint32_t timeout_ms);
int  kos_q_delete(kh_t h);

kh_t kos_ev_create(uintptr_t owner);
int  kos_ev_set(kh_t h, uint32_t bits, uint32_t *after);
int  kos_ev_clear(kh_t h, uint32_t bits, uint32_t *before);
int  kos_ev_wait(kh_t h, uint32_t want, bool all, bool clear, uint32_t timeout_ms, uint32_t *out);
int  kos_ev_delete(kh_t h);

/* Timers, in microseconds from now. The callback is *not* run here: the
 * owner's timer thread collects expired ones with kos_timer_wait() and calls
 * them itself, in its own domain. */
int  kos_timer_setfn(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg);
int  kos_timer_arm(uintptr_t owner, uintptr_t key, uintptr_t fn, uintptr_t arg,
                   uint64_t delay_us, bool periodic);
int  kos_timer_disarm(uintptr_t owner, uintptr_t key);
int  kos_timer_done(uintptr_t owner, uintptr_t key);
/* Blocks until one of `owner`'s timers is due (KO_OK, fields filled) or
 * `timeout_ms` passes (KO_TIMEOUT). Arming or disarming wakes it to
 * recompute. One timer thread per owner. */
int  kos_timer_wait(uintptr_t owner, uintptr_t *key, uintptr_t *fn, uintptr_t *arg,
                    uint32_t timeout_ms);

/* `kobjselftest` (kernel/kobj_selftest.c): the primitives under real tasks,
 * real preemption and real timeouts. Prints KOBJSELFTEST_OK or _FAIL. */
int kobj_selftest(void);

#endif /* LUGALOS_KERNEL_KOBJ_SCHED_H */
