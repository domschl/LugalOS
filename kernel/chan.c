#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include "kernel/uaccess.h"
#include "kernel/time.h"
#include "kernel/devirq.h"
#include <string.h>

/* See kernel/include/kernel/chan.h for the rationale, especially why the
 * two copies below are deliberate rather than wasteful, and for the
 * task-owned endpoint design. */

struct chan_endpoint {
    const char     *name;
    /* Inline-handler endpoints (B1: p9, console): handler != NULL. */
    chan_handler_fn handler;
    void           *ctx;
    /* Task-owned endpoints: owner_pid >= 0, handler == NULL. */
    int             owner_pid;
    int             caller_pid;      /* who chan_call_task() is blocking, -1 if none */
    uint32_t        req_len;         /* valid once request_pending */
    int             reply_len;       /* set by chan_serve_reply(); -1 until then */
    bool            request_pending;
    uint8_t        *req_buf;
    uint32_t        req_cap;
    uint8_t        *resp_buf;
    uint32_t        resp_cap;
    bool            in_use;
    /* Claimed for the duration of one call, including the blocking wait
     * inside chan_call_task(). NOT a lock: contention on it must *refuse*
     * (the caller gets -1) rather than wait, because what it protects is a
     * single-slot buffer whose second writer would corrupt the first's
     * in-flight request. See chan_call(). */
    bool            busy;
    /* While busy: the sched_context_id() of the claimant, so a context that
     * re-enters its own endpoint is refused instead of queued behind itself. */
    int             holder;
    /* The waiting tasks, oldest first (chan_call_wait()), and the one a
     * release handed the claim to, until it wakes and takes it (-1: none). */
    int16_t         wq[CHAN_WAITQ_LEN];
    uint8_t         wq_head;
    uint8_t         wq_len;
    int             handed_to;
    /* Guards `busy`, `holder`, the queue and `handed_to`, and nothing else --
     * a handful of instructions that never block. */
    spinlock_t      lock;
};

static chan_endpoint_t g_endpoints[CHAN_MAX_ENDPOINTS];
/* accepted: endpoints are registered at boot and by selftests; a registration from two
 * terminals at once could take one slot. */
static uint32_t        g_num_endpoints;

/* The anti-cycle guard lives in kernel/lock.c now : one wait-for graph fed by every
 * blocking primitive, rather than a channel-only one here.
 *
 * This file kept its own array and walked it with no lock, which made
 * §0.3's cycle -- a task holding a lock, calling a task that wants that lock
 * -- invisible, because the other half of it was a lock edge and this graph
 * had never heard of locks. The rule it enforces is unchanged and is still
 * the one stated when it was written: task A can call B, or be called by B,
 * never both, and no chains either. What changed is what counts as a wait.
 *
 * See waitfor_enter() in kernel/lock.h, including why testing and adding are
 * one call. */

int chan_register(const char *name, chan_handler_fn handler, void *ctx,
                  uint8_t *req_buf, uint32_t req_cap,
                  uint8_t *resp_buf, uint32_t resp_cap) {
    if (!name || !handler || !req_buf || !resp_buf || req_cap == 0 || resp_cap == 0) {
        return -1;
    }
    if (chan_lookup(name)) {
        printk("[Chan] Duplicate endpoint name '%s' ignored\n", name);
        return -1;
    }
    if (g_num_endpoints >= CHAN_MAX_ENDPOINTS) {
        printk("[Chan] Endpoint table full, dropping '%s'\n", name);
        return -1;
    }

    chan_endpoint_t *ep = &g_endpoints[g_num_endpoints++];
    ep->name     = name;
    ep->handler  = handler;
    ep->ctx      = ctx;
    ep->owner_pid = -1;
    ep->req_buf  = req_buf;
    ep->req_cap  = req_cap;
    ep->resp_buf = resp_buf;
    ep->resp_cap = resp_cap;
    ep->in_use   = true;
    ep->busy     = false;
    ep->holder   = -1;
    ep->wq_head  = 0;
    ep->wq_len   = 0;
    ep->handed_to = -1;
    spinlock_init(&ep->lock);
    return 0;
}

int chan_register_task(const char *name, int owner_pid,
                       uint8_t *req_buf, uint32_t req_cap,
                       uint8_t *resp_buf, uint32_t resp_cap) {
    if (!name || owner_pid < 0 || !req_buf || !resp_buf || req_cap == 0 || resp_cap == 0) {
        return -1;
    }
    if (sched_task_state(owner_pid) == TASK_UNUSED) return -1;
    if (chan_lookup(name)) {
        printk("[Chan] Duplicate endpoint name '%s' ignored\n", name);
        return -1;
    }
    if (g_num_endpoints >= CHAN_MAX_ENDPOINTS) {
        printk("[Chan] Endpoint table full, dropping '%s'\n", name);
        return -1;
    }

    chan_endpoint_t *ep = &g_endpoints[g_num_endpoints++];
    ep->name     = name;
    ep->handler  = NULL;
    ep->ctx      = NULL;
    ep->owner_pid = owner_pid;
    ep->caller_pid = -1;
    ep->req_len = 0;
    ep->reply_len = -1;
    ep->request_pending = false;
    ep->req_buf  = req_buf;
    ep->req_cap  = req_cap;
    ep->resp_buf = resp_buf;
    ep->resp_cap = resp_cap;
    ep->in_use   = true;
    ep->busy     = false;
    ep->holder   = -1;
    ep->wq_head  = 0;
    ep->wq_len   = 0;
    ep->handed_to = -1;
    spinlock_init(&ep->lock);
    return 0;
}

chan_endpoint_t *chan_lookup(const char *name) {
    if (!name) return NULL;
    for (uint32_t i = 0; i < g_num_endpoints; i++) {
        if (g_endpoints[i].in_use && strcmp(g_endpoints[i].name, name) == 0) {
            return &g_endpoints[i];
        }
    }
    return NULL;
}

/* --- Claiming an endpoint: the busy flag and its FIFO of waiters --------- */

/* Both under ep->lock. */
static void wq_push(chan_endpoint_t *ep, int pid) {
    ep->wq[(ep->wq_head + ep->wq_len) % CHAN_WAITQ_LEN] = (int16_t)pid;
    ep->wq_len++;
}

static void wq_remove(chan_endpoint_t *ep, int pid) {
    uint8_t kept = 0;
    for (uint8_t i = 0; i < ep->wq_len; i++) {
        int16_t w = ep->wq[(ep->wq_head + i) % CHAN_WAITQ_LEN];
        if (w != pid) ep->wq[(ep->wq_head + kept++) % CHAN_WAITQ_LEN] = w;
    }
    ep->wq_len = kept;
}

/* Gives the claim up: to the oldest waiter still alive, which keeps the
 * endpoint busy across the hand-off, or back to free. Under ep->lock; returns
 * the pid to wake once the lock is dropped, or -1. */
static int release_locked(chan_endpoint_t *ep) {
    while (ep->wq_len > 0) {
        int pid = ep->wq[ep->wq_head];
        ep->wq_head = (uint8_t)((ep->wq_head + 1u) % CHAN_WAITQ_LEN);
        ep->wq_len--;
        int st = sched_task_state(pid);
        if (st == TASK_UNUSED || st == TASK_DEAD) continue;
        ep->handed_to = pid;
        ep->holder = pid;
        return pid;
    }
    ep->busy = false;
    ep->holder = -1;
    return -1;
}

/* Released under the same lock as the claim, which gives the next claimant
 * release ordering over everything the call wrote. */
static void ep_release(chan_endpoint_t *ep) {
    uintptr_t flags = spin_lock_irqsave(&ep->lock);
    int next = release_locked(ep);
    spin_unlock_irqrestore(&ep->lock, flags);
    if (next >= 0) task_unblock(next);
}

/* Takes the endpoint, waiting up to `wait_ms` for a turn. See "Waiting for a
 * turn" in chan.h for what is refused at once and why.
 *
 * Contention must refuse or wait, never spin: the claim is held across
 * chan_call_task()'s task_block(), so a spinlock_t in its place would be the
 * block-while-holding deadlock kernel/lock.h refuses. The spinlock only makes
 * reading and taking the claim indivisible against the other hart. */
static bool ep_claim(chan_endpoint_t *ep, uint32_t wait_ms) {
    int ctx = sched_context_id();
    uintptr_t flags = spin_lock_irqsave(&ep->lock);
    if (!ep->busy) {
        ep->busy = true;
        ep->holder = ctx;
        spin_unlock_irqrestore(&ep->lock, flags);
        return true;
    }
    int me = sched_current_pid();
    if (wait_ms == 0 || ep->holder == ctx || me < 0 || !sched_is_active() ||
        devirq_in_handler()) {
        spin_unlock_irqrestore(&ep->lock, flags);
        return false;
    }
    bool queued = ep->wq_len < CHAN_WAITQ_LEN;
    if (queued) wq_push(ep, me);
    spin_unlock_irqrestore(&ep->lock, flags);

    uint64_t deadline = time_get_ms() + wait_ms;
    for (;;) {
        /* Prepare, test, block (sched.h): a hand-off between the test and the
         * block leaves a token, so the block returns at once. */
        if (queued) task_prepare_block();
        flags = spin_lock_irqsave(&ep->lock);
        if (queued && ep->handed_to == me) {
            ep->handed_to = -1;
            ep->holder = ctx;
            spin_unlock_irqrestore(&ep->lock, flags);
            task_wait_done();
            return true;
        }
        if (!queued) {
            /* The queue was full: poll, and join it once there is room. A
             * free endpoint means nobody is queued, so taking it overtakes
             * no one. */
            if (!ep->busy) {
                ep->busy = true;
                ep->holder = ctx;
                spin_unlock_irqrestore(&ep->lock, flags);
                return true;
            }
            if (ep->wq_len < CHAN_WAITQ_LEN) { wq_push(ep, me); queued = true; }
        }
        if (time_get_ms() >= deadline) {
            if (queued) wq_remove(ep, me);
            spin_unlock_irqrestore(&ep->lock, flags);
            if (queued) task_wait_done();
            return false;
        }
        spin_unlock_irqrestore(&ep->lock, flags);
        if (queued) task_block_until_ms(deadline);
        else task_sleep_ms(1);
    }
}

/* The task-owned half of chan_call(). Everything from setting up the
 * request through task_block() runs under one continuous irq_save() -- the
 * same requirement UART TX/RX waiters have, and for the same reason:
 * releasing the mask before task_block() actually runs would let a
 * scheduling event land in the gap, where the owner task could be woken
 * (found not yet BLOCKED, a no-op) before the caller ever blocks, losing
 * the wakeup permanently. See drivers/uart_16550.c's uart_flush() for the
 * fuller explanation of why this specific pattern is safe to hold across a
 * context switch (kernel/include/kernel/irq.h's nesting contract) without
 * itself deadlocking anything that needs interrupts enabled to make
 * progress. */
static int chan_call_task(chan_endpoint_t *ep, uint32_t req_len) {
    /* Refuse rather than hang if the owner cannot possibly answer. Does not
     * close the race where the owner dies *after* this check but before it
     * replies -- a known, accepted gap for this milestone, not a promise
     * this covers every case. */
    if (sched_task_state(ep->owner_pid) == TASK_UNUSED ||
        sched_task_state(ep->owner_pid) == TASK_DEAD) {
        return -1;
    }

    int me = sched_current_pid();

    /* A hart with no task cannot make this call: it would index g_wait_for[]
     * with -1, and the task_block() below has nothing to block. Refusing is
     * the right answer rather than a special case -- every caller of
     * chan_call() in this tree already handles -1 by falling back to direct
     * access, which is exactly what a bring-up hart should do. Before the
     * identity fix this hart reported pid 0 and would have written the boot
     * task's wait edge and blocked it. */
    if (me < 0) return -1;

    /* Refuse rather than deadlock if this call would close a cycle in the
     * wait-for graph -- see g_wait_for's comment above. Caught here, before
     * anything is written to the endpoint, so a rejected call leaves no
     * state for the next attempt to trip over. */
    /* Tested and recorded in one step, so the answer cannot go stale between
     * the two -- see waitfor_enter(). A refusal leaves no edge behind, which
     * is why there is no waitfor_leave() on this path. */
    if (!waitfor_enter(me, ep->owner_pid)) {
        printk("[Chan] Refusing '%s': task %d -> %d would close a circular wait\n",
              ep->name, me, ep->owner_pid);
        return -1;
    }

    /* Prepared before the request is visible: on two harts the owner can
     * serve it and reply before this task reaches task_block(), and that
     * reply's wake has to leave a token rather than vanish (sched.h). The
     * loop is the other half -- the reply, not the wake, ends the wait
     * (chan_serve_reply() and chan_owner_exited() both clear caller_pid). */
    task_prepare_block();
    uintptr_t flags = irq_save();
    ep->req_len = req_len;
    ep->caller_pid = me;
    ep->reply_len = -1;
    ep->request_pending = true;
    task_unblock(ep->owner_pid); /* if the owner is not yet waiting, it sees the request when it next asks */
    for (;;) {
        task_block();
        if (ep->caller_pid != me) break;
        task_prepare_block();
        if (ep->caller_pid != me) break;
    }
    task_wait_done();
    irq_restore(flags);
    waitfor_leave(me);

    return ep->reply_len;
}

/* One call, copying in and out either with memcpy() or -- `user` -- from and
 * to the calling U-mode task's own memory, validated against its domain. */
static int chan_call_impl(chan_endpoint_t *ep, const void *req, uintptr_t ureq,
                          uint32_t req_len, void *resp, uintptr_t uresp,
                          uint32_t resp_max, bool user, uint32_t wait_ms) {
    if (!ep || !ep->in_use || (!user && !req) || req_len == 0) return -1;
    if (req_len > ep->req_cap) return -1;

    /* This call blocks until the
     * owner replies, so a spinlock_t held across it is the cycle §0.3
     * describes -- the owner may need the very lock the caller is holding.
     *
     * **Refused**, where task_block() only reports, and the difference is
     * that refusal is already this function's contract: every caller in the
     * tree handles -1 by falling back to direct hardware access, which is
     * exactly the right thing to do from a context that should not have been
     * here. A ylock held across this is legal and is not checked -- see
     * kernel/lock.h. */
    if (lock_check_may_block("called chan_call()")) return -1;

    /* Re-entrancy: the endpoint's buffers are single-slot, so an inner call
     * would overwrite the outer call's request mid-flight. See the header --
     * a recursive local 9P mount is the concrete way to reach this, and why
     * ep_claim() refuses its own holder rather than queueing it. */
    if (!ep_claim(ep, wait_ms)) return -1;

    /* Copy IN. The handler must never see the caller's pointer -- that is
     * Rule 1, and it is what keeps this identical to the MMU case (where the
     * caller's address is meaningless here) and to the remote case (where it
     * is on another machine entirely). */
    if (user) {
        if (copy_from_user(ep->req_buf, ureq, req_len) < 0) {
            ep_release(ep);
            return -1;
        }
    } else {
        memcpy(ep->req_buf, req, req_len);
    }

    int resp_len = ep->owner_pid >= 0
        ? chan_call_task(ep, req_len)
        : ep->handler(ep->ctx, ep->req_buf, req_len, ep->resp_buf, ep->resp_cap);

    /* The response is copied out while the endpoint is still claimed (phase
     * 40 review). It used to be released first and copied after, which on two
     * harts let a second caller claim the endpoint and have the reply buffer
     * overwritten before this one had read it.
     *
     * A truncated response is a failure, not a short read: every protocol
     * carried over a channel so far is message-oriented (9P frames), where
     * half a message is not a partial success. */
    int ret = resp_len;
    if (resp_len < 0 || (uint32_t)resp_len > ep->resp_cap ||   /* handler overran its contract */
        (uint32_t)resp_len > resp_max) {
        ret = -1;
    } else if (resp_len > 0) {
        if (user) {
            if (copy_to_user(uresp, ep->resp_buf, (uint32_t)resp_len) < 0) ret = -1;
        } else if (resp) {
            memcpy(resp, ep->resp_buf, (uint32_t)resp_len);
        }
    }

    ep_release(ep);
    return ret;
}

int chan_call(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
              uint8_t *resp, uint32_t resp_max) {
    return chan_call_impl(ep, req, 0, req_len, resp, 0, resp_max, false, 0);
}

int chan_call_wait(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
                   uint8_t *resp, uint32_t resp_max, uint32_t wait_ms) {
    return chan_call_impl(ep, req, 0, req_len, resp, 0, resp_max, false, wait_ms);
}

int chan_call_user(chan_endpoint_t *ep, uintptr_t ureq, uint32_t req_len,
                   uintptr_t uresp, uint32_t resp_max) {
    return chan_call_impl(ep, NULL, ureq, req_len, NULL, uresp, resp_max, true, CHAN_WAIT_MS);
}

uint32_t chan_serve_wait(chan_endpoint_t *ep) {
    if (ep->request_pending) return ep->req_len;
    /* Prepare, test, block, until a request is there: the same lost wake as
     * in chan_call_task(), from the caller's side (sched.h). */
    uintptr_t flags = irq_save();
    for (;;) {
        task_prepare_block();
        if (ep->request_pending) break;
        task_block();
    }
    task_wait_done();
    irq_restore(flags);
    return ep->req_len;
}

void chan_serve_reply(chan_endpoint_t *ep, uint32_t resp_len) {
    ep->reply_len = (int)resp_len;
    ep->request_pending = false;
    int caller = ep->caller_pid;
    ep->caller_pid = -1;
    if (caller >= 0) task_unblock(caller);
}

void chan_owner_exited(int owner_pid) {
    for (uint32_t i = 0; i < g_num_endpoints; i++) {
        chan_endpoint_t *ep = &g_endpoints[i];
        if (!ep->in_use) continue;
        /* As a waiter: out of the queue; a turn handed to it, or a claim it
         * died holding (a fault inside an inline handler), goes to the next. */
        uintptr_t flags = spin_lock_irqsave(&ep->lock);
        wq_remove(ep, owner_pid);
        int next = -1;
        if (ep->busy && ep->holder == owner_pid) {
            ep->handed_to = -1;
            next = release_locked(ep);
        }
        spin_unlock_irqrestore(&ep->lock, flags);
        if (next >= 0) task_unblock(next);

        if (ep->in_use && ep->owner_pid == owner_pid && ep->request_pending) {
            /* Same shape as chan_serve_reply(), just with a failure result
             * instead of whatever the owner would have produced -- there
             * is no answer to hand back, the owner is gone. */
            ep->reply_len = -1;
            ep->request_pending = false;
            int caller = ep->caller_pid;
            ep->caller_pid = -1;
            if (caller >= 0) task_unblock(caller);
        }
    }
}

uint32_t chan_serve_wait_copy(chan_endpoint_t *ep, uint8_t *out, uint32_t out_max) {
    uint32_t len = chan_serve_wait(ep);
    uint32_t n = len < out_max ? len : out_max;
    if (n && out) memcpy(out, ep->req_buf, n);
    return len;
}

void chan_serve_reply_copy(chan_endpoint_t *ep, const uint8_t *in, uint32_t resp_len) {
    uint32_t n = resp_len < ep->resp_cap ? resp_len : ep->resp_cap;
    if (n && in) memcpy(ep->resp_buf, in, n);
    chan_serve_reply(ep, n);
}

uint32_t chan_serve_wait_user(chan_endpoint_t *ep, uintptr_t uout, uint32_t out_max, int *err) {
    uint32_t len = chan_serve_wait(ep);
    *err = 0;
    if (len > out_max) return len;                 /* the caller refuses a truncation */
    if (len && copy_to_user(uout, ep->req_buf, len) < 0) *err = -1;
    return len;
}

int chan_serve_reply_user(chan_endpoint_t *ep, uintptr_t uin, uint32_t resp_len) {
    if (resp_len > ep->resp_cap) return -1;
    if (resp_len && copy_from_user(ep->resp_buf, uin, resp_len) < 0) return -1;
    chan_serve_reply(ep, resp_len);
    return 0;
}

bool chan_info(uint32_t index, const char **name_out, bool *busy_out) {
    if (index >= g_num_endpoints || !g_endpoints[index].in_use) return false;
    if (name_out) *name_out = g_endpoints[index].name;
    if (busy_out) *busy_out = g_endpoints[index].busy;
    return true;
}

bool chan_endpoint_busy(chan_endpoint_t *ep) {
    return ep && ep->in_use && ep->busy;
}
