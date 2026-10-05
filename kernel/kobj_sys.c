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
#include "kernel/mem_domain.h"
#include "kernel/random.h"
#include "kernel/identity.h"
#include "kernel/radio_intr.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "arch/umode.h"
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

/* ---- threads in the caller's domain -------------------------------------- *
 *
 * The radio is several tasks sharing one memory domain: the blob creates its own
 * (the pp task), and the shim adds a timer thread and an interrupt thread. A
 * thread is a kernel task whose body drops straight into U-mode at the entry
 * point it was given, under the *same* domain object as its creator -- so a
 * stack and a heap it shares with its siblings, and nothing of the kernel's.
 * Nothing about the new task is taken on trust: the entry and the stack are
 * checked against the creator's own regions before the task exists. */

#define KTHREAD_MAX 8
typedef struct {
    bool          used;
    mem_domain_t *domain;
    uintptr_t     entry, arg, stack_top;
} kthread_t;
static kthread_t g_thr[KTHREAD_MAX];

static void kthread_body(void *argp) {
    kthread_t t = *(kthread_t *)argp;       /* copied: the slot is free to be reused at once */
    ((kthread_t *)argp)->used = false;
    if (task_set_domain(sched_current_pid(), t.domain) != 0) {
        printk("[kobj] A thread refused to enter U-mode: its domain is not enforceable\n");
        return;
    }
    arch_enter_user((void (*)(void))t.entry, t.stack_top, 0, t.arg, 0);
}

static int tier_for(uintptr_t prio) {
    if (prio >= 20) return TASK_PRIO_INTERRUPT;
    if (prio >= 10) return TASK_PRIO_NORMAL;
    return TASK_PRIO_BACKGROUND;
}

static long thread_create(uintptr_t entry, uintptr_t arg, uintptr_t stack_base,
                          uintptr_t stack_size, uintptr_t prio) {
    mem_domain_t *dom = sched_current_domain();
    if (!dom) return KO_FAIL;               /* a kernel task has no domain to share */
    if (stack_size < 256 || (stack_base & 7u) || (stack_size & 7u)) return KO_FAIL;
    if (!mem_domain_permits(dom, entry, 2, MEM_X)) return KO_FAIL;
    if (!mem_domain_permits(dom, stack_base, stack_size, MEM_R | MEM_W)) return KO_FAIL;

    int slot = -1;
    for (int i = 0; i < KTHREAD_MAX; i++)
        if (!g_thr[i].used) { slot = i; break; }
    if (slot < 0) return KO_FAIL;
    g_thr[slot] = (kthread_t){ true, dom, entry, arg, stack_base + stack_size };
    int pid = task_create("uthread", kthread_body, &g_thr[slot]);
    if (pid < 0) { g_thr[slot].used = false; return KO_FAIL; }
    task_set_priority(pid, tier_for(prio));
    return pid;
}

/* A handle that is not this caller's behaves exactly like one that does not
 * exist. */
#define OWN(h) do { if (!kobj_owned_by((kh_t)(h), owner)) return KO_FAIL; } while (0)

/* The 45.2 trace, from the kernel's side: every call the radio makes into the
 * kernel-object seam, as it makes it. Everything the shim does that matters to
 * the blob's control flow passes through here, so this is the whole picture of
 * what the blob is waiting for without a debugger. Off unless `radio trace on`;
 * the quiet calls (clock, critical sections, log lines) are never traced. */
volatile int g_kobj_trace;
static const char *const k_opnames[KOBJ_OP_COUNT] = {
    "sem_create", "sem_take", "sem_give", "sem_delete", "mutex_create", "mutex_lock", "mutex_unlock",
    "mutex_delete", "q_create", "q_send", "q_recv", "q_waiting", "q_delete", "ev_create", "ev_set",
    "ev_clear", "ev_wait", "ev_delete", "timer_setfn", "timer_arm", "timer_disarm", "timer_done",
    "timer_wait", "time_us", "crit_enter", "crit_leave", "thread_create", "thread_self", "random", "mac",
    "log", "intr_set", "intr_clear", "isr_set", "ints_on", "ints_off", "event_post", "isr_wait", "isr_done",
};

static long kobj_syscall_impl(unsigned op, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);

long kobj_syscall(unsigned op, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                  uintptr_t a4, uintptr_t a5) {
    const bool tr = g_kobj_trace && op < KOBJ_OP_COUNT && op != KOBJ_OP_TIME_US && op != KOBJ_OP_CRIT_ENTER
                    && op != KOBJ_OP_CRIT_LEAVE && op != KOBJ_OP_LOG && op != KOBJ_OP_THREAD_SELF;
    int pid = tr ? sched_current_pid() : 0;
    if (tr)
        printk("[ktrace] pid%d %s(%lx,%lx,%lx,%lx) ...\n", pid, k_opnames[op],
               (unsigned long)a1, (unsigned long)a2, (unsigned long)a3, (unsigned long)a4);
    long r = kobj_syscall_impl(op, a1, a2, a3, a4, a5);
    if (tr) printk("[ktrace] pid%d %s -> %ld\n", pid, k_opnames[op], r);
    return r;
}

static long kobj_syscall_impl(unsigned op, uintptr_t a1, uintptr_t a2, uintptr_t a3,
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
    case KOBJ_OP_THREAD_CREATE: return thread_create(a1, a2, a3, a4, a5);
    case KOBJ_OP_THREAD_SELF:   return sched_current_pid();
    case KOBJ_OP_RANDOM: {
        uint32_t n = (uint32_t)a2;
        if (n > sizeof item) n = sizeof item;           /* a long request is served in pieces by the caller */
        random_bytes(item, n);
        return copy_to_user(a1, item, n) < 0 ? KO_FAIL : (long)n;
    }
    case KOBJ_OP_MAC: {
        uint8_t mac[6];
        const uint8_t *base = node_mac();
        for (int i = 0; i < 6; i++) mac[i] = base[i];
        mac[5] = (uint8_t)(mac[5] + (uint8_t)(a1 == 0 ? 0 : a1 == 1 ? 1 : a1 == 2 ? 2 : 3));
        return copy_to_user(a2, mac, 6) < 0 ? KO_FAIL : KO_OK;
    }
    case KOBJ_OP_LOG: {
        char line[160];
        uint32_t n = (uint32_t)a3;
        if (n >= sizeof line) n = sizeof line - 1;
        if (copy_from_user(line, a2, n) < 0) return KO_FAIL;
        line[n] = 0;
        if (g_kobj_trace) cprintf("[radio%u] %s\n", (unsigned)a1, line);   /* direct: survives a hang */
        else printk("[radio%u] %s\n", (unsigned)a1, line);
        return KO_OK;
    }
#if defined(CONFIG_BOARD_ESP32C6) && defined(CONFIG_RADIO_C6)
    case KOBJ_OP_INTR_SET:   return radio_intr_set((uint32_t)a2, (uint32_t)a3);
    case KOBJ_OP_INTR_CLEAR: return radio_intr_clear((uint32_t)a1, (uint32_t)a2);
    case KOBJ_OP_ISR_SET:    return radio_isr_set((uint32_t)a1, a2, a3);
    case KOBJ_OP_INTS_ON:    return radio_ints_on((uint32_t)a1);
    case KOBJ_OP_INTS_OFF:   return radio_ints_off((uint32_t)a1);
    case KOBJ_OP_ISR_WAIT:   return radio_isr_wait(a1);
    case KOBJ_OP_ISR_DONE:   return radio_isr_done((uint32_t)a1);
#endif
    default:
        return KO_FAIL;
    }
}
