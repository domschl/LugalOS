/* `chanqtest`: chan_call_wait()'s queue under real tasks (kernel/chan.h,
 * "Waiting for a turn").
 *
 * One inline endpoint, "chanq", whose handler logs who called and then holds
 * the endpoint for the time the request asks, by sleeping: the claim stays
 * taken for the whole handler, so the other tasks meet a busy endpoint for
 * exactly as long as the test wants. Request: tag, hold ms, re-enter flag. */
#include "kernel/chan.h"
#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/time.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHANQ_LOG_MAX 64u

/* accepted: one selftest run at a time (a shell command); the tasks it starts
 * write the log only through the endpoint's handler, which runs under the
 * endpoint's claim. */
static uint8_t          g_req[4];
static uint8_t          g_resp[4];
static chan_endpoint_t *g_ep;
static uint8_t          g_log[CHANQ_LOG_MAX];
static uint32_t         g_log_len;
static int              g_reenter_rc;

static int chanq_handler(void *ctx, const uint8_t *req, uint32_t req_len,
                         uint8_t *resp, uint32_t resp_max) {
    (void)ctx; (void)resp_max;
    if (req_len < 3) return -1;
    if (g_log_len < CHANQ_LOG_MAX) g_log[g_log_len++] = req[0];
    if (req[2]) {
        uint8_t q[3] = { 0xEE, 0, 0 }, r[1];
        g_reenter_rc = chan_call_wait(g_ep, q, sizeof q, r, sizeof r, 100u);
    }
    if (req[1]) task_sleep_ms(req[1]);
    resp[0] = req[0];
    return 1;
}

static int call(uint8_t tag, uint8_t hold_ms, uint32_t wait_ms) {
    uint8_t q[3] = { tag, hold_ms, 0 }, r[1];
    int n = chan_call_wait(g_ep, q, sizeof q, r, sizeof r, wait_ms);
    return (n == 1 && r[0] == tag) ? 0 : -1;
}

/* A task's job, passed as its argument. */
typedef struct {
    uint8_t  tag;
    uint8_t  hold_ms;
    uint8_t  count;       /* calls, back to back */
    uint32_t wait_ms;
    uint32_t start_ms;    /* sleep this long first */
    volatile int  failures;
    volatile bool done;
} chanq_job_t;

static void chanq_task(void *arg) {
    chanq_job_t *j = (chanq_job_t *)arg;
    if (j->start_ms) task_sleep_ms(j->start_ms);
    for (uint8_t i = 0; i < j->count; i++)
        if (call(j->tag, j->hold_ms, j->wait_ms) < 0) j->failures++;
    j->done = true;
}

/* accepted: one selftest run at a time. Static, not on the shell's stack: a
 * check that gives up waiting must not leave its tasks pointing into a frame
 * that has gone. */
static chanq_job_t g_jobs[4];

static bool wait_jobs(chanq_job_t *jobs, int n, uint32_t limit_ms) {
    uint64_t end = time_get_ms() + limit_ms;
    for (;;) {
        bool all = true;
        for (int i = 0; i < n; i++) all = all && jobs[i].done;
        if (all) return true;
        if (time_get_ms() >= end) return false;
        task_sleep_ms(2);
    }
}

static int g_checks, g_fails;   /* accepted: one selftest run at a time */

static void check(const char *what, bool ok) {
    cprintf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    g_checks++;
    if (!ok) g_fails++;
}

static int index_of(uint8_t tag, uint32_t from) {
    for (uint32_t i = from; i < g_log_len; i++) if (g_log[i] == tag) return (int)i;
    return -1;
}

int chan_selftest(void) {
    g_checks = g_fails = 0;
    cprintf("chan_call_wait() queue selftest:\n");
    if (!g_ep) {
        if (chan_register("chanq", chanq_handler, NULL, g_req, sizeof g_req,
                          g_resp, sizeof g_resp) != 0 && !chan_lookup("chanq")) {
            cprintf("CHANQ_SELFTEST_FAIL (no endpoint)\n");
            return -1;
        }
        g_ep = chan_lookup("chanq");
    }

    /* 1. Three waiters arriving in order are served in order. */
    {
        g_log_len = 0;
        chanq_job_t *j = g_jobs;
        j[0] = (chanq_job_t){ .tag = 'H', .hold_ms = 40, .count = 1, .wait_ms = 500 };
        j[1] = (chanq_job_t){ .tag = 'A', .hold_ms = 1,  .count = 1, .wait_ms = 500, .start_ms = 10 };
        j[2] = (chanq_job_t){ .tag = 'B', .hold_ms = 1,  .count = 1, .wait_ms = 500, .start_ms = 15 };
        j[3] = (chanq_job_t){ .tag = 'C', .hold_ms = 1,  .count = 1, .wait_ms = 500, .start_ms = 20 };
        int started = 0;
        for (int i = 0; i < 4; i++) started += task_create("chanq", chanq_task, &j[i]) >= 0;
        bool fin = started == 4 && wait_jobs(j, 4, 2000);
        check("FIFO: three waiters behind a holder are served in arrival order",
              fin && j[1].failures + j[2].failures + j[3].failures == 0 && g_log_len == 4 &&
              g_log[0] == 'H' && g_log[1] == 'A' && g_log[2] == 'B' && g_log[3] == 'C');
    }

    /* 2. A client calling back to back cannot keep the endpoint from one
     * that waits: the waiter's turn comes after the hog's current call. */
    {
        g_log_len = 0;
        chanq_job_t *j = g_jobs;
        j[0] = (chanq_job_t){ .tag = 'G', .hold_ms = 3, .count = 30, .wait_ms = 500 };
        j[1] = (chanq_job_t){ .tag = 'W', .hold_ms = 1, .count = 1,  .wait_ms = 30, .start_ms = 20 };
        int started = 0;
        for (int i = 0; i < 2; i++) started += task_create("chanq", chanq_task, &j[i]) >= 0;
        bool fin = started == 2 && wait_jobs(j, 2, 3000);
        int w = index_of('W', 0);
        /* The hog's calls take 3 ms each: a 30 ms bound would expire behind
         * it unless the hand-off gives the waiter the next turn. */
        check("hand-off: a back-to-back client does not starve a waiter",
              fin && j[0].failures == 0 && j[1].failures == 0 && w > 0 && w < 30);
    }

    /* 3. chan_call() still refuses a busy endpoint at once; a wait ends at
     * its deadline and leaves the queue as it found it. */
    {
        g_log_len = 0;
        chanq_job_t *h = g_jobs;
        *h = (chanq_job_t){ .tag = 'H', .hold_ms = 60, .count = 1, .wait_ms = 500 };
        bool started = task_create("chanq", chanq_task, h) >= 0;
        task_sleep_ms(10);
        uint8_t q[3] = { 'X', 0, 0 }, r[1];
        uint64_t t0 = time_get_ms();
        int plain = chan_call(g_ep, q, sizeof q, r, sizeof r);
        uint64_t t1 = time_get_ms();
        int timed = call('T', 0, 15);
        uint64_t t2 = time_get_ms();
        bool fin = started && wait_jobs(h, 1, 1000);
        check("chan_call() on a busy endpoint: refused at once",
              started && plain < 0 && t1 - t0 < 5);
        check("chan_call_wait() deadline: -1 after its bound, before the holder is done",
              timed < 0 && t2 - t1 >= 15 && t2 - t1 < 50);
        check("after a timed-out wait the endpoint is free again",
              fin && !chan_endpoint_busy(g_ep) && call('Z', 0, 0) == 0);
    }

    /* 4. A context re-entering the endpoint it holds is refused, not queued
     * behind itself (the recursive local 9P mount). */
    {
        g_reenter_rc = 0;
        uint8_t q[3] = { 'R', 0, 1 }, r[1];
        uint64_t t0 = time_get_ms();
        int n = chan_call_wait(g_ep, q, sizeof q, r, sizeof r, 100u);
        uint64_t t1 = time_get_ms();
        check("re-entry from the holder: refused at once, outer call served",
              n == 1 && g_reenter_rc < 0 && t1 - t0 < 50);
    }

    if (g_fails == 0) cprintf("CHANQ_SELFTEST_OK (%d/%d)\n", g_checks, g_checks);
    else cprintf("CHANQ_SELFTEST_FAIL (%d of %d failed)\n", g_fails, g_checks);
    return g_fails == 0 ? 0 : -1;
}
