/* The driver-as-task serve loop, once. See
 * drivers/include/drivers/driver_task.h for the contract and the three
 * invariants, and plan/phase30_driver_framework.md §2.1 for why this exists
 * at all (nine copies, zero board-specific content). */

#include "drivers/driver_task.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/lock.h"
#include <stddef.h>

/* The task body every driver task now runs. `arg` is the driver_task_t, which
 * is how one function serves any number of drivers -- the nine copies this
 * replaces each closed over their own file-scope globals instead. */
static void driver_task_body(void *arg) {
    driver_task_t *dt = (driver_task_t *)arg;
    if (!dt) return;

    /* driver_task_start() creates the task before the endpoint it serves
     * exists -- chan_register_task() needs the pid first -- so wait for
     * registration rather than risk running before ep is set, which a
     * preemption landing in that narrow window could do. */
    while (!dt->ep) sched_yield();

    for (;;) {
        uint32_t req_len = chan_serve_wait(dt->ep);

        /* Short requests never reach the callback and are not counted: a
         * malformed or empty request is not a call served, and
         * blk_task_call_count()'s meaning depends on that distinction. */
        if (req_len < dt->spec.min_req_len) {
            chan_serve_reply(dt->ep, 0);
            continue;
        }

        dt->calls++;

        /* Invariant 1, checked rather than remembered. The bracket is here
         * and not in each driver precisely because "remember not to printk"
         * is what nine separate comments were already saying. */
        lock_noprintk_enter(dt->spec.name);
        uint32_t resp_len = dt->spec.serve(dt->spec.ctx,
                                           dt->spec.req, req_len,
                                           dt->spec.resp, dt->spec.resp_cap);
        lock_noprintk_leave();

        /* Clamped rather than trusted: a callback that miscounts would
         * otherwise have the endpoint copy past the response buffer into the
         * caller's, and the caller is the one that would be corrupted. */
        if (resp_len > dt->spec.resp_cap) resp_len = dt->spec.resp_cap;
        chan_serve_reply(dt->ep, resp_len);
    }
}

int driver_task_start(driver_task_t *dt, const driver_task_spec_t *spec) {
    if (!dt || !spec || !spec->name || !spec->serve) return -1;
    if (!spec->req || !spec->resp) return -1;

    dt->spec  = *spec;
    dt->ep    = NULL;
    dt->pid   = -1;
    dt->calls = 0;

    uint32_t pages = spec->stack_pages ? spec->stack_pages : 1u;

    /* The pid is needed to register the endpoint, and the endpoint is needed
     * before the body can serve -- hence the handshake at the top of
     * driver_task_body(). dt is the arg, so the body finds all of this. */
    int pid = task_create_driver(spec->name, driver_task_body, dt, pages);
    if (pid < 0) {
        printk("[%s] Could not start the %s task; callers stay on direct hardware access.\n",
               spec->name, spec->name);
        return -1;
    }

    if (spec->priority != DRIVER_PRIO_DEFAULT) {
        task_set_priority(pid, spec->priority);
    }

    if (chan_register_task(spec->name, pid, spec->req, spec->req_cap,
                           spec->resp, spec->resp_cap) != 0) {
        printk("[%s] Could not register the %s channel endpoint; falling back to direct hardware access.\n",
               spec->name, spec->name);
        return -1;
    }

    /* ep last: the body spins on it, so setting it is what releases the task
     * to serve, and everything it needs must already be in place. */
    dt->pid = pid;
    dt->ep  = chan_lookup(spec->name);
    printk("[%s] Driver running as task #%d, reachable via chan_call(\"%s\", ...)\n",
           spec->name, pid, spec->name);
    return pid;
}

bool driver_task_alive(const driver_task_t *dt) {
    if (!dt || dt->pid < 0) return false;
    int st = sched_task_state(dt->pid);
    return st != TASK_UNUSED && st != TASK_DEAD;
}

int driver_task_call(driver_task_t *dt, const uint8_t *req, uint32_t req_len,
                     uint8_t *resp, uint32_t resp_max) {
    if (!dt || !dt->ep) return -1;

    /* Bounded, yielding retry rather than blocking indefinitely or falling
     * straight back to a hardware access that would race the request already
     * in flight. Busy is expected to be rare -- these endpoints have one
     * writer in practice, already serialized by the driver's own batching
     * where there is any. */
    for (int attempt = 0; attempt < 8; attempt++) {
        int n = chan_call(dt->ep, req, req_len, resp, resp_max);
        if (n >= 0) return n;
        sched_yield();
    }
    return -1;
}
