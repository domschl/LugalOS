/* The service host (kernel/include/kernel/svchost.h). */

#include "kernel/svchost.h"
#include "kernel/lock.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <stddef.h>

/* guarded by: g_lock; svc_register() appends from any task, the host reads. A slot, once
 * filled, never changes, so the host may use a job outside the lock. */
static spinlock_t g_lock = SPINLOCK_INIT;
static svc_job_t *g_jobs[SVC_MAX_JOBS];
static uint32_t g_count;
static bool g_host_up;

static uint32_t jobs_snapshot(svc_job_t **out) {
    uintptr_t f = spin_lock_irqsave(&g_lock);
    uint32_t n = g_count;
    for (uint32_t i = 0; i < n; i++) out[i] = g_jobs[i];
    spin_unlock_irqrestore(&g_lock, f);
    return n;
}

static void svc_task(void *arg) {
    (void)arg;
    for (;;) {
        svc_job_t *jobs[SVC_MAX_JOBS];
        uint32_t n = jobs_snapshot(jobs);
        uint64_t now = time_get_ms();
        uint64_t wake = now + 1000u;
        for (uint32_t i = 0; i < n; i++) {
            svc_job_t *j = jobs[i];
            if (!j->started) {
                if (j->init) j->init();
                j->started = true;
                j->next_ms = time_get_ms() + j->period_ms;
            } else if (now >= j->next_ms) {
                uint64_t t0 = time_get_us();
                j->step(now);
                uint64_t took = time_get_us() - t0;
                if (took > j->max_us) j->max_us = took > UINT32_MAX ? UINT32_MAX : (uint32_t)took;
                j->runs++;
                /* From the planned time, so a period holds on average; a step that overran
                 * more than a whole period does not run twice to catch up. */
                j->next_ms += j->period_ms;
                if (j->next_ms <= now) j->next_ms = now + j->period_ms;
            }
            if (j->next_ms < wake) wake = j->next_ms;
        }
        now = time_get_ms();
        if (wake > now) task_sleep_ms((uint32_t)(wake - now));
    }
}

bool svc_register(svc_job_t *job) {
    job->started = false;
    job->runs = 0;
    job->max_us = 0;
    uintptr_t f = spin_lock_irqsave(&g_lock);
    bool start = !g_host_up;
    bool ok = g_count < SVC_MAX_JOBS;
    if (ok) {
        g_jobs[g_count++] = job;
        g_host_up = true;
    }
    spin_unlock_irqrestore(&g_lock, f);
    if (!ok) {
        printk("[svc] no room for %s (SVC_MAX_JOBS)\n", job->name);
        return false;
    }
    if (start && task_create_sized("svchost", svc_task, NULL, SVC_STACK_PAGES) < 0) {
        printk("[svc] could not start the host: %s does not run\n", job->name);
        f = spin_lock_irqsave(&g_lock);
        g_count = 0;
        g_host_up = false;
        spin_unlock_irqrestore(&g_lock, f);
        return false;
    }
    return true;
}

void svc_report(void) {
    svc_job_t *jobs[SVC_MAX_JOBS];
    uint32_t n = jobs_snapshot(jobs);
    if (!n) { cprintf("svc: no jobs\n"); return; }
    cprintf("svc: %lu job%s on one task, %u pages of stack\n", (unsigned long)n, n == 1 ? "" : "s",
            (unsigned)SVC_STACK_PAGES);
    for (uint32_t i = 0; i < n; i++)
        cprintf("  %-10s every %5lu ms  %8lu runs  longest %lu us\n", jobs[i]->name,
                (unsigned long)jobs[i]->period_ms, (unsigned long)jobs[i]->runs,
                (unsigned long)jobs[i]->max_us);
}
