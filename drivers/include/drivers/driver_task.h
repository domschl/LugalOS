#ifndef LUGALOS_DRIVERS_DRIVER_TASK_H
#define LUGALOS_DRIVERS_DRIVER_TASK_H

/* The driver-as-task pattern, once (G2, plan/phase30_driver_framework.md).
 *
 * Nine drivers in this tree run as tasks serving a chan_call() endpoint, and
 * nine of them wrote the same loop:
 *
 *     static void X_task_body(void *arg) {
 *         while (!g_X_ep) sched_yield();
 *         for (;;) {
 *             uint32_t req_len = chan_serve_wait(g_X_ep);
 *             ... switch on g_X_req[0] ...
 *             chan_serve_reply(g_X_ep, resp_len);
 *         }
 *     }
 *
 * The switch is the driver. Everything around it is not, and this is it:
 * the registration handshake, the serve loop, the reply, the liveness check,
 * the bounded retry a caller needs, and the three invariants below.
 *
 * Category C of plan/hardware_seams.md -- zero board-specific content, which
 * is exactly why it is shared and why the register halves are not.
 *
 * ## The three invariants
 *
 * They were a comment repeated in every driver. Here is what each one is
 * now:
 *
 * 1. **printk() freely; never cprintf() from inside the serve callback.**
 *    This inverted, and the half that inverted is worth knowing. printk() is
 *    a ring append that reaches no blocking primitive from any context, so a
 *    serve callback may log as freely as anything else (Y5c,
 *    plan/phase31_concurrency_hierarchy.md). The *console* stream still
 *    reaches the uart task through chan_call(), so a callback writing it
 *    while a caller is blocked on this endpoint closes the cycle -- the
 *    original hazard, in the one place it survives.
 *
 *    **Checked**, not remembered: driver_task.c brackets every callback with
 *    lock_serve_enter()/leave(), and console_lock() reports a named fault if
 *    it is reached from inside one (kernel/lock.h). It fires on the
 *    violation, not on the collision, so it is deterministic on QEMU instead
 *    of being the bug that only ever reproduces on hardware.
 *
 * 2. **Never call back into anything that could chan_call() this same
 *    endpoint.** Refused since Y3: chan_call() will not close a cycle in the
 *    wait-for graph (kernel/lock.h). The callback is handed its request and
 *    its response buffer and needs no facade function to reach either, which
 *    is what made this easy to get wrong before.
 *
 * 3. **Only this task may touch the hardware while it is alive.** Structural
 *    now rather than stated: the framework owns the loop, so the only code
 *    that runs inside it is the serve callback. Facade functions check
 *    driver_task_alive() and fall back to direct access when it is not.
 *
 * ## What this deliberately does not do
 *
 * It carries bytes and does not read them. The wire protocol -- 'H'/'R'/'W'
 * for uart, the block opcodes, the i2c ops -- stays in the driver, because a
 * framework that knew about opcodes would be a second place to change when
 * one is added. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/chan.h"

/* Serves one request. Returns the number of response bytes written, which
 * may be 0; anything larger than resp_cap is clamped by the framework.
 *
 * `req` is the endpoint's own request buffer, already filled, and `resp` its
 * response buffer -- the same two the spec handed over, so a callback that
 * wants to parse in place may. Runs with invariant 1 checked (see above). */
typedef uint32_t (*driver_serve_fn)(void *ctx,
                                    const uint8_t *req, uint32_t req_len,
                                    uint8_t *resp, uint32_t resp_cap);

/* Leave `priority` as this to keep whatever task_create_driver() gave, which
 * is what six of the nine drivers want. The three UARTs raise it to
 * TASK_PRIO_INTERRUPT: a caller that chan_call()s them unblocks the driver
 * and then blocks itself, and at TASK_PRIO_NORMAL the scheduler could put an
 * arbitrary queue between the two halves of what is meant to be a near-
 * instant hardware handoff. */
#define DRIVER_PRIO_DEFAULT (-1)

typedef struct {
    /* Both the task name and the endpoint name -- they have never differed,
     * and a driver that needed them to differ would be worth a second look
     * rather than a second field. */
    const char     *name;

    driver_serve_fn serve;
    void           *ctx;

    /* The buffers registered with the endpoint. chan_call() copies a
     * caller's request straight into `req`, so the serve callback reads its
     * request out of the same array the driver registered. */
    uint8_t        *req;
    uint32_t        req_cap;
    uint8_t        *resp;
    uint32_t        resp_cap;

    /* Requests shorter than this never reach the callback; they get an empty
     * reply, and are not counted as calls served. Every driver had this as
     * its own first line -- `if (req_len < N) { reply(0); continue; }`. */
    uint32_t        min_req_len;

    uint32_t        stack_pages;   /* 0 means 1, which is what all nine use */
    int             priority;      /* DRIVER_PRIO_DEFAULT, or a TASK_PRIO_* */
} driver_task_spec_t;

/* The driver declares one of these static and hands it to
 * driver_task_start(). It holds what the driver used to hold in three
 * separate globals: the endpoint, the pid, and the count of calls served. */
typedef struct {
    driver_task_spec_t spec;
    chan_endpoint_t   *ep;
    int                pid;

    /* Requests handed to the serve callback since boot. M4.5's verification
     * question -- "is the task genuinely serving requests, or did every
     * caller silently fall back to direct access?" -- is answered by this
     * being nonzero and growing, and several tests assert exactly that. */
    uint32_t           calls;
} driver_task_t;

/* Creates the task, registers the endpoint, and returns the pid (or -1).
 *
 * Failure is not fatal by construction: every caller's facade checks
 * driver_task_alive() and falls back to direct hardware access, which is
 * what it did before the driver was a task at all. The framework prints the
 * same "falling back" line the nine drivers each printed. */
int driver_task_start(driver_task_t *dt, const driver_task_spec_t *spec);

/* False before the task starts, after it dies, and for a driver whose start
 * failed -- the one question every facade function asks before choosing
 * between the endpoint and the hardware. */
bool driver_task_alive(const driver_task_t *dt);

static inline uint32_t driver_task_call_count(const driver_task_t *dt) {
    return dt ? dt->calls : 0u;
}

/* chan_call() with a bounded retry on a transient busy (the endpoint's
 * single in-flight slot occupied by someone else), yielding between
 * attempts. Returns the response length, or -1 once the retries are
 * exhausted -- which is a real if unlikely degradation and the only case a
 * caller should fall back to direct access for. A dead or unstarted task is
 * driver_task_alive()'s question, asked before this one. */
int driver_task_call(driver_task_t *dt, const uint8_t *req, uint32_t req_len,
                     uint8_t *resp, uint32_t resp_max);

/* The endpoint itself, for the one caller whose retry policy is a driver
 * fact rather than a framework one: uart_flush() must never give up while
 * another WRITE is actually in flight (it would interleave bytes on the
 * wire), but must fall back promptly when the endpoint is merely busy with a
 * pending READ that is waiting on a human. That distinction is
 * g_uart_write_in_flight's, and a framework that took an opinion on it would
 * be taking one about UARTs. NULL before the task is registered. */
static inline chan_endpoint_t *driver_task_endpoint(const driver_task_t *dt) {
    return dt ? dt->ep : NULL;
}

#endif /* LUGALOS_DRIVERS_DRIVER_TASK_H */
