/* The syscall surface of the kernel objects. See kernel/include/kernel/kobj_abi.h.
 * 45.3b, plan/phase45_esp32c6.md.
 *
 * Three rules, each of which is what makes this a boundary and not a hole:
 *
 *   1. A handle is honoured only for the domain that created it. The owner is
 *      the caller's memory domain, so a second U-mode task cannot operate on
 *      the radio's semaphores by guessing a number.
 *   2. The kernel never dereferences a user address: items, bit results and
 *      timer records are copied through kernel buffers, validated against the
 *      caller's domain by uaccess.
 *   3. Nothing the caller passes is trusted as a size: a queue item is exactly
 *      the item size the queue was created with, which the kernel remembers.
 */

#include "kernel/kobj_sys.h"
#include "kernel/kobj_sched.h"
#include "kernel/sched.h"
#include "kernel/uaccess.h"
#include "kernel/time.h"
#include <stddef.h>

static inline uintptr_t caller_domain(void) {
    return (uintptr_t)sched_current_domain();
}

/* One recursive mutex per domain backs KOBJ_OP_CRIT_ENTER/LEAVE: the blob's
 * critical sections ("disable interrupts around this") become mutual
 * exclusion among the domain's own threads. The hardware interrupt itself
 * only ever runs a kernel stub; the handler logic runs in a thread of the
 * domain (plan/phase45 §4.5), so excluding that thread is excluding the
 * interrupt. Few domains use it, so a tiny table is enough. */
#define CRIT_MAX 4
static struct { uintptr_t owner; kh_t m; } g_crit[CRIT_MAX];

static kh_t crit_mutex(uintptr_t owner, bool create) {
    for (int i = 0; i < CRIT_MAX; i++)
        if (g_crit[i].m && g_crit[i].owner == owner) return g_crit[i].m;
    if (!create) return 0;
    for (int i = 0; i < CRIT_MAX; i++) {
        if (g_crit[i].m) continue;
        kh_t m = kos_mutex_create(true, owner);
        if (!m) return 0;
        g_crit[i].owner = owner; g_crit[i].m = m;
        return m;
    }
    return 0;
}

/* A handle that is not this caller's behaves exactly like one that does not
 * exist. */
#define OWN(h) do { if (!kobj_owned_by((kh_t)(h), owner)) return KO_FAIL; } while (0)

long kobj_syscall(unsigned op, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                  uintptr_t a4, uintptr_t a5) {
    kos_init();
    const uintptr_t owner = caller_domain();
    uint8_t item[KQUEUE_MAX_ITEM];

    switch (op) {
    case KOBJ_OP_SEM_CREATE:   return (long)kos_sem_create((uint32_t)a1, (uint32_t)a2, owner);
    case KOBJ_OP_SEM_TAKE:     OWN(a1); return kos_sem_take((kh_t)a1, (uint32_t)a2);
    case KOBJ_OP_SEM_GIVE:     OWN(a1); return kos_sem_give((kh_t)a1);
    case KOBJ_OP_SEM_DELETE:   OWN(a1); return kos_sem_delete((kh_t)a1);

    case KOBJ_OP_MUTEX_CREATE: return (long)kos_mutex_create(a1 != 0, owner);
    case KOBJ_OP_MUTEX_LOCK:   OWN(a1); return kos_mutex_lock((kh_t)a1, (uint32_t)a2);
    case KOBJ_OP_MUTEX_UNLOCK: OWN(a1); return kos_mutex_unlock((kh_t)a1);
    case KOBJ_OP_MUTEX_DELETE: OWN(a1); return kos_mutex_delete((kh_t)a1);

    case KOBJ_OP_Q_CREATE:     return (long)kos_q_create((uint32_t)a1, (uint32_t)a2, owner);
    case KOBJ_OP_Q_SEND: {
        OWN(a1);
        uint32_t n = kq_item_size((kh_t)a1);
        if (n == 0 || n > sizeof item) return KO_FAIL;
        if (copy_from_user(item, a2, n) < 0) return KO_FAIL;
        return kos_q_send((kh_t)a1, item, a3 != 0, (uint32_t)a4);
    }
    case KOBJ_OP_Q_RECV: {
        OWN(a1);
        uint32_t n = kq_item_size((kh_t)a1);
        if (n == 0 || n > sizeof item) return KO_FAIL;
        int r = kos_q_recv((kh_t)a1, item, (uint32_t)a3);
        if (r != KO_OK) return r;
        return copy_to_user(a2, item, n) < 0 ? KO_FAIL : KO_OK;
    }
    case KOBJ_OP_Q_WAITING:    OWN(a1); return (long)kq_waiting((kh_t)a1);
    case KOBJ_OP_Q_DELETE:     OWN(a1); return kos_q_delete((kh_t)a1);

    case KOBJ_OP_EV_CREATE:    return (long)kos_ev_create(owner);
    case KOBJ_OP_EV_SET: {
        OWN(a1);
        uint32_t after = 0;
        kos_ev_set((kh_t)a1, (uint32_t)a2, &after);
        return (long)after;
    }
    case KOBJ_OP_EV_CLEAR: {
        OWN(a1);
        uint32_t before = 0;
        kos_ev_clear((kh_t)a1, (uint32_t)a2, &before);
        return (long)before;
    }
    case KOBJ_OP_EV_WAIT: {
        OWN(a1);
        uint32_t bits = 0;
        int r = kos_ev_wait((kh_t)a1, (uint32_t)a2, (a3 & KOBJ_EV_ALL) != 0, (a3 & KOBJ_EV_CLEAR) != 0,
                            (uint32_t)a4, &bits);
        if (a5 && (r == KO_OK || r == KO_TIMEOUT || r == KO_AGAIN) &&
            copy_to_user(a5, &bits, sizeof bits) < 0) return KO_FAIL;
        return r;
    }
    case KOBJ_OP_EV_DELETE:    OWN(a1); return kos_ev_delete((kh_t)a1);

    case KOBJ_OP_TIMER_SETFN:  return kos_timer_setfn(owner, a1, a2, a3);
    case KOBJ_OP_TIMER_ARM:    return kos_timer_arm(owner, a1, a2, a3, (uint64_t)(uint32_t)a4, a5 != 0);
    case KOBJ_OP_TIMER_DISARM: return kos_timer_disarm(owner, a1);
    case KOBJ_OP_TIMER_DONE:   return kos_timer_done(owner, a1);
    case KOBJ_OP_TIMER_WAIT: {
        uintptr_t rec[3];
        int r = kos_timer_wait(owner, &rec[0], &rec[1], &rec[2], (uint32_t)a2);
        if (r != KO_OK) return r;
        return copy_to_user(a1, rec, sizeof rec) < 0 ? KO_FAIL : KO_OK;
    }
    case KOBJ_OP_TIME_US: {
        uint64_t t = time_get_us();
        return copy_to_user(a1, &t, sizeof t) < 0 ? KO_FAIL : KO_OK;
    }
    case KOBJ_OP_CRIT_ENTER: {
        kh_t m = crit_mutex(owner, true);
        return m ? kos_mutex_lock(m, KOS_FOREVER) : KO_FAIL;
    }
    case KOBJ_OP_CRIT_LEAVE: {
        kh_t m = crit_mutex(owner, false);
        return m ? kos_mutex_unlock(m) : KO_FAIL;
    }
    default:
        return KO_FAIL;
    }
}
