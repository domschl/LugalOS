/* The kernel log's consumer (Y5c, plan/phase31_concurrency_hierarchy.md).
 *
 * Y5b made a log record atomic to store. This is the half that makes the
 * *producer* free: once this task is registered, klog_emit() appends a record
 * and returns, and everything that can block -- the UART write, the batch
 * lock, the chan_call() to the uart task -- happens here instead.
 *
 * ## The one asymmetry the whole design rests on
 *
 * **The consumer may block; the producer may not.** This task takes
 * console_lock() around its fan-out and will happily wait behind cprintf();
 * that is harmless precisely because nothing is waiting on this task. A
 * producer that waited would be a cycle, which is what phase 31 exists to
 * prevent and what §0.4 measured the cost of.
 *
 * ## Why a wake rather than a poll
 *
 * task_unblock() is a non-blocking signal, which is the shape the user named
 * when this phase reopened the question of new primitives: send and continue,
 * no edge in the wait-for graph, so it cannot close a cycle.
 *
 * It has one restriction that matters here and is honoured in klog_emit():
 * task_unblock() takes g_sched_lock, so a producer that already holds a
 * spinlock must not call it -- a nested spinlock is exactly what Y2's leaf
 * check refuses. Such a printk() therefore arrives with no wake, and the
 * bounded sleep below is what still gets it out. That is the only reason this
 * task sleeps on a deadline rather than blocking outright.
 */

#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Long enough that an idle system is not waking a task pointlessly -- the
 * rp2350-clock persona is this tree's canary for CPU and bus hogs -- and short
 * enough to bound the one case with no wake behind it: a printk() issued while
 * a spinlock is held. Those are rare and diagnostic; 50 ms late is invisible
 * to a human reading a console and irrelevant to a log being read afterwards. */
#define KLOGD_IDLE_MS 50

/* One page. The body formats nothing and recurses nowhere: it copies bytes
 * from the ring to the sinks. */
#define KLOGD_STACK_PAGES 1

static int g_klogd_pid = -1;

static void klogd_body(void *arg) {
    (void)arg;
    for (;;) {
        /* Drains to the current end and reports any bytes that were evicted
         * before we reached them. Takes console_lock() itself, once per pass
         * rather than once per record, so a burst leaves the console in one
         * run instead of interleaved with whatever else is printing. */
        klog_drain();

        /* Nothing left: sleep until a producer wakes us, or the deadline
         * does. Re-checked on wake by the loop, never trusted once. */
        task_sleep_ms(KLOGD_IDLE_MS);
    }
}

int klogd_start(void) {
    if (g_klogd_pid >= 0) return g_klogd_pid;

    int pid = task_create_sized("klogd", klogd_body, NULL, KLOGD_STACK_PAGES);
    if (pid < 0) {
        /* Not fatal, and deliberately not silent: without a consumer the
         * producer keeps draining inline, which is what it did before Y5c and
         * is correct -- just blocking. Saying so beats a board whose logging
         * quietly still has the old failure mode. */
        printk("[klogd] Could not start the log consumer; logging stays synchronous.\n");
        return -1;
    }

    /* TASK_PRIO_NORMAL -- left as task_create_sized() made it, deliberately.
     *
     * TASK_PRIO_INTERRUPT was tried, on the reasoning that a woken consumer
     * should not wait behind a queue. It measurably changed the scheduling of
     * everything else: `taskdemo`'s two tasks stopped interleaving, because a
     * high-priority klogd ran on every sched_yield() and the rotation resumed
     * at the task that had just yielded rather than moving on. A logger that
     * perturbs what it observes is worse than a slightly late one.
     *
     * Latency is not what this task is for anyway. Output a reader is waiting
     * on is drained synchronously at the two points the console is written --
     * cprintf() and the prompt -- so klogd's own turn matters only on a node
     * with nobody typing at it, where nothing is waiting. */

    g_klogd_pid = pid;
    /* Registered last: klog_emit() stops fanning out the moment this is set,
     * so the task it hands the work to has to exist first. */
    klog_set_consumer(pid);
    printk("[klogd] Log consumer running as task #%d; printk() no longer blocks.\n", pid);
    return pid;
}
