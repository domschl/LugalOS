#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/irq.h"
#include "kernel/lock.h"
#include <string.h>

/* See kernel/include/kernel/chan.h for the rationale, especially why the
 * two copies below are deliberate rather than wasteful, and for the
 * task-owned endpoint design (M4). */

struct chan_endpoint {
    const char     *name;
    /* Inline-handler endpoints (B1: p9, console): handler != NULL. */
    chan_handler_fn handler;
    void           *ctx;
    /* Task-owned endpoints (M4): owner_pid >= 0, handler == NULL. */
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
    /* Guards the check-and-set of `busy` above, and nothing else -- a
     * handful of instructions that never block (S3,
     * plan/phase22_smp_locking_foundation.md). */
    spinlock_t      lock;
};

static chan_endpoint_t g_endpoints[CHAN_MAX_ENDPOINTS];
static uint32_t        g_num_endpoints;

/* Anti-cycle guard: a wait-for graph over task-owned endpoint calls.
 * g_wait_for[pid] is the owner_pid that task `pid` is currently blocked
 * inside chan_call_task() waiting on, or -1 if it is not calling anyone.
 * chan_call_task() adds the edge caller->target for the duration of the
 * block; a call is refused before the edge is added if target can already
 * (transitively) reach caller, which is exactly "task A can call B, or be
 * called by B, never both -- and no longer chains either" (the rule this
 * exists to enforce structurally, not just by convention, before any
 * further driver gets converted into a task). */
static int  g_wait_for[MAX_TASKS];
static bool g_wait_for_ready;

static void wait_for_init(void) {
    if (g_wait_for_ready) return;
    for (int i = 0; i < MAX_TASKS; i++) g_wait_for[i] = -1;
    g_wait_for_ready = true;
}

static bool would_cycle(int caller_pid, int target_pid) {
    int cur = target_pid;
    for (int guard = 0; cur >= 0 && cur < MAX_TASKS && guard < MAX_TASKS; guard++) {
        if (cur == caller_pid) return true;
        cur = g_wait_for[cur];
    }
    return false;
}

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

/* The task-owned half of chan_call(). Everything from setting up the
 * request through task_block() runs under one continuous irq_save() -- the
 * same requirement M2's UART TX/RX waiters have, and for the same reason:
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

    wait_for_init();
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
    if (would_cycle(me, ep->owner_pid)) {
        printk("[Chan] Refusing '%s': task %d -> %d would close a circular wait\n",
              ep->name, me, ep->owner_pid);
        return -1;
    }

    uintptr_t flags = irq_save();
    ep->req_len = req_len;
    ep->caller_pid = me;
    ep->reply_len = -1;
    ep->request_pending = true;
    g_wait_for[me] = ep->owner_pid;
    task_unblock(ep->owner_pid); /* no-op if the owner is not yet waiting; it will see the request when it next asks */
    task_block();
    g_wait_for[me] = -1;
    irq_restore(flags);

    return ep->reply_len;
}

int chan_call(chan_endpoint_t *ep, const uint8_t *req, uint32_t req_len,
              uint8_t *resp, uint32_t resp_max) {
    if (!ep || !ep->in_use || !req || req_len == 0) return -1;
    if (req_len > ep->req_cap) return -1;

    /* Re-entrancy: the endpoint's buffers are single-slot, so an inner call
     * would overwrite the outer call's request mid-flight. See the header --
     * a recursive local 9P mount is the concrete way to reach this.
     *
     * Check-then-set, so it must be atomic against a timer tick landing
     * between the two (M3 preemption): without the mask, two callers whose
     * check-and-set straddle a preemption both see busy == false and both
     * proceed, and their memcpy()s below race on the same ep->req_buf. */
    /* S3: the lock covers these three lines and stops. It is deliberately
     * NOT held for the duration of the call, and the reason is exactly the
     * distinction kernel/lock.h draws between its two types:
     *
     *   - `busy` is held across chan_call_task(), which calls task_block().
     *     A spinlock_t held across a block is the deadlock its own header
     *     warns about: the holder cannot run to release it.
     *   - Contention here must *refuse*, not wait. A caller that finds the
     *     endpoint busy gets -1 and decides for itself; spinning or
     *     yielding until it frees would change chan_call()'s contract, and
     *     on the re-entrant path (a recursive local 9P mount) the waiter
     *     would be waiting on itself.
     *
     * So the claim stays a plain flag with a long lifetime, and the lock
     * only makes reading-and-taking that flag indivisible. Two different
     * jobs; conflating them is how this goes wrong. irq_save() alone was
     * never going to stop a second hart arriving here at the same instant,
     * which is what changes now. */
    uintptr_t flags = spin_lock_irqsave(&ep->lock);
    if (ep->busy) { spin_unlock_irqrestore(&ep->lock, flags); return -1; }
    ep->busy = true;
    spin_unlock_irqrestore(&ep->lock, flags);

    /* Copy IN. The handler must never see the caller's pointer -- that is
     * Rule 1, and it is what keeps this identical to the MMU case (where the
     * caller's address is meaningless here) and to the remote case (where it
     * is on another machine entirely). */
    memcpy(ep->req_buf, req, req_len);

    int resp_len = ep->owner_pid >= 0
        ? chan_call_task(ep, req_len)
        : ep->handler(ep->ctx, ep->req_buf, req_len, ep->resp_buf, ep->resp_cap);

    /* Released under the same lock as the claim. The store is a single byte
     * and would not tear, but going through the lock is what gives the next
     * hart to take this endpoint release ordering over everything the call
     * above wrote into its buffers. */
    flags = spin_lock_irqsave(&ep->lock);
    ep->busy = false;
    spin_unlock_irqrestore(&ep->lock, flags);

    if (resp_len < 0) return -1;
    if ((uint32_t)resp_len > ep->resp_cap) return -1; /* handler overran its contract */

    /* Copy OUT, bounded by what the caller can actually accept. A truncated
     * response is a failure, not a short read: every protocol carried over a
     * channel so far is message-oriented (9P frames), where half a message is
     * not a partial success. */
    if ((uint32_t)resp_len > resp_max) return -1;
    if (resp && resp_len > 0) memcpy(resp, ep->resp_buf, (uint32_t)resp_len);
    return resp_len;
}

uint32_t chan_serve_wait(chan_endpoint_t *ep) {
    if (ep->request_pending) return ep->req_len;
    uintptr_t flags = irq_save();
    if (!ep->request_pending) {
        task_block();
    }
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

bool chan_info(uint32_t index, const char **name_out, bool *busy_out) {
    if (index >= g_num_endpoints || !g_endpoints[index].in_use) return false;
    if (name_out) *name_out = g_endpoints[index].name;
    if (busy_out) *busy_out = g_endpoints[index].busy;
    return true;
}

bool chan_endpoint_busy(chan_endpoint_t *ep) {
    return ep && ep->in_use && ep->busy;
}
