/* The driver-as-task serve loop, once. See
 * drivers/include/drivers/driver_task.h for the contract and the three
 * invariants, and plan/phase30_driver_framework.md §2.1 for why this exists
 * at all (nine copies, zero board-specific content). */

#include "drivers/driver_task.h"
#include "kernel/sched.h"
#include "kernel/chan.h"
#include "kernel/printk.h"
#include "kernel/lock.h"
#include "kernel/mem_domain.h"
#include "kernel/device.h"
#include "arch/umode.h"
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

        /* Invariant 1, checked rather than remembered. The bracket marks the
         * callback as serving; console_lock() is what asks (kernel/lock.h).
         *
         * It guarded printk() when G2 added it and guards cprintf() now --
         * printk() stopped being able to block in Y5c. The bracket is here
         * and not in each driver for the reason it always was: "remember not
         * to do this" is what nine separate comments were already saying. */
        lock_serve_enter(dt->spec.name);
        uint32_t resp_len = dt->spec.serve(dt->spec.ctx,
                                           dt->spec.req, req_len,
                                           dt->spec.resp, dt->spec.resp_cap);
        lock_serve_leave();

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


/* --- The U-mode domain (G3, plan/phase30_driver_framework.md §2.2) -------- */

/* One per driver that enters U-mode, and they are not concurrent: a task
 * builds its domain once, on its own first pass through its body, and never
 * again. Static rather than on the task's stack because task_set_domain()
 * keeps a pointer to it -- a domain that lived on the stack would be
 * describing memory that had since become something else. */
static mem_domain_t g_umode_domains[DRIVER_UMODE_MAX_DOMAINS];
static uint32_t     g_umode_domain_count;

int driver_umode_enter(const driver_umode_spec_t *spec) {
    if (!spec || !spec->body || !spec->stack_base) return -1;

    if (g_umode_domain_count >= DRIVER_UMODE_MAX_DOMAINS) {
        printk("[%s] Refusing to enter U-mode: no domain slot left; %s\n",
               spec->name ? spec->name : "driver",
               spec->fallback ? spec->fallback : "falling back to direct hardware access.");
        return -1;
    }
    mem_domain_t *dom = &g_umode_domains[g_umode_domain_count++];

    mem_domain_init(dom);
    mem_domain_add(dom, spec->stack_base, spec->stack_size, MEM_R | MEM_W);

    /* The shared .utext page, always. U-mode can execute nothing else, so a
     * driver that omitted this would fault on its first instruction -- which
     * is exactly the kind of thing that should not be each driver's to
     * remember. */
    uintptr_t tbase, tsize;
    if (spec->text_region) spec->text_region(&tbase, &tsize);
    else                   board_text_region(&tbase, &tsize);
    mem_domain_add(dom, tbase, tsize, MEM_R | MEM_X);

    for (uint32_t i = 0; i < spec->region_count && i < DRIVER_UMODE_MAX_REGIONS; i++) {
        mem_domain_add(dom, spec->regions[i].base, spec->regions[i].size,
                       spec->regions[i].perms);
    }

    /* Refuse rather than claim unverified isolation. The driver keeps working
     * -- every facade function falls back to direct hardware access when its
     * task is not serving -- it simply keeps working without the confinement,
     * and says so rather than pretending. */
    if (task_set_domain(sched_current_pid(), dom) != 0) {
        printk("[%s] Refusing to enter U-mode: memory domain not enforceable; %s\n",
               spec->name ? spec->name : "driver",
               spec->fallback ? spec->fallback : "falling back to direct hardware access.");
        g_umode_domain_count--;   /* the slot was never used */
        return -1;
    }

    uintptr_t top = spec->stack_top ? spec->stack_top
                                    : (spec->stack_base + spec->stack_size);
    arch_enter_user(spec->body, top, 0, spec->arg, 0);
    return -1;   /* not reached: arch_enter_user() does not return */
}
